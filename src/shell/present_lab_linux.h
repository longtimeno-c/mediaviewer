// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// OpenGL present lab — Linux PR 1's instrument, kept as the Linux debug
// harness as the Win32 and Metal labs are (docs/plans/linux.md, docs/design/10).
// The render thread owns the EGL device and surface, waits on the surface's
// frame callback before it records (gfx/wayland_frame_clock.h), stops
// presenting when idle (gfx/present_policy.h), and scores the run from
// wp_presentation feedback (gfx/presentation_pacer.h).
//
// It is given a wl_display and a wl_surface it does not own, so Linux PR 3
// can hand it Qt's child surface instead of the lab's own toplevel.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "core/result.h"
#include "core/spsc_ring.h"
#include "gfx/device_gl.h"
#include "gfx/gl_surface_wayland.h"
#include "gfx/presentation_pacer.h"
#include "gfx/wayland_frame_clock.h"
#include "shell/input_state.h"

namespace mv::shell {

struct linux_lab_options {
  double soak_seconds = 0.0;
  std::string json_report_path;
  bool gate_exit_code = false;
  bool start_animating = false;
  bool overlay_visible = true;
  // A measuring run with no --soak timer (--boot-report): present whether or
  // not the window is active, as a soak does.
  bool harness = false;
  // [render thread] Right before each commit of a width x height buffer. The
  // host acks its configure and sets the viewport that goes with that size.
  std::function<void(std::uint32_t width, std::uint32_t height)> before_commit;
  // [any thread] The refresh of the outputs the surface is on, mHz; 0 unknown.
  std::function<std::uint32_t()> output_refresh_mhz;
};

class present_lab_linux {
 public:
  present_lab_linux() = default;
  ~present_lab_linux();

  present_lab_linux(const present_lab_linux&) = delete;
  present_lab_linux& operator=(const present_lab_linux&) = delete;

  // [UI thread] Starts the render thread and returns once its device and
  // surface exist (or failed: exit_code() is 2 then). `initial` is the first
  // snapshot: the surface is created at its size.
  [[nodiscard]] expected start(void* wl_display, void* wl_surface, const input_snapshot& initial,
                               const linux_lab_options& options) noexcept;
  void stop() noexcept;
  // [UI thread] The newest input and window state, then wake() to have the
  // render thread look at it.
  void publish(const input_snapshot& snapshot) noexcept { input_.publish(snapshot); }
  // [any thread] One write to an eventfd: no lock, and a wake while the
  // render thread is busy is still seen by its next wait.
  void wake() noexcept;

  // [any thread] Readable (an eventfd) when the render thread finished or
  // presented its first frame; the UI loop polls it beside the display.
  [[nodiscard]] int events_fd() const noexcept { return events_fd_; }
  [[nodiscard]] bool finished() const noexcept { return finished_.load(std::memory_order_acquire); }
  [[nodiscard]] bool first_frame_presented() const noexcept {
    return first_frame_.load(std::memory_order_acquire);
  }
  [[nodiscard]] int exit_code() const noexcept { return exit_code_.load(std::memory_order_acquire); }

 private:
  void render_thread_main() noexcept;
  void absorb_feedback() noexcept;
  void drain_feedback_at_end() noexcept;
  void draw_frame(const input_snapshot& snapshot, double elapsed, float delta, float wheel) noexcept;
  void draw_overlay(const input_snapshot& snapshot) noexcept;
  [[nodiscard]] bool rebuild_device() noexcept;
  [[nodiscard]] bool write_json_report() const noexcept;
  void signal_events() noexcept;
  void fail_start() noexcept;

  void* display_ = nullptr;
  void* surface_ = nullptr;
  linux_lab_options options_{};
  input_snapshot initial_{};

  gfx::gl_device device_;
  gfx::gl_surface_wayland gl_surface_;
  gfx::wayland_frame_clock clock_;
  gfx::presentation_pacer pacer_;

  publish_slot<input_snapshot> input_;
  std::thread render_thread_;
  int wake_fd_ = -1;
  int events_fd_ = -1;
  int ready_fd_ = -1;
  std::atomic<bool> running_{false};
  std::atomic<bool> finished_{false};
  std::atomic<bool> first_frame_{false};
  std::atomic<int> exit_code_{0};

  // Render thread only.
  bool imgui_ready_ = false;
  bool overlay_visible_ = true;
  bool animating_ = false;
  bool occluded_ = false;
  bool warmed_up_ = false;
  bool measurement_valid_ = true;
  bool soak_complete_ = false;
  bool was_presenting_ = false;
  bool painted_static_ = false;
  bool redraw_owed_ = false;
  std::uint64_t frame_id_ = 0;
  std::uint64_t total_presents_ = 0;
  input_cursor input_cursor_;
  gfx::presentation_idle_stats idle_stats_;
  double measurement_start_seconds_ = 0.0;
  double idle_start_cpu_seconds_ = -1.0;
  double last_input_time_ = -1.0;
  std::uint32_t seen_overlay_seq_ = 0;
  std::uint32_t seen_animation_seq_ = 0;
  std::uint32_t seen_reset_seq_ = 0;
  std::uint32_t seen_resize_seq_ = 0;
  std::uint32_t seen_display_seq_ = 0;
  std::uint64_t device_rebuilds_ = 0;
};

}  // namespace mv::shell
