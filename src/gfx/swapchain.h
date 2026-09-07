// SPDX-License-Identifier: GPL-2.0-or-later
// The flip-model, frame-latency-waitable swapchain, hosted in a
// DirectComposition visual tree.
//
// plan/03-rendering.md, non-negotiable:
//   BufferCount 3, FLIP_DISCARD, FRAME_LATENCY_WAITABLE_OBJECT,
//   SetMaximumFrameLatency(1), and the render thread waits on the waitable
//   object BEFORE recording the frame — not after Present. That ordering is
//   what buys input->photon of one refresh interval.
//
// The canvas is this swapchain. It is never a XAML Image, never a
// MediaPlayerElement, and under the D1 amendment it is never a SwapChainPanel
// either: PR 3 hosts WinUI chrome inside this window as XAML islands rather
// than re-implementing presentation.
#pragma once

#include <dcomp.h>

#include <cstdint>

#include "core/result.h"
#include "gfx/device.h"

namespace mv::gfx {

struct swapchain_desc {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  // v1 is an 8-bit sRGB swapchain (D6). The FP16 scRGB branch exists in the
  // format choice and nowhere else; the linear FP16 *working space* is a
  // separate decision and is not this one.
  bool hdr_output = false;
};

class swapchain {
 public:
  swapchain() = default;
  ~swapchain();

  swapchain(const swapchain&) = delete;
  swapchain& operator=(const swapchain&) = delete;

  [[nodiscard]] expected create(device& dev, HWND window, const swapchain_desc& desc) noexcept;
  void destroy() noexcept;

  [[nodiscard]] bool valid() const noexcept { return swapchain_ != nullptr; }

  // Blocks until the compositor is ready for the next frame. Call this at the
  // TOP of the frame. Returns false if the wait failed (device teardown).
  [[nodiscard]] bool wait_for_next_frame(std::uint32_t timeout_ms = 1000) noexcept;

  // Present with vsync. `allow_tearing` is only honoured when the swapchain was
  // created with the tearing flag AND the caller has established the display
  // supports VRR — note that a composition swapchain cannot tear, so this is
  // effectively lab-only (plan/03).
  [[nodiscard]] HRESULT present(bool allow_tearing = false) noexcept;

  // A no-render visibility probe for the occlusion path.
  [[nodiscard]] HRESULT present_test() noexcept;

  [[nodiscard]] expected resize(std::uint32_t width, std::uint32_t height) noexcept;

  [[nodiscard]] ID3D11RenderTargetView* back_buffer_rtv() const noexcept { return rtv_.Get(); }
  [[nodiscard]] IDXGISwapChain4* dxgi() const noexcept { return swapchain_.Get(); }
  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
  [[nodiscard]] DXGI_FORMAT format() const noexcept { return format_; }
  [[nodiscard]] bool tearing_supported() const noexcept { return tearing_supported_; }

  // The current output interval in seconds, or zero when unavailable.
  // Re-queried on WM_DISPLAYCHANGE and on window move (plan/03).
  [[nodiscard]] double refresh_interval_seconds() const noexcept { return refresh_seconds_; }
  void refresh_output_info() noexcept;

 private:
  [[nodiscard]] expected create_rtv() noexcept;

  device* device_ = nullptr;
  HWND window_ = nullptr;

  com_ptr<IDXGISwapChain4> swapchain_;
  com_ptr<ID3D11RenderTargetView> rtv_;

  // DirectComposition visual tree. Rounded corners, transparency, and — the
  // reason it is here in PR 1 rather than PR 3 — the native canvas island that
  // WinUI chrome will later be hosted alongside.
  com_ptr<IDCompositionDevice> comp_device_;
  com_ptr<IDCompositionTarget> comp_target_;
  com_ptr<IDCompositionVisual> comp_visual_;

  HANDLE waitable_ = nullptr;

  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  DXGI_FORMAT format_ = DXGI_FORMAT_R8G8B8A8_UNORM;
  bool tearing_supported_ = false;
  double refresh_seconds_ = 0.0;
};

}  // namespace mv::gfx
