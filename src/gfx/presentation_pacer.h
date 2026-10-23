// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Frame pacing measurement for the Linux present lab (Linux PR 1).
//
//   "Verify: presents at exactly display refresh, 0 dropped frames over 60 s,
//    ~0 % CPU idle, on a real Wayland session (Mutter or KWin) with Mesa on
//    Intel or AMD, measured from wp_presentation feedback (VSYNC and
//    HW_CLOCK). Under headless Weston it runs, reports, and labels its source
//    weaker."  (docs/plans/linux.md, Linux PR 1)
//
// The portable twin of gfx/metal_pacer.h: no Wayland, EGL or GL types, so the
// arithmetic is tested on all three OSes. gfx/wayland_frame_clock turns each
// wp_presentation_feedback event into a presentation_sample; the lab hands it
// here.
//
// The display side arrives late. DXGI and the display link answer for the
// frame just presented; a Wayland compositor answers one or two refreshes
// after the commit, so a sample names the frame it is for (frame_end's id) and
// the pacer joins the two. Frames still waiting when the window closes are
// counted as unavailable, which keeps a run whose feedback stopped from
// passing on the frames it did hear about.
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "gfx/frame_histogram.h"

namespace mv::gfx {

enum class presentation_drop_source : std::uint8_t {
  none,
  presentation_feedback,  // authoritative: wp_presentation, VSYNC + HW_CLOCK, refreshes counted
  interval_heuristic,     // inferred from clock intervals; weaker, and labelled so
};

// wp_presentation_feedback.kind (presentation-time.xml). Repeated here so this
// header names no protocol header.
inline constexpr std::uint32_t k_presentation_vsync = 0x1;
inline constexpr std::uint32_t k_presentation_hw_clock = 0x2;
inline constexpr std::uint32_t k_presentation_hw_completion = 0x4;
inline constexpr std::uint32_t k_presentation_zero_copy = 0x8;

// One wp_presentation_feedback answer.
struct presentation_sample {
  std::uint64_t frame_id = 0;      // the id frame_end() was given for the commit
  bool presented = false;          // false: `discarded`, the commit was never shown
  std::uint64_t timestamp_ns = 0;  // in the wp_presentation.clock_id domain, not ours
  std::uint32_t refresh_ns = 0;    // predicted to the next refresh; 0 = unknown / variable
  std::uint64_t msc = 0;           // (seq_hi << 32) | seq_lo; 0 = no vblank counter
  std::uint32_t flags = 0;         // k_presentation_*
};

// Display-side truth for one sample, before the pacer reconciles it.
struct presentation_display_sample {
  bool valid = false;          // authoritative and a delta from the previous present
  bool baseline = false;       // authoritative, but the first of the window
  bool discontinuity = false;  // the refresh count went backwards, stood still or jumped > 1000
  bool discarded = false;
  std::uint32_t presents = 0;
  std::uint32_t missed_refreshes = 0;  // refreshes since the previous present − 1
  double presented_interval_ms = 0.0;  // this present − the previous, presentation clock
};

// True when a sample can count: the compositor says the timestamp is the
// vblank (VSYNC) taken from the hardware (HW_CLOCK), and the refreshes between
// two presents can be counted, by its vblank counter (msc) or, with none, by
// those hardware timestamps over a constant refresh. KWin sends seq 0 on every
// answer, with both flags and the refresh (Plasma 6, presentationtime.cpp).
// Headless Weston reports flags 0 and seq 0: never authoritative. Nor is seq 0
// with refresh 0 (a variable-rate output under presentation v1).
[[nodiscard]] inline bool presentation_authoritative(const presentation_sample& s) noexcept {
  const std::uint32_t need = k_presentation_vsync | k_presentation_hw_clock;
  return s.presented && (s.flags & need) == need && (s.msc != 0 || s.refresh_ns != 0);
}

// Consecutive presented msc values are the Wayland analog of DXGI's
// PresentRefreshCount: a step of 1 is a clean present, 2 is one miss. A rewind,
// a repeat or a > 1000-refresh jump is a discontinuity, not a drop.
//
// The hardware timestamps count refreshes too: round(Δt / refresh). They are
// the count where the compositor sends no msc (KWin), and they overrule an msc
// that counted fewer: Mutter steps its per-surface counter by 1 whenever the
// driver gives it no vblank sequence, so a frame shown a refresh late can
// arrive one step after the last. An msc that counted more than the clock is
// reconcile_feedback_misses()'s case below. A counter that appears or goes
// away mid-window is a discontinuity.
class presentation_statistics {
 public:
  presentation_display_sample observe(const presentation_sample& sample) noexcept;
  void reset() noexcept { have_previous_ = false; }

 private:
  bool have_previous_ = false;
  bool last_had_msc_ = false;
  std::uint64_t last_msc_ = 0;
  std::uint64_t last_timestamp_ns_ = 0;
};

// The other pacers' idle-gap rule, on the display's own clock: msc steps count
// as misses only when the presentation timestamps agree that the time passed.
// An msc that moved dozens of refreshes while the presentation clock moved one
// is a counter that kept counting or changed under us (the surface went to
// another output), not a stall. The other pacers compare against the
// application's clock; here that would hide a compositor miss, because the
// frame shown late was committed on time (its feedback comes later) — see
// docs/design/12 (2026-10-23, the presentation clock reconciles).
[[nodiscard]] inline std::uint32_t reconcile_feedback_misses(std::uint32_t msc_missed,
                                                             double presented_interval_ms,
                                                             double refresh_ms) noexcept {
  if (msc_missed == 0 || refresh_ms <= 0.0) return msc_missed;
  if (presented_interval_ms < refresh_ms * 1.5) return 0;
  return msc_missed;
}

struct presentation_pace_stats {
  std::uint64_t frames = 0;
  std::uint64_t dropped_frames = 0;
  std::uint64_t missed_refreshes = 0;
  std::uint64_t statistics_discontinuities = 0;
  std::uint64_t statistics_unavailable_frames = 0;
  std::uint64_t displayed_presents = 0;
  std::uint64_t discarded_frames = 0;   // also counted in dropped_frames
  std::uint64_t pending_feedback = 0;   // committed, not yet answered

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
  presentation_drop_source source = presentation_drop_source::none;

  // The same numeric gate as PR 1 and Mac PR 1 (gfx/pacer.h, gfx/metal_pacer.h,
  // tools/frametime/report.h), but the source must be presentation feedback.
  // A clock-only zero-drop report cannot pass.
  [[nodiscard]] bool meets_linux_pr1_gate() const noexcept {
    if (!std::isfinite(refresh_interval_ms) || refresh_interval_ms <= 0.0 ||
        !std::isfinite(elapsed_seconds) || elapsed_seconds < 60.0 ||
        !std::isfinite(mean_ms) || !std::isfinite(p50_ms) || !std::isfinite(max_ms))
      return false;
    const double expected_frames = elapsed_seconds * 1000.0 / refresh_interval_ms;
    const auto frames_d = static_cast<double>(frames);
    const auto displayed_d = static_cast<double>(displayed_presents);
    return frames_d >= expected_frames * 0.98 && frames_d <= expected_frames * 1.02 &&
           displayed_d >= expected_frames * 0.98 && displayed_d <= expected_frames * 1.02 &&
           std::abs(mean_ms - refresh_interval_ms) <= refresh_interval_ms * 0.02 &&
           std::abs(p50_ms - refresh_interval_ms) <= refresh_interval_ms * 0.02 + 0.05 &&
           max_ms <= refresh_interval_ms * 2.0 && dropped_frames == 0 &&
           missed_refreshes == 0 && source == presentation_drop_source::presentation_feedback &&
           statistics_discontinuities == 0 && statistics_unavailable_frames == 0;
  }
};

struct presentation_idle_stats {
  double elapsed_seconds = 0.0;
  double cpu_percent = -1.0;
  std::uint64_t presents = 0;
  std::uint64_t input_events = 0;
  [[nodiscard]] bool meets_linux_pr1_gate() const noexcept {
    return std::isfinite(elapsed_seconds) && elapsed_seconds >= 60.0 &&
           std::isfinite(cpu_percent) && cpu_percent >= 0.0 && cpu_percent <= 1.0 &&
           presents == 0 && input_events == 0;
  }
};

[[nodiscard]] inline const char* presentation_drop_source_label(
    presentation_drop_source s) noexcept {
  switch (s) {
    case presentation_drop_source::presentation_feedback:
      return "Wayland presentation feedback";
    case presentation_drop_source::interval_heuristic: return "clock intervals (weaker)";
    case presentation_drop_source::none:               return "no data";
  }
  return "unknown";
}

class presentation_pacer {
 public:
  void begin_session(double refresh_seconds) noexcept;
  void set_refresh(double refresh_seconds) noexcept;
  void frame_begin() noexcept;
  // After the swap that committed frame `frame_id` (any increasing number the
  // lab also tagged the feedback request with). Records the application-side
  // interval and CPU time and leaves the frame waiting for its feedback.
  void frame_end(std::uint64_t frame_id) noexcept;
  // One feedback answer. A sample for a frame from before reset_window(), or
  // for a frame this pacer is not waiting for, is ignored.
  void on_feedback(const presentation_sample& sample) noexcept;
  // The run is over and the lab has waited what it will for the stragglers:
  // every frame still waiting becomes unavailable (and its misses are
  // inferred from its clock interval, the weaker source).
  void expire_pending() noexcept;
  void reset_window() noexcept;
  [[nodiscard]] presentation_pace_stats stats() const noexcept;
  [[nodiscard]] std::size_t pending_feedback() const noexcept { return pending_count_; }
  [[nodiscard]] std::uint64_t dropped_frames_so_far() const noexcept { return dropped_frames_; }
  // The refresh the compositor last predicted in a presented sample, ns; 0
  // until one arrives or when the output has no constant rate.
  [[nodiscard]] std::uint32_t feedback_refresh_ns() const noexcept { return feedback_refresh_ns_; }
  [[nodiscard]] double refresh_seconds() const noexcept { return refresh_seconds_; }

  static constexpr std::size_t history_size = 240;
  [[nodiscard]] const std::array<float, history_size>& history() const noexcept {
    return history_;
  }
  [[nodiscard]] std::size_t history_cursor() const noexcept { return history_cursor_; }

  // Frames committed and not yet answered, kept without allocating. A
  // compositor answers within a refresh or two; one that stops answering
  // (a hidden surface may hold its feedback) pushes the oldest out as
  // unavailable rather than growing.
  static constexpr std::size_t pending_capacity = 64;

 private:
  struct pending_frame {
    std::uint64_t id = 0;
    double interval_ms = -1.0;  // < 0: the window's baseline present, no interval
  };

  void record_interval(double ms) noexcept;
  void count_unavailable(const pending_frame& frame) noexcept;
  void count_misses(std::uint32_t missed) noexcept;
  void update_source() noexcept;
  bool take_pending(std::uint64_t id, pending_frame& out) noexcept;
  [[nodiscard]] std::int64_t now_ticks() const noexcept;
  [[nodiscard]] std::int64_t ticks_per_second() const noexcept;

  frame_histogram intervals_;
  frame_histogram cpu_times_;
  std::array<float, history_size> history_{};
  std::size_t history_cursor_ = 0;

  std::array<pending_frame, pending_capacity> pending_{};
  std::size_t pending_head_ = 0;   // oldest
  std::size_t pending_count_ = 0;

  std::int64_t tick_frequency_ = 0;
  std::int64_t session_start_ticks_ = 0;
  std::int64_t last_present_ticks_ = 0;
  std::int64_t frame_begin_ticks_ = 0;

  presentation_statistics statistics_;
  std::uint64_t window_first_id_ = 0;  // samples for older frames are ignored
  bool have_window_first_ = false;
  std::uint32_t feedback_refresh_ns_ = 0;

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
  std::uint64_t discarded_frames_ = 0;
  presentation_drop_source source_ = presentation_drop_source::none;
};

}  // namespace mv::gfx
