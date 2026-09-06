// SPDX-License-Identifier: GPL-2.0-or-later
// The present lab: PR 1's instrument, and the app's actual render thread.
//
// plan/10-roadmap.md, PR 1: "Win32 + DComp + Dear ImGui host, D3D11 device,
// flip-model waitable swapchain, per-monitor-v2 DPI, clear to a colour, F3
// frame-time overlay reading real present-to-present intervals."
//
// It stays in the tree as a debug harness. Under the D1 amendment this window
// and this swapchain ARE the app — PR 3 hosts WinUI chrome inside them rather
// than re-implementing presentation, so nothing here is throwaway.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "core/spsc_ring.h"
#include "gfx/device.h"
#include "gfx/pacer.h"
#include "gfx/swapchain.h"
#include "shell/input_state.h"

namespace mv::shell {

struct lab_options {
  // Run for this many seconds, write the JSON report, then exit. 0 = run until
  // the window is closed. This is what tools/frametime drives.
  double soak_seconds = 0.0;
  std::wstring json_report_path;
  // With a soak, exit non-zero if the PR 1 gate did not hold. CI wants this;
  // a human running the lab does not.
  bool gate_exit_code = false;
  bool start_animating = true;
  bool overlay_visible = true;
};

class present_lab {
 public:
  present_lab() = default;
  ~present_lab();

  present_lab(const present_lab&) = delete;
  present_lab& operator=(const present_lab&) = delete;

  // Called from the UI thread once the window exists.
  [[nodiscard]] expected start(HWND window, const lab_options& options) noexcept;

  // Signals the render thread and joins it. Safe to call twice.
  void stop() noexcept;

  // [ui-thread] Publishes the snapshot the render thread will consume next
  // frame. Never blocks.
  void publish(const input_snapshot& snapshot) noexcept { input_.publish(snapshot); }

  // [ui-thread][no-block] Wakes the render thread out of the idle wait.
  void wake() noexcept;

  // True once the render thread has stopped, for whatever reason — including
  // the soak finishing, which is how the UI thread knows to close the window.
  [[nodiscard]] bool finished() const noexcept {
    return finished_.load(std::memory_order_acquire);
  }

  // Valid after finished(). 0 when the gate held or no gate was requested.
  [[nodiscard]] int exit_code() const noexcept { return exit_code_; }

 private:
  void render_thread_main() noexcept;
  [[nodiscard]] expected rebuild_device() noexcept;
  void draw_frame(const input_snapshot& snapshot, double elapsed_seconds) noexcept;
  void draw_overlay(const input_snapshot& snapshot) noexcept;
  void write_json_report() const noexcept;

  HWND window_ = nullptr;
  lab_options options_{};

  gfx::device device_;
  gfx::swapchain swapchain_;
  gfx::pacer pacer_;

  publish_slot<input_snapshot> input_;
  std::thread render_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> finished_{false};
  HANDLE wake_event_ = nullptr;
  int exit_code_ = 0;

  // Render-thread-only state.
  bool overlay_visible_ = true;
  bool animating_ = true;
  bool occluded_ = false;
  bool imgui_ready_ = false;
  bool warmed_up_ = false;
  std::uint32_t seen_overlay_seq_ = 0;
  std::uint32_t seen_animation_seq_ = 0;
  std::uint32_t seen_reset_seq_ = 0;
  std::uint32_t seen_resize_seq_ = 0;
  std::uint32_t seen_display_seq_ = 0;
  double animation_phase_ = 0.0;
  double last_input_time_ = 0.0;
};

}  // namespace mv::shell
