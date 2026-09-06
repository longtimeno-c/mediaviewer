// SPDX-License-Identifier: GPL-2.0-or-later
// Frame pacing measurement — PR 1's verify line, made falsifiable.
//
//   "Verify: presents at exactly display refresh, 0 dropped frames over 60 s,
//    ~0 % CPU idle."  (plan/10-roadmap.md, PR 1)
//
// Two independent measurements, because either one alone lies:
//
//   1. QPC present-to-present intervals. Cheap, always available, and what the
//      F3 overlay shows. But a present that the compositor silently held for an
//      extra refresh can still look like a clean interval from inside the app.
//
//   2. DXGI_FRAME_STATISTICS.SyncRefreshCount. This is the authoritative one:
//      it counts vblanks, so a delta greater than one refresh per present IS a
//      dropped frame, measured by the display rather than inferred by us.
//      Composition swapchains do not always provide it; when they do not, we
//      fall back to (1) with a 1.5x-refresh threshold and SAY SO, because a
//      green number that proves nothing is worse than no number.
//
// plan/09: "Treat a dropped frame as a test failure, not a nuisance."
#pragma once

#include <dxgi1_6.h>

#include <array>
#include <cstdint>

namespace mv::gfx {

enum class drop_source : std::uint8_t {
  none,               // no frames presented yet
  frame_statistics,   // authoritative: DXGI told us vblanks were missed
  interval_heuristic, // inferred from QPC intervals; weaker, and labelled so
};

struct pace_stats {
  std::uint64_t frames = 0;
  std::uint64_t dropped_frames = 0;   // presents that missed at least one vblank
  std::uint64_t missed_refreshes = 0; // total vblanks missed (a 3-refresh stall counts 2)

  // Samples where DXGI's counters moved by an implausible amount — the swapchain
  // was recreated, the counters had not started, or the scheduler resynced.
  // Counted rather than folded into dropped_frames, because a discontinuity is
  // not a dropped frame and silently treating it as one turns the gate into
  // noise. A soak with many of these is a measurement to distrust.
  std::uint64_t statistics_discontinuities = 0;

  double last_present_to_present_ms = 0.0;
  double last_cpu_frame_ms = 0.0;

  double mean_ms = 0.0;
  double p50_ms = 0.0;
  double p99_ms = 0.0;
  double max_ms = 0.0;

  // Time spent inside our own frame: wait returns, we record, we Present. This
  // is what separates "we were late" from "the scheduler took our slice". If
  // cpu_max stays far under the refresh interval while frames still drop, the
  // app is not the problem and the machine is.
  double cpu_mean_ms = 0.0;
  double cpu_p99_ms = 0.0;
  double cpu_max_ms = 0.0;

  double refresh_interval_ms = 0.0;
  double elapsed_seconds = 0.0;

  drop_source source = drop_source::none;

  // The verify line, evaluated: presenting at refresh with nothing dropped.
  [[nodiscard]] bool meets_pr1_gate() const noexcept {
    return frames > 0 && dropped_frames == 0 && source == drop_source::frame_statistics &&
           statistics_discontinuities == 0;
  }
};

class pacer {
 public:
  // `refresh_seconds` comes from the swapchain's containing output and is
  // re-supplied whenever the window moves monitors (plan/03).
  void begin_session(double refresh_seconds) noexcept;
  void set_refresh(double refresh_seconds) noexcept;

  // Called at the top of the frame, right after the waitable object returns.
  void frame_begin() noexcept;

  // Called immediately after Present returns. `swapchain` may be null, in which
  // case only the QPC path runs.
  void frame_end(IDXGISwapChain* swapchain) noexcept;

  // Drops the accumulated histogram but keeps the refresh interval. Used when
  // the app leaves and re-enters the active present state, so an idle gap is
  // not scored as a stall.
  void reset_window() noexcept;

  [[nodiscard]] pace_stats stats() const noexcept;

  // Rolling history for the overlay graph, oldest to newest, in milliseconds.
  static constexpr std::size_t history_size = 240;
  [[nodiscard]] const std::array<float, history_size>& history() const noexcept {
    return history_;
  }
  [[nodiscard]] std::size_t history_cursor() const noexcept { return history_cursor_; }

 private:
  void record_interval(double ms) noexcept;

  // 0.05 ms buckets up to 60 ms, plus an overflow bucket. Fixed-size so the
  // render thread never allocates.
  static constexpr std::size_t bucket_count = 1201;
  static constexpr double bucket_ms = 0.05;

  std::array<std::uint32_t, bucket_count> buckets_{};
  std::array<std::uint32_t, bucket_count> cpu_buckets_{};
  std::array<float, history_size> history_{};
  std::size_t history_cursor_ = 0;

  std::int64_t qpc_frequency_ = 0;
  std::int64_t session_start_qpc_ = 0;
  std::int64_t last_present_qpc_ = 0;
  std::int64_t frame_begin_qpc_ = 0;

  // DXGI frame-statistics tracking.
  std::uint32_t last_present_count_ = 0;
  std::uint32_t last_sync_refresh_count_ = 0;
  bool have_last_statistics_ = false;

  double refresh_seconds_ = 1.0 / 60.0;
  double interval_sum_ms_ = 0.0;
  double max_ms_ = 0.0;
  double cpu_sum_ms_ = 0.0;
  double cpu_max_ms_ = 0.0;
  double last_present_to_present_ms_ = 0.0;
  double last_cpu_frame_ms_ = 0.0;

  std::uint64_t frames_ = 0;
  std::uint64_t dropped_frames_ = 0;
  std::uint64_t missed_refreshes_ = 0;
  std::uint64_t discontinuities_ = 0;
  drop_source source_ = drop_source::none;
};

}  // namespace mv::gfx
