// SPDX-License-Identifier: GPL-2.0-or-later
// Frame pacing measurement for the Metal present lab (PR 16).
//
//   "Verify: presents at exactly display refresh, 0 dropped frames over 60 s,
//    ~0 % CPU idle, on Apple Silicon, measured from the Metal / display-link
//    side."  (plan/10-roadmap.md, PR 16)
//
// A Windows DXGI soak is not this verify. This file has no Metal and no DXGI
// types so the arithmetic can be tested on a Windows box. The Darwin lab feeds
// it CAMetalDisplayLink target timestamps.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace mv::gfx {

enum class metal_drop_source : std::uint8_t {
  none,
  display_link,         // authoritative: CAMetalDisplayLink target timestamps
  interval_heuristic,   // inferred from clock intervals; weaker, and labelled so
};

struct metal_display_sample {
  bool valid = false;
  bool discontinuity = false;
  std::uint32_t presents = 0;
  std::uint32_t missed_refreshes = 0;
};

struct display_link_tick {
  bool valid = false;
  // CAMetalDisplayLinkUpdate.targetTimestamp, seconds in CACurrentMediaTime.
  double target_seconds = 0.0;
  // drawable.presentedTime when the GPU reports it; 0 means unknown.
  double presented_seconds = 0.0;
};

// Consecutive target timestamps are the Metal analog of PresentRefreshCount.
// A 1-refresh step is a clean present; a 2-refresh step is one miss. A rewind
// or a >1000-refresh jump is a discontinuity, not a drop.
class display_link_statistics {
 public:
  metal_display_sample observe(const display_link_tick& tick,
                               double refresh_seconds) noexcept;

 private:
  bool have_previous_ = false;
  double last_target_ = 0.0;
};

// Same idle-gap rule as the DXGI pacer: a compositor that kept counting while
// we were paused must not turn the first resume interval into dozens of misses
// when the clock says one refresh.
[[nodiscard]] inline std::uint32_t reconcile_link_misses(
    std::uint32_t link_missed, double interval_ms, double refresh_ms) noexcept {
  if (link_missed == 0 || refresh_ms <= 0.0) return link_missed;
  if (interval_ms < refresh_ms * 1.5) return 0;
  return link_missed;
}

struct metal_pace_stats {
  std::uint64_t frames = 0;
  std::uint64_t dropped_frames = 0;
  std::uint64_t missed_refreshes = 0;
  std::uint64_t statistics_discontinuities = 0;
  std::uint64_t statistics_unavailable_frames = 0;
  std::uint64_t displayed_presents = 0;

  double last_present_to_present_ms = 0.0;
  double last_cpu_frame_ms = 0.0;
  double mean_ms = 0.0;
  double p50_ms = 0.0;
  double p99_ms = 0.0;
  double max_ms = 0.0;
  double cpu_mean_ms = 0.0;
  double cpu_p99_ms = 0.0;
  double cpu_max_ms = 0.0;
  double refresh_interval_ms = 0.0;
  double elapsed_seconds = 0.0;
  metal_drop_source source = metal_drop_source::none;

  // Same numeric gate as PR 1, but the source must be the display link.
  // A QPC-only zero-drop report cannot pass.
  [[nodiscard]] bool meets_pr16_gate() const noexcept {
    if (!std::isfinite(refresh_interval_ms) || refresh_interval_ms <= 0.0 ||
        !std::isfinite(elapsed_seconds) || elapsed_seconds < 60.0 ||
        !std::isfinite(mean_ms) || !std::isfinite(p50_ms) || !std::isfinite(max_ms))
      return false;
    const double expected_frames = elapsed_seconds * 1000.0 / refresh_interval_ms;
    return frames >= expected_frames * 0.98 && frames <= expected_frames * 1.02 &&
           displayed_presents >= expected_frames * 0.98 &&
           displayed_presents <= expected_frames * 1.02 &&
           std::abs(mean_ms - refresh_interval_ms) <= refresh_interval_ms * 0.02 &&
           std::abs(p50_ms - refresh_interval_ms) <= refresh_interval_ms * 0.02 + 0.05 &&
           max_ms <= refresh_interval_ms * 2.0 && dropped_frames == 0 &&
           missed_refreshes == 0 && source == metal_drop_source::display_link &&
           statistics_discontinuities == 0 && statistics_unavailable_frames == 0;
  }
};

struct metal_idle_stats {
  double elapsed_seconds = 0.0;
  double cpu_percent = -1.0;
  std::uint64_t presents = 0;
  std::uint64_t input_events = 0;
  [[nodiscard]] bool meets_pr16_gate() const noexcept {
    return std::isfinite(elapsed_seconds) && elapsed_seconds >= 60.0 &&
           std::isfinite(cpu_percent) && cpu_percent >= 0.0 && cpu_percent <= 1.0 &&
           presents == 0 && input_events == 0;
  }
};

[[nodiscard]] inline const char* metal_drop_source_label(metal_drop_source s) noexcept {
  switch (s) {
    case metal_drop_source::display_link:       return "Metal display-link";
    case metal_drop_source::interval_heuristic: return "clock intervals (weaker)";
    case metal_drop_source::none:               return "no data";
  }
  return "unknown";
}

class metal_pacer {
 public:
  void begin_session(double refresh_seconds) noexcept;
  void set_refresh(double refresh_seconds) noexcept;
  void frame_begin() noexcept;
  // `tick.valid == false` is the "display-link sample unavailable" case: the
  // pacer falls back to the clock and says so. meets_pr16_gate() requires the
  // authoritative source.
  void frame_end(const display_link_tick& tick) noexcept;
  void reset_window() noexcept;
  [[nodiscard]] metal_pace_stats stats() const noexcept;

  static constexpr std::size_t history_size = 240;
  [[nodiscard]] const std::array<float, history_size>& history() const noexcept {
    return history_;
  }
  [[nodiscard]] std::size_t history_cursor() const noexcept { return history_cursor_; }

 private:
  void record_interval(double ms) noexcept;
  [[nodiscard]] std::int64_t now_ticks() const noexcept;
  [[nodiscard]] std::int64_t ticks_per_second() const noexcept;

  static constexpr std::size_t bucket_count = 1201;
  static constexpr double bucket_ms = 0.05;

  std::array<std::uint32_t, bucket_count> buckets_{};
  std::array<std::uint32_t, bucket_count> cpu_buckets_{};
  std::array<float, history_size> history_{};
  std::size_t history_cursor_ = 0;

  std::int64_t tick_frequency_ = 0;
  std::int64_t session_start_ticks_ = 0;
  std::int64_t last_present_ticks_ = 0;
  std::int64_t frame_begin_ticks_ = 0;

  display_link_statistics link_statistics_;

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
  metal_drop_source source_ = metal_drop_source::none;
};

}  // namespace mv::gfx
