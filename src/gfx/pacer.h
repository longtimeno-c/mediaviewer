// SPDX-License-Identifier: GPL-2.0-or-later
// Frame pacing measurement — PR 1's verify line, made falsifiable.
//
//   "Verify: presents at exactly display refresh, 0 dropped frames over 60 s,
//    ~0 % CPU idle."  (plan/10-roadmap.md, PR 1)
//
// QPC measures application cadence; PresentCount/PresentRefreshCount measure
// displayed progress. Missing display statistics invalidate the whole window.
#pragma once

#include <dxgi1_6.h>

#include <array>
#include <cmath>
#include <cstdint>

namespace mv::gfx {

enum class drop_source : std::uint8_t {
  none,               // no frames presented yet
  frame_statistics,   // authoritative: DXGI told us vblanks were missed
  interval_heuristic, // inferred from QPC intervals; weaker, and labelled so
};

struct display_sample {
  bool valid = false;
  bool discontinuity = false;
  std::uint32_t presents = 0;
  std::uint32_t missed_refreshes = 0;
};

// Pure counter tracking, independently testable without a GPU or a clock.
class display_statistics {
 public:
  display_sample observe(HRESULT result, const DXGI_FRAME_STATISTICS& sample) noexcept;
 private:
  bool have_previous_ = false;
  std::uint32_t present_count_ = 0;
  std::uint32_t refresh_count_ = 0;
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
  std::uint64_t statistics_unavailable_frames = 0;
  std::uint64_t displayed_presents = 0;

  double last_present_to_present_ms = 0.0;
  double last_cpu_frame_ms = 0.0;

  double mean_ms = 0.0;
  double p50_ms = 0.0;
  double p99_ms = 0.0;
  double max_ms = 0.0;

  // Wall time from frame begin through Present, excluding the waitable wait.
  // Includes scheduling delays; this is not process CPU utilization.
  double cpu_mean_ms = 0.0;
  double cpu_p99_ms = 0.0;
  double cpu_max_ms = 0.0;

  double refresh_interval_ms = 0.0;
  double elapsed_seconds = 0.0;

  drop_source source = drop_source::none;

  // 60 measured seconds; 2% cadence tolerance plus histogram quantization.
  [[nodiscard]] bool meets_pr1_gate() const noexcept {
    if (!std::isfinite(refresh_interval_ms) || refresh_interval_ms <= 0.0 ||
        !std::isfinite(elapsed_seconds) || elapsed_seconds < 60.0 ||
        !std::isfinite(mean_ms) || !std::isfinite(p50_ms) || !std::isfinite(max_ms)) return false;
    const double expected_frames = elapsed_seconds * 1000.0 / refresh_interval_ms;
    return frames >= expected_frames * 0.98 && frames <= expected_frames * 1.02 &&
           displayed_presents >= expected_frames * 0.98 &&
           displayed_presents <= expected_frames * 1.02 &&
           std::abs(mean_ms - refresh_interval_ms) <= refresh_interval_ms * 0.02 &&
           std::abs(p50_ms - refresh_interval_ms) <= refresh_interval_ms * 0.02 + 0.05 &&
           max_ms <= refresh_interval_ms * 2.0 && dropped_frames == 0 &&
           missed_refreshes == 0 && source == drop_source::frame_statistics &&
           statistics_discontinuities == 0 && statistics_unavailable_frames == 0;
  }
};

struct idle_stats {
  double elapsed_seconds = 0.0;
  // Process kernel + user time / wall time, as a percentage of ONE CPU core.
  double cpu_percent = -1.0;
  std::uint64_t presents = 0;
  std::uint64_t input_events = 0;
  [[nodiscard]] bool meets_pr1_gate() const noexcept {
    return std::isfinite(elapsed_seconds) && elapsed_seconds >= 60.0 &&
           std::isfinite(cpu_percent) && cpu_percent >= 0.0 && cpu_percent <= 1.0 &&
           presents == 0 && input_events == 0;
  }
};

class pacer {
 public:
  // `refresh_seconds` comes from the host monitor's active display path and is
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
  display_statistics display_statistics_;

  double refresh_seconds_ = 0.0;
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
  std::uint64_t unavailable_frames_ = 0;
  std::uint64_t displayed_presents_ = 0;
  drop_source source_ = drop_source::none;
};

}  // namespace mv::gfx
