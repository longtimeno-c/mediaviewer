// SPDX-License-Identifier: GPL-2.0-or-later
#include "gfx/swapchain.h"

#include <cwchar>
#include <vector>

#include "core/trace.h"

namespace mv::gfx {

namespace {

status from_hresult(HRESULT hr) noexcept {
  if (SUCCEEDED(hr)) return status::ok;
  switch (hr) {
    case E_OUTOFMEMORY:             return status::out_of_memory;
    case E_INVALIDARG:              return status::invalid_arg;
    case DXGI_ERROR_DEVICE_REMOVED:
    case DXGI_ERROR_DEVICE_RESET:   return status::device_lost;
    default:                        return status::internal;
  }
}

bool query_tearing_support(IDXGIFactory2* factory) noexcept {
  com_ptr<IDXGIFactory5> f5;
  if (FAILED(factory->QueryInterface(IID_PPV_ARGS(f5.GetAddressOf())))) return false;
  BOOL allowed = FALSE;
  if (FAILED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowed,
                                     sizeof(allowed)))) {
    return false;
  }
  return allowed != FALSE;
}

}  // namespace

swapchain::~swapchain() { destroy(); }

expected swapchain::create(device& dev, HWND window, const swapchain_desc& desc) noexcept {
  destroy();
  device_ = &dev;
  window_ = window;
  width_ = desc.width ? desc.width : 1;
  height_ = desc.height ? desc.height : 1;

  // Flip-model buffers are UNORM; the sRGB RTV encodes linear writes (D6).
  format_ = desc.hdr_output ? DXGI_FORMAT_R16G16B16A16_FLOAT   // scRGB, v1.1 (D6)
                            : DXGI_FORMAT_R8G8B8A8_UNORM;

  tearing_supported_ = query_tearing_support(dev.factory());

  DXGI_SWAP_CHAIN_DESC1 scd{};
  scd.Width = width_;
  scd.Height = height_;
  scd.Format = format_;
  scd.Stereo = FALSE;
  scd.SampleDesc = {1, 0};                        // flip model forbids MSAA on the back buffer
  scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  scd.BufferCount = 3;
  scd.Scaling = DXGI_SCALING_STRETCH;             // required for composition swapchains
  scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  scd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
  if (tearing_supported_) scd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

  com_ptr<IDXGISwapChain1> sc1;
  HRESULT hr = dev.factory()->CreateSwapChainForComposition(dev.d3d(), &scd, nullptr,
                                                            sc1.GetAddressOf());
  if (FAILED(hr)) {
    destroy();
    return err(from_hresult(hr));
  }
  if (FAILED(sc1.As(&swapchain_))) {
    destroy();
    return err(status::internal);
  }

  // One frame in flight. This is the low-latency half of the pacing contract;
  // the other half is waiting on the object before recording, not after Present.
  if (FAILED(swapchain_->SetMaximumFrameLatency(1))) {
    destroy();
    return err(status::internal);
  }
  waitable_ = swapchain_->GetFrameLatencyWaitableObject();
  if (!waitable_) {
    destroy();
    return err(status::internal);
  }

  // Composition swapchains are never presented to an HWND directly; they live
  // in a DComp visual tree. topmost = FALSE so WinUI island child HWNDs (PR 3)
  // sit above the swapchain; TRUE would hide the command bar under the canvas.
  hr = ::DCompositionCreateDevice(nullptr, IID_PPV_ARGS(comp_device_.GetAddressOf()));
  if (FAILED(hr)) { destroy(); return err(from_hresult(hr)); }
  hr = comp_device_->CreateTargetForHwnd(window, FALSE, comp_target_.GetAddressOf());
  if (FAILED(hr)) { destroy(); return err(from_hresult(hr)); }
  hr = comp_device_->CreateVisual(comp_visual_.GetAddressOf());
  if (FAILED(hr)) { destroy(); return err(from_hresult(hr)); }
  if (FAILED(comp_visual_->SetContent(swapchain_.Get())) ||
      FAILED(comp_target_->SetRoot(comp_visual_.Get())) ||
      FAILED(comp_device_->Commit())) {
    destroy();
    return err(status::internal);
  }

  // ALT+ENTER belongs to us, not to DXGI. This is a windowed app.
  (void)dev.factory()->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

  refresh_output_info();

  auto rtv = create_rtv();
  if (!rtv) { destroy(); return rtv; }

  MV_LOG_INFO("gfx: swapchain %ux%u, tearing %s, refresh %.3f ms", width_, height_,
              tearing_supported_ ? "supported" : "unavailable", refresh_seconds_ * 1000.0);
  return {};
}

expected swapchain::create_rtv() noexcept {
  rtv_.Reset();

  com_ptr<ID3D11Texture2D> back_buffer;
  HRESULT hr = swapchain_->GetBuffer(0, IID_PPV_ARGS(back_buffer.GetAddressOf()));
  if (FAILED(hr)) return err(from_hresult(hr));

  // The _SRGB view over the UNORM buffer: this is where D6's "8-bit sRGB
  // swapchain" actually happens.
  D3D11_RENDER_TARGET_VIEW_DESC rtvd{};
  rtvd.Format = (format_ == DXGI_FORMAT_R8G8B8A8_UNORM) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                                        : format_;
  rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

  hr = device_->d3d()->CreateRenderTargetView(back_buffer.Get(), &rtvd, rtv_.GetAddressOf());
  if (FAILED(hr)) return err(from_hresult(hr));
  return {};
}

void swapchain::destroy() noexcept {
  if (device_ && device_->context()) {
    ID3D11RenderTargetView* none = nullptr;
    device_->context()->OMSetRenderTargets(1, &none, nullptr);
    device_->context()->Flush();
  }
  rtv_.Reset();
  comp_visual_.Reset();
  comp_target_.Reset();
  comp_device_.Reset();
  // GetFrameLatencyWaitableObject transfers handle ownership to the caller.
  if (waitable_) ::CloseHandle(waitable_);
  waitable_ = nullptr;
  swapchain_.Reset();
  device_ = nullptr;
  window_ = nullptr;
}

bool swapchain::wait_for_next_frame(std::uint32_t timeout_ms) noexcept {
  if (!waitable_) return false;
  return ::WaitForSingleObject(waitable_, timeout_ms) == WAIT_OBJECT_0;
}

HRESULT swapchain::present(bool allow_tearing) noexcept {
  if (!swapchain_) return DXGI_ERROR_INVALID_CALL;
  // The waitable object bounds queue depth. Sync interval 1 retains each
  // frame for a vblank; it does not add a second mandatory CPU-side wait.
  const bool tear = allow_tearing && tearing_supported_;
  return swapchain_->Present(tear ? 0u : 1u, tear ? DXGI_PRESENT_ALLOW_TEARING : 0u);
}

HRESULT swapchain::present_test() noexcept {
  if (!swapchain_) return DXGI_ERROR_INVALID_CALL;
  return swapchain_->Present(0, DXGI_PRESENT_TEST);
}

expected swapchain::resize(std::uint32_t width, std::uint32_t height) noexcept {
  if (!swapchain_) return err(status::internal);
  width = width ? width : 1;
  height = height ? height : 1;
  if (width == width_ && height == height_) return rtv_ ? expected{} : create_rtv();

  rtv_.Reset();
  if (device_ && device_->context()) {
    ID3D11RenderTargetView* none = nullptr;
    device_->context()->OMSetRenderTargets(1, &none, nullptr);
    device_->context()->Flush();
  }

  UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
  if (tearing_supported_) flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

  const HRESULT hr = swapchain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, flags);
  if (FAILED(hr)) {
    // Some drivers discard the buffers even when ResizeBuffers fails. Try
    // the old view first, then recreate the previous size if necessary.
    // Still report the original failure so the caller can rebuild or abort.
    if (from_hresult(hr) != status::device_lost && !create_rtv()) {
      if (SUCCEEDED(swapchain_->ResizeBuffers(0, width_, height_, DXGI_FORMAT_UNKNOWN, flags)))
        (void)create_rtv();
    }
    return err(from_hresult(hr));
  }

  width_ = width;
  height_ = height;
  refresh_output_info();
  return create_rtv();
}

void swapchain::refresh_output_info() noexcept {
  // GetContainingOutput is invalid on composition swapchains. Match the host
  // monitor to an active display path and read its current rational rate.
  // Zero means unknown: guessing 60 Hz must never certify a measurement.
  refresh_seconds_ = 0.0;
  if (!window_) return;
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (!::GetMonitorInfoW(::MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &mi)) return;
  try {
    // Topology can change between sizing and querying; retry with a bound.
    for (int attempt = 0; attempt < 3; ++attempt) {
      UINT32 path_count = 0, mode_count = 0;
      if (::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) !=
          ERROR_SUCCESS) return;
      std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
      std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
      const LONG rc = ::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(),
                                           &mode_count, modes.data(), nullptr);
      if (rc == ERROR_INSUFFICIENT_BUFFER) continue;
      if (rc != ERROR_SUCCESS) return;
      for (UINT32 i = 0; i < path_count; ++i) {
        const auto& path = paths[i];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source),
                         path.sourceInfo.adapterId, path.sourceInfo.id};
        if (::DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            std::wcscmp(source.viewGdiDeviceName, mi.szDevice) != 0) continue;
        const auto rate = path.targetInfo.refreshRate;
        if (rate.Numerator == 0 || rate.Denominator == 0) return;
        const double seconds = static_cast<double>(rate.Denominator) / rate.Numerator;
        // A cloned source with differing target rates is ambiguous.
        if (refresh_seconds_ != 0.0 && refresh_seconds_ != seconds) {
          refresh_seconds_ = 0.0;
          return;
        }
        refresh_seconds_ = seconds;
      }
      return;
    }
  } catch (...) {
    refresh_seconds_ = 0.0;
  }
}

}  // namespace mv::gfx
