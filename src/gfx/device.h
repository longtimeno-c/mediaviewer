// SPDX-License-Identifier: GPL-2.0-or-later
// The D3D11 device, and the adapter-selection rules that go with it.
//
// plan/03-rendering.md: pick the adapter that drives the *output the window is
// on* — not adapter 0, not highest VRAM — and rebuild when the window moves to
// another GPU or the device is removed. The hybrid-GPU case was used to reject
// libmpv (D2), which makes it our problem on the path we chose.
#pragma once

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>

#include "core/result.h"

namespace mv::gfx {

template <typename T>
using com_ptr = Microsoft::WRL::ComPtr<T>;

struct adapter_info {
  LUID luid{};
  std::uint64_t dedicated_video_memory = 0;
  wchar_t description[128]{};
};

class device {
 public:
  device() = default;
  ~device();

  device(const device&) = delete;
  device& operator=(const device&) = delete;

  // Creates a device on the adapter driving `window`'s current monitor.
  // D3D11_CREATE_DEVICE_BGRA_SUPPORT for DirectComposition; VIDEO_SUPPORT is
  // requested up front so PR 5a can build the FFmpeg D3D11VA context from this
  // exact device rather than creating a second one.
  [[nodiscard]] expected create(HWND window) noexcept;

  void destroy() noexcept;

  [[nodiscard]] bool valid() const noexcept { return d3d_ != nullptr; }

  [[nodiscard]] ID3D11Device5* d3d() const noexcept { return d3d_.Get(); }
  [[nodiscard]] ID3D11DeviceContext4* context() const noexcept { return context_.Get(); }
  [[nodiscard]] IDXGIFactory2* factory() const noexcept { return factory_.Get(); }
  [[nodiscard]] IDXGIAdapter4* adapter() const noexcept { return adapter_.Get(); }
  [[nodiscard]] const adapter_info& info() const noexcept { return info_; }

  // True when the adapter driving `window`'s monitor is no longer the one this
  // device was created on. Checked on WM_DISPLAYCHANGE and on window move; a
  // true here means rebuild, same path as DXGI_ERROR_DEVICE_REMOVED.
  [[nodiscard]] bool adapter_changed_for(HWND window) const noexcept;

  // DXGI_ERROR_DEVICE_REMOVED / _RESET, with the removal reason if any.
  [[nodiscard]] HRESULT removed_reason() const noexcept;

  // Current VRAM budget from IDXGIAdapter3::QueryVideoMemoryInfo. The 60/25/15
  // split in plan/02 is applied by the caches that spend it, not here.
  [[nodiscard]] std::uint64_t video_memory_budget() const noexcept;

 private:
  [[nodiscard]] result<com_ptr<IDXGIAdapter4>> adapter_for_window(HWND window) const noexcept;

  com_ptr<IDXGIFactory6> factory_;
  com_ptr<IDXGIAdapter4> adapter_;
  com_ptr<ID3D11Device5> d3d_;
  com_ptr<ID3D11DeviceContext4> context_;
  adapter_info info_{};
  D3D_FEATURE_LEVEL feature_level_ = D3D_FEATURE_LEVEL_11_0;
};

}  // namespace mv::gfx
