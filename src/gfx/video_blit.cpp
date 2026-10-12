// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 5a - NV12/P010 -> RGB with the stream's real matrix, plus HLG/PQ -> SDR
// tone-mapping. Lives in gfx/ because gfx may not include player.
//
// OWNER: mediaviewer-48 (5a). The draw entry takes raw ID3D11ShaderResourceView*
// plus a gfx::colour_desc, so gfx never sees a player type.
//
// Output is LINEAR light. The swapchain's RTV is R8G8B8A8_UNORM_SRGB
// (gfx/swapchain.cpp), so the hardware does the sRGB encode on write; writing
// an already-encoded value here would double-encode it.
#include "gfx/video_blit.h"

#include <cstring>

// Built from gfx/video_blit.hlsl by fxc (CMakeLists.txt, mv_fxc; issue #163),
// so device create never runs D3DCompile.
#include "video_blit_ps_main.h"
#include "video_blit_vs_main.h"

namespace mv::gfx {
namespace {

// Enum values are passed to the shader as uints and MUST match colour_desc.h.
// Compile-time checked below rather than trusted.
static_assert(static_cast<int>(colour_matrix::bt709) == 1);
static_assert(static_cast<int>(colour_matrix::bt601) == 2);
static_assert(static_cast<int>(colour_matrix::bt2020_ncl) == 3);
static_assert(static_cast<int>(colour_matrix::smpte240m) == 4);
static_assert(static_cast<int>(colour_transfer::bt709) == 1);
static_assert(static_cast<int>(colour_transfer::srgb) == 2);
static_assert(static_cast<int>(colour_transfer::smpte2084) == 3);
static_assert(static_cast<int>(colour_transfer::arib_std_b67) == 4);
static_assert(static_cast<int>(colour_range::limited) == 1);
static_assert(static_cast<int>(colour_range::full) == 2);
static_assert(static_cast<int>(colour_primaries::bt709) == 1);
static_assert(static_cast<int>(colour_primaries::bt601_525) == 2);
static_assert(static_cast<int>(colour_primaries::bt601_625) == 3);
static_assert(static_cast<int>(colour_primaries::bt2020) == 4);

struct alignas(16) video_blit_cb {
  float pan_x, pan_y;
  float zoom;
  float pad0;
  float window_w, window_h;
  float image_w, image_h;
  float texture_w, texture_h;
  float origin_x, origin_y;
  std::uint32_t matrix, transfer, range, bit_depth;
  std::uint32_t primaries, turns, pad2, pad3;
};
static_assert(sizeof(video_blit_cb) == 80, "cbuffer layout must match the HLSL declaration");

}  // namespace

video_blitter::~video_blitter() { destroy(); }

expected video_blitter::create(ID3D11Device* device) {
  destroy();
  if (!device) return err(status::invalid_arg);

  HRESULT hr = device->CreateVertexShader(kVideoBlitVsMain, sizeof(kVideoBlitVsMain), nullptr,
                                          vs_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);
  hr = device->CreatePixelShader(kVideoBlitPsMain, sizeof(kVideoBlitPsMain), nullptr,
                                 ps_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);

  D3D11_BUFFER_DESC cbd{};
  cbd.ByteWidth = sizeof(video_blit_cb);
  cbd.Usage = D3D11_USAGE_DYNAMIC;
  cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  hr = device->CreateBuffer(&cbd, nullptr, cb_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);

  // Linear, not anisotropic: the chroma plane is half resolution and an
  // aniso tap count on it buys nothing but bandwidth.
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxAnisotropy = 1;
  sd.MinLOD = 0.0f;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
  hr = device->CreateSamplerState(&sd, linear_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);

  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  hr = device->CreateSamplerState(&sd, point_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);

  D3D11_BLEND_DESC bd{};
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  hr = device->CreateBlendState(&bd, blend_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);

  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  hr = device->CreateRasterizerState(&rd, raster_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);

  return {};
}

void video_blitter::destroy() noexcept {
  raster_.Reset();
  blend_.Reset();
  point_.Reset();
  linear_.Reset();
  cb_.Reset();
  ps_.Reset();
  vs_.Reset();
}

void video_blitter::draw(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* luma,
                         ID3D11ShaderResourceView* chroma, const colour_desc& colour,
                         const video_blit_params& p) noexcept {
  if (!ctx || !luma || !chroma || !vs_ || !ps_ || !cb_) return;

  video_blit_cb cb{};
  cb.pan_x = p.pan_x;
  cb.pan_y = p.pan_y;
  cb.zoom = (p.zoom > 0.0f) ? p.zoom : 1.0f;
  cb.window_w = p.window_w;
  cb.window_h = p.window_h;
  cb.image_w = p.image_w;
  cb.image_h = p.image_h;
  // Guard the divide in the shader: a zero here would make every sample NaN.
  cb.texture_w = (p.texture_w > 0.0f) ? p.texture_w : p.image_w;
  cb.texture_h = (p.texture_h > 0.0f) ? p.texture_h : p.image_h;
  cb.origin_x = p.origin_x;
  cb.origin_y = p.origin_y;
  cb.matrix = static_cast<std::uint32_t>(colour.matrix);
  cb.transfer = static_cast<std::uint32_t>(colour.transfer);
  cb.range = static_cast<std::uint32_t>(colour.range);
  cb.bit_depth = colour.bit_depth;
  cb.primaries = static_cast<std::uint32_t>(colour.primaries);
  cb.turns = p.rotation & 3u;

  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (SUCCEEDED(ctx->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    std::memcpy(mapped.pData, &cb, sizeof(cb));
    ctx->Unmap(cb_.Get(), 0);
  }

  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->IASetInputLayout(nullptr);
  ctx->VSSetShader(vs_.Get(), nullptr, 0);
  ctx->PSSetShader(ps_.Get(), nullptr, 0);
  ID3D11Buffer* cbs[] = {cb_.Get()};
  ctx->VSSetConstantBuffers(0, 1, cbs);
  ctx->PSSetConstantBuffers(0, 1, cbs);
  ID3D11ShaderResourceView* srvs[] = {luma, chroma};
  ctx->PSSetShaderResources(0, 2, srvs);
  ID3D11SamplerState* samps[] = {linear_.Get(), point_.Get()};
  ctx->PSSetSamplers(0, 2, samps);
  ctx->OMSetBlendState(blend_.Get(), nullptr, 0xffffffff);
  ctx->RSSetState(raster_.Get());
  ctx->Draw(3, 0);

  // Unbind: the still path binds t0 too, and leaving a video plane there makes
  // the next photo draw sample luma as if it were RGBA.
  ID3D11ShaderResourceView* none[] = {nullptr, nullptr};
  ctx->PSSetShaderResources(0, 2, none);
}

}  // namespace mv::gfx
