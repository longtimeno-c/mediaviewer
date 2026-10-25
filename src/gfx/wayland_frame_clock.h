// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Wayland half of the Linux pacer: the render thread's wait before it
// records a frame, and the wp_presentation feedback the gate is judged by
// (Linux PR 1, docs/plans/linux.md §2, docs/design/03).
//
// Windows waits on the swapchain's frame-latency object with
// SetMaximumFrameLatency(1); the Mac waits on CAMetalDisplayLink. Here the
// render thread asks for the surface's next wl_surface.frame callback and a
// wp_presentation_feedback before eglSwapBuffers commits (interval 0, so EGL
// never blocks inside the swap), and waits for that callback before it reads
// input and steps springs for the next frame: never two frames in flight.
//
// Everything runs on a private wl_event_queue, so this never dispatches the
// host's own queue (the UI thread's, or Qt's from Linux PR 3); wl_display is
// thread-safe for requests and for the prepare/read/dispatch dance below.
// The surface is borrowed: the lab's own xdg toplevel now, Qt's child
// surface later. No Wayland type in this header (D9).
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/result.h"
#include "gfx/presentation_pacer.h"

namespace mv::gfx {

class wayland_frame_clock {
 public:
  enum class wait_result : std::uint8_t {
    frame,    // the outstanding frame callback fired (or none was outstanding)
    timeout,  // nothing within the timeout: a hidden surface gets no callbacks
    woken,    // the wake fd was signalled (and has been reset)
    failed,   // the connection is gone
  };

  wayland_frame_clock() = default;
  ~wayland_frame_clock();
  wayland_frame_clock(const wayland_frame_clock&) = delete;
  wayland_frame_clock& operator=(const wayland_frame_clock&) = delete;

  // [render thread] Binds its own wp_presentation (version 1) on a private
  // queue. Without the global it still paces on frame callbacks, and every
  // frame is unavailable to the gate.
  [[nodiscard]] expected create(void* wl_display, void* wl_surface) noexcept;
  void destroy() noexcept;

  // [render thread] Before the eglSwapBuffers that commits frame `frame_id`:
  // the next frame callback and its presentation feedback, both pending state
  // the commit applies.
  void request(std::uint64_t frame_id) noexcept;
  // [render thread] The swap for `frame_id` failed, so no commit carried what
  // request() asked for and nothing answers it until another commit does:
  // forget its callback and feedback, or wait_frame() would wait on that
  // callback for ever and the loop never commit again. The compositor still
  // holds both as pending state; the next commit fires them into proxies
  // already destroyed, which libwayland drops.
  void abandon(std::uint64_t frame_id) noexcept;

  // [render thread] Waits for the outstanding frame callback, dispatching the
  // private queue. `timeout_ms` < 0 waits for ever; `wake_fd` (an eventfd,
  // or -1) interrupts the wait and is read back to zero.
  [[nodiscard]] wait_result wait_frame(int timeout_ms, int wake_fd) noexcept;
  // [render thread] The idle wait: dispatches feedback until woken or timed
  // out. Returns `frame` if a callback outstanding at entry fired meanwhile
  // (the occlusion probe: a hidden surface's pending callback fires once it
  // is shown again).
  [[nodiscard]] wait_result wait(int timeout_ms, int wake_fd) noexcept;

  // [render thread] Feedback answers received since the last drain, oldest
  // first. Returns how many were written.
  std::size_t drain(presentation_sample* out, std::size_t cap) noexcept;

  [[nodiscard]] bool frame_outstanding() const noexcept { return callback_ != nullptr; }
  [[nodiscard]] bool has_presentation() const noexcept { return presentation_ != nullptr; }
  // wp_presentation.clock_id: the clockid_t the feedback timestamps are in
  // (CLOCK_MONOTONIC on Mutter and KWin, CLOCK_MONOTONIC_RAW on headless
  // Weston); -1 until the compositor says.
  [[nodiscard]] int clock_id() const noexcept { return clock_id_; }
  // The last presented answer's flags, for F3.
  [[nodiscard]] std::uint32_t last_flags() const noexcept { return last_flags_; }

  // Listener plumbing (C callbacks from libwayland); not for callers.
  void on_clock_id(std::uint32_t clock) noexcept { clock_id_ = static_cast<int>(clock); }
  void on_global(void* registry, std::uint32_t name, const char* interface,
                 std::uint32_t version) noexcept;
  void on_frame_done(void* callback) noexcept;
  void on_feedback(void* feedback, bool presented, std::uint64_t timestamp_ns,
                   std::uint32_t refresh_ns, std::uint64_t msc, std::uint32_t flags) noexcept;

  static constexpr std::size_t max_in_flight = 16;
  static constexpr std::size_t sample_capacity = 128;

 private:
  [[nodiscard]] wait_result pump(int timeout_ms, int wake_fd, bool until_frame) noexcept;
  void push_sample(const presentation_sample& sample) noexcept;

  void* display_ = nullptr;       // wl_display*, borrowed
  void* surface_ = nullptr;       // wl_surface*, borrowed
  void* queue_ = nullptr;         // wl_event_queue*
  void* display_wrapper_ = nullptr;
  void* surface_wrapper_ = nullptr;
  void* registry_ = nullptr;
  void* presentation_ = nullptr;  // wp_presentation*, ours
  void* callback_ = nullptr;      // the outstanding wl_callback*, or null
  std::uint64_t callback_frame_id_ = 0;  // the request() that made callback_
  int clock_id_ = -1;
  std::uint32_t last_flags_ = 0;

  struct in_flight {
    void* feedback = nullptr;  // wp_presentation_feedback*
    std::uint64_t frame_id = 0;
  };
  in_flight in_flight_[max_in_flight]{};

  presentation_sample samples_[sample_capacity]{};
  std::size_t sample_head_ = 0;
  std::size_t sample_count_ = 0;
};

}  // namespace mv::gfx
