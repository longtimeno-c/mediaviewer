// SPDX-License-Identifier: GPL-2.0-or-later
#include "gfx/device.h"

#include <cwchar>

#include "core/trace.h"

namespace mv::gfx {

namespace {

// Maps an HRESULT onto the ABI's status set. Device loss is called out
// separately because callers rebuild rather than fail.
status from_hresult(HRESULT hr) noexcept {
  if (SUCCEEDED(hr)) return status::ok;
  switch (hr) {
    case E_OUTOFMEMORY:                return status::out_of_memory;
    case E_INVALIDARG:                 return status::invalid_arg;
    case DXGI_ERROR_DEVICE_REMOVED:
    case DXGI_ERROR_DEVICE_RESET:
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
      return status::device_lost;
    default:                           return status::internal;
  }
}

}  // namespace

device::~device() { destroy(); }

result<com_ptr<IDXGIAdapter4>> device::adapter_for_window(HWND window) const noexcept {
  // The monitor the window is (mostly) on. DXGI reports the one it overlaps
  // most, which is also what MonitorFromWindow gives us.
  HMONITOR monitor = ::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);

  com_ptr<IDXGIAdapter1> candidate;
  for (UINT i = 0; factory_->EnumAdapters1(i, candidate.ReleaseAndGetAddressOf()) !=
                   DXGI_ERROR_NOT_FOUND;
       ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(candidate->GetDesc1(&desc))) continue;
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;  // skip WARP

    com_ptr<IDXGIOutput> output;
    for (UINT j = 0; candidate->EnumOutputs(j, output.ReleaseAndGetAddressOf()) !=
                     DXGI_ERROR_NOT_FOUND;
         ++j) {
      DXGI_OUTPUT_DESC odesc{};
      if (FAILED(output->GetDesc(&odesc))) continue;
      if (odesc.Monitor == monitor) {
        com_ptr<IDXGIAdapter4> found;
        if (FAILED(candidate.As(&found))) return err(status::internal);
        return found;
      }
    }
  }

  // No adapter claims this monitor — a hybrid laptop where the iGPU drives the
  // panel through the dGPU, or a remote session. Fall back to the adapter DXGI
  // prefers for performance rather than guessing adapter 0.
  com_ptr<IDXGIAdapter4> preferred;
  const HRESULT hr = factory_->EnumAdapterByGpuPreference(
      0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(preferred.GetAddressOf()));
  if (FAILED(hr)) return err(from_hresult(hr));
  MV_LOG_WARN("gfx: no adapter enumerates this monitor; using DXGI's preferred adapter");
  return preferred;
}

expected device::create(HWND window) noexcept {
  destroy();

  UINT factory_flags = 0;
#if defined(_DEBUG)
  factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
  HRESULT hr = ::CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(factory_.GetAddressOf()));
  if (FAILED(hr)) return err(from_hresult(hr));

  auto picked = adapter_for_window(window);
  if (!picked) return err(picked.error());
  adapter_ = std::move(picked).value();

  DXGI_ADAPTER_DESC3 desc{};
  if (SUCCEEDED(adapter_->GetDesc3(&desc))) {
    info_.luid = desc.AdapterLuid;
    info_.dedicated_video_memory = desc.DedicatedVideoMemory;
    std::wcsncpy(info_.description, desc.Description,
                 sizeof(info_.description) / sizeof(wchar_t) - 1);
  }

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT       // DirectComposition requires it
             | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;     // PR 5a builds D3D11VA on THIS device
#if defined(_DEBUG)
  flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

  // 11_0 is the floor. Windows 10 21H2 (plan/02 platform floor) guarantees it
  // on anything with a display driver.
  static const D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
  };

  com_ptr<ID3D11Device> base_device;
  com_ptr<ID3D11DeviceContext> base_context;
  hr = ::D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                           ARRAYSIZE(levels), D3D11_SDK_VERSION, base_device.GetAddressOf(),
                           &feature_level_, base_context.GetAddressOf());
#if defined(_DEBUG)
  if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING || hr == E_FAIL) {
    // The debug layer is not installed on this machine. Retry without it
    // rather than refusing to start.
    flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
    hr = ::D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                             ARRAYSIZE(levels), D3D11_SDK_VERSION, base_device.GetAddressOf(),
                             &feature_level_, base_context.GetAddressOf());
  }
#endif
  if (FAILED(hr)) return err(from_hresult(hr));

  if (FAILED(base_device.As(&d3d_)) || FAILED(base_context.As(&context_))) {
    destroy();
    return err(status::internal);
  }

  // Decode workers create immutable textures from their own threads
  // (plan/02, "Free-threaded resource creation") and PR 5a hands this device to
  // FFmpeg. Both require the multithread-protected flag to be honest about it.
  com_ptr<ID3D10Multithread> multithread;
  if (SUCCEEDED(d3d_.As(&multithread))) {
    multithread->SetMultithreadProtected(TRUE);
  }

  MV_LOG_INFO("gfx: %ls, feature level %x, %llu MB dedicated", info_.description,
              static_cast<unsigned>(feature_level_),
              static_cast<unsigned long long>(info_.dedicated_video_memory >> 20));
  return {};
}

void device::destroy() noexcept {
  if (context_) context_->ClearState();
  context_.Reset();
  d3d_.Reset();
  adapter_.Reset();
  factory_.Reset();
  info_ = {};
}

bool device::adapter_changed_for(HWND window) const noexcept {
  if (!factory_ || !adapter_) return false;
  auto now = adapter_for_window(window);
  if (!now) return false;  // transient enumeration failure is not a rebuild signal

  DXGI_ADAPTER_DESC3 desc{};
  if (FAILED(now.value()->GetDesc3(&desc))) return false;
  return desc.AdapterLuid.LowPart != info_.luid.LowPart ||
         desc.AdapterLuid.HighPart != info_.luid.HighPart;
}

HRESULT device::removed_reason() const noexcept {
  return d3d_ ? d3d_->GetDeviceRemovedReason() : S_OK;
}

std::uint64_t device::video_memory_budget() const noexcept {
  if (!adapter_) return 0;
  DXGI_QUERY_VIDEO_MEMORY_INFO info{};
  if (FAILED(adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) return 0;
  return info.Budget;
}

}  // namespace mv::gfx
