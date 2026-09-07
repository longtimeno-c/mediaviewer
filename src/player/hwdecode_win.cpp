// SPDX-License-Identifier: GPL-2.0-or-later
// PR 5a - D3D11VA hardware decode. Windows port (D9 / plan/15-platforms.md).
//
// OWNER: mediaviewer-48 (5a). This is the ONLY file in player/ permitted to
// include <d3d11.h>: tools/check-hostable-core.ps1 exempts *_win.cpp by name,
// and now also bans <libavutil/hwcontext_d3d11va.h> everywhere else, because
// that header drags d3d11.h in without ever naming it.
// A Metal host replaces this file and nothing else.
#include <d3d11.h>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}

#include <cstring>

#include "core/trace.h"
#include "player/video_internal.h"

namespace mv::player {
namespace {

// The decoder pool hands out (pool texture, array slice) pairs. A D3D11 texture
// array with one mip level numbers its subresources by slice, so the slice
// index IS the subresource index.
[[nodiscard]] UINT subresource_of(const AVFrame* frame) noexcept {
  return static_cast<UINT>(reinterpret_cast<std::uintptr_t>(frame->data[1]));
}

[[nodiscard]] ID3D11Texture2D* pool_texture_of(const AVFrame* frame) noexcept {
  return reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
}

}  // namespace

result<AVBufferRef*> create_hw_device_ctx(ID3D11Device* device) noexcept {
  if (!device) return err(status::invalid_arg);

  AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
  if (!ref) return err(status::out_of_memory);

  auto* hw = reinterpret_cast<AVHWDeviceContext*>(ref->data);
  auto* d3d = static_cast<AVD3D11VADeviceContext*>(hw->hwctx);

  // plan/05: build the hardware device context from OUR ID3D11Device, not one
  // FFmpeg makes, so the decoded surface is usable without a cross-device copy.
  // src/gfx/device.cpp already created it with D3D11_CREATE_DEVICE_VIDEO_SUPPORT
  // and ID3D10Multithread::SetMultithreadProtected(TRUE).
  //
  // FFmpeg takes ownership of this reference and releases it in the context's
  // free callback, so the AddRef here is balanced by av_buffer_unref.
  device->AddRef();
  d3d->device = device;

  // device_context, video_device and video_context are left null on purpose:
  // av_hwdevice_ctx_init fills them from the device's IMMEDIATE context and
  // installs the default lock/unlock pair over ID3D10Multithread. That
  // immediate context is precisely the one copy_hw_surface must use — see the
  // ordering note there.
  const int rc = av_hwdevice_ctx_init(ref);
  if (rc < 0) {
    av_buffer_unref(&ref);
    return err(status::unsupported_format);
  }

  MV_LOG_INFO("player: D3D11VA hardware device context built on the shell's device");
  return ref;
}

status describe_hw_surface(const AVFrame* frame, std::uint32_t* out_texture_w,
                           std::uint32_t* out_texture_h, bool* out_ten_bit) noexcept {
  if (!frame || !out_texture_w || !out_texture_h || !out_ten_bit) return status::invalid_arg;
  if (frame->format != AV_PIX_FMT_D3D11) return status::invalid_arg;

  ID3D11Texture2D* pool = pool_texture_of(frame);
  if (!pool) return status::corrupt;

  D3D11_TEXTURE2D_DESC desc{};
  pool->GetDesc(&desc);

  // This is the ALLOCATION, padded up to the decoder's alignment — commonly a
  // multiple of 16 or 32, so a 1080p clip is routinely 1920x1088. Our ring
  // textures must match it exactly for CopySubresourceRegion with a null box,
  // and the visible rect (frame->width/height) travels separately so the shader
  // does not sample the padding.
  *out_texture_w = desc.Width;
  *out_texture_h = desc.Height;

  switch (desc.Format) {
    case DXGI_FORMAT_NV12: *out_ten_bit = false; return status::ok;
    case DXGI_FORMAT_P010: *out_ten_bit = true;  return status::ok;
    default:
      // P016 and the 4:2:2/4:4:4 formats are not in the D5 v1 set. Failing here
      // routes the clip to the software fallback, which is visible in the
      // overlay rather than silent.
      return status::unsupported_format;
  }
}

status copy_hw_surface(AVBufferRef* hw_device_ctx, const AVFrame* frame,
                       ID3D11Texture2D* dst) noexcept {
  if (!hw_device_ctx || !frame || !dst) return status::invalid_arg;
  if (frame->format != AV_PIX_FMT_D3D11) return status::invalid_arg;

  ID3D11Texture2D* pool = pool_texture_of(frame);
  if (!pool) return status::corrupt;

  auto* hw = reinterpret_cast<AVHWDeviceContext*>(hw_device_ctx->data);
  auto* d3d = static_cast<AVD3D11VADeviceContext*>(hw->hwctx);
  if (!d3d->device_context) return status::internal;

  // Take FFmpeg's OWN lock rather than reaching for ID3D10Multithread. It
  // defaults to the same lock today; going through the documented callback is
  // what keeps this correct if a future FFmpeg changes that default.
  d3d->lock(d3d->lock_ctx);

  // Submit and return. The decode thread does no GPU synchronisation of any
  // kind here — no Map, no Flush, no query wait — because it is not the render
  // thread's job to wait for it and it is not this thread's job to stall.
  d3d->device_context->CopySubresourceRegion(dst, 0, 0, 0, 0, pool, subresource_of(frame),
                                             nullptr);

  d3d->unlock(d3d->lock_ctx);

  // Safe to release the AVFrame the moment this returns: the copy is already
  // ordered on the same immediate context ahead of any subsequent decoder write
  // to that slice.
  return status::ok;
}

status create_texture_from_planes(ID3D11Device* device, std::uint32_t width,
                                  std::uint32_t height, bool ten_bit, const std::uint8_t* luma,
                                  int luma_pitch, const std::uint8_t* chroma, int chroma_pitch,
                                  video_frame* out) noexcept {
  if (!device || !luma || !chroma || !out || width == 0 || height == 0) {
    return status::invalid_arg;
  }
  // NV12 and P010 are 4:2:0: both dimensions must be even or the chroma plane
  // has no defined size.
  if ((width & 1u) || (height & 1u)) return status::invalid_arg;

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = ten_bit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
  desc.SampleDesc = {1, 0};
  // DEFAULT with initial data, not IMMUTABLE: the video formats are less well
  // travelled than R8G8B8A8 and IMMUTABLE has driver corner cases here. Both
  // create without touching a device context, which is the property that
  // matters — this is plan/02's worker-thread pattern, and the software path
  // deliberately does not extend the immediate-context exception that
  // copy_hw_surface needs.
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  // D3D11 lays an NV12/P010 texture out as the luma plane followed immediately
  // by the interleaved chroma plane, so one D3D11_SUBRESOURCE_DATA describes
  // both — but only if the two planes are contiguous at a single pitch. They
  // are not: swscale hands us two allocations with their own strides. Repack.
  const std::uint32_t bytes_per_sample = ten_bit ? 2u : 1u;
  const std::uint32_t row_bytes = width * bytes_per_sample;
  const std::size_t luma_bytes = static_cast<std::size_t>(row_bytes) * height;
  const std::size_t chroma_bytes = luma_bytes / 2;

  std::unique_ptr<std::uint8_t[]> packed(new (std::nothrow)
                                             std::uint8_t[luma_bytes + chroma_bytes]);
  if (!packed) return status::out_of_memory;

  for (std::uint32_t y = 0; y < height; ++y) {
    std::memcpy(packed.get() + static_cast<std::size_t>(y) * row_bytes,
                luma + static_cast<std::size_t>(y) * static_cast<std::size_t>(luma_pitch),
                row_bytes);
  }
  std::uint8_t* chroma_dst = packed.get() + luma_bytes;
  for (std::uint32_t y = 0; y < height / 2; ++y) {
    std::memcpy(chroma_dst + static_cast<std::size_t>(y) * row_bytes,
                chroma + static_cast<std::size_t>(y) * static_cast<std::size_t>(chroma_pitch),
                row_bytes);
  }

  D3D11_SUBRESOURCE_DATA init{};
  init.pSysMem = packed.get();
  init.SysMemPitch = row_bytes;
  init.SysMemSlicePitch = static_cast<UINT>(luma_bytes + chroma_bytes);

  gfx::com_ptr<ID3D11Texture2D> texture;
  HRESULT hr = device->CreateTexture2D(&desc, &init, texture.GetAddressOf());
  if (FAILED(hr)) return hr == E_OUTOFMEMORY ? status::out_of_memory : status::internal;

  D3D11_SHADER_RESOURCE_VIEW_DESC luma_srv{};
  luma_srv.Format = ten_bit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
  luma_srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  luma_srv.Texture2D.MipLevels = 1;

  D3D11_SHADER_RESOURCE_VIEW_DESC chroma_srv = luma_srv;
  chroma_srv.Format = ten_bit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;

  gfx::com_ptr<ID3D11ShaderResourceView> luma_view, chroma_view;
  hr = device->CreateShaderResourceView(texture.Get(), &luma_srv, luma_view.GetAddressOf());
  if (FAILED(hr)) return status::internal;
  hr = device->CreateShaderResourceView(texture.Get(), &chroma_srv, chroma_view.GetAddressOf());
  if (FAILED(hr)) return status::internal;

  out->texture = std::move(texture);
  out->luma = std::move(luma_view);
  out->chroma = std::move(chroma_view);
  return status::ok;
}

}  // namespace mv::player
