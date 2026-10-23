// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/presentation_pacer.h"

#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <ctime>
#endif

namespace mv::gfx {

namespace {

// The application's clock, CLOCK_MONOTONIC (QPC on Windows, where only the
// tests run this). Never the presentation clock: a compositor may stamp its
// feedback in CLOCK_MONOTONIC_RAW (headless Weston does), and the springs and
// the gate's intervals stay on one clock.
std::int64_t clock_now() noexcept {
#if defined(_WIN32)
  LARGE_INTEGER t{};
  ::QueryPerformanceCounter(&t);
  return t.QuadPart;
#else
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
#endif
}

std::int64_t clock_frequency() noexcept {
#if defined(_WIN32)
  LARGE_INTEGER f{};
  ::QueryPerformanceFrequency(&f);
  return f.QuadPart;
#else
  return 1'000'000'000;
#endif
}

}  // namespace

presentation_display_sample presentation_statistics::observe(
    const presentation_sample& sample) noexcept {
  presentation_display_sample out;
  if (!sample.presented) {
    // Never shown. Says nothing about the vblank counter, so the baseline
    // stays where it was.
    out.discarded = true;
    return out;
  }
  if (!presentation_authoritative(sample)) {
    have_previous_ = false;
    return out;
  }
  const bool has_msc = sample.msc != 0;
  if (!have_previous_) {
    out.baseline = true;
  } else if (has_msc != last_had_msc_ || (has_msc && sample.msc <= last_msc_) ||
             sample.timestamp_ns < last_timestamp_ns_) {
    out.discontinuity = true;
  } else {
    const std::uint64_t elapsed_ns = sample.timestamp_ns - last_timestamp_ns_;
    // Refreshes by the hardware clock, rounded; 0 with no constant refresh.
    const std::uint64_t clock_steps =
        sample.refresh_ns > 0 ? (elapsed_ns + sample.refresh_ns / 2) / sample.refresh_ns : 0;
    const std::uint64_t steps = has_msc ? sample.msc - last_msc_ : clock_steps;
    if (steps == 0 || steps > 1000) {
      // Two presents in one refresh (no msc to say otherwise), or a jump.
      out.discontinuity = true;
    } else {
      out.valid = true;
      out.presents = 1;
      out.missed_refreshes = static_cast<std::uint32_t>(steps - 1);
      out.presented_interval_ms = static_cast<double>(elapsed_ns) / 1.0e6;
    }
  }
  last_had_msc_ = has_msc;
  last_msc_ = sample.msc;
  last_timestamp_ns_ = sample.timestamp_ns;
  have_previous_ = true;
  return out;
}

std::int64_t presentation_pacer::now_ticks() const noexcept { return clock_now(); }
std::int64_t presentation_pacer::ticks_per_second() const noexcept { return clock_frequency(); }

void presentation_pacer::begin_session(double refresh_seconds) noexcept {
  tick_frequency_ = ticks_per_second();
  session_start_ticks_ = now_ticks();
  refresh_seconds_ = refresh_seconds > 0.0 ? refresh_seconds : 0.0;
  reset_window();
}

void presentation_pacer::set_refresh(double refresh_seconds) noexcept {
  refresh_seconds_ = refresh_seconds > 0.0 ? refresh_seconds : 0.0;
}

void presentation_pacer::reset_window() noexcept {
  intervals_.clear();
  cpu_times_.clear();
  history_.fill(0.0f);
  history_cursor_ = 0;
  last_present_ticks_ = 0;
  // Frames committed before the reset are forgotten, not expired: their
  // feedback, when it comes, belongs to the window that just closed.
  pending_head_ = 0;
  pending_count_ = 0;
  have_window_first_ = false;
  window_first_id_ = 0;
  statistics_.reset();
  interval_sum_ms_ = 0.0;
  max_ms_ = 0.0;
  cpu_sum_ms_ = 0.0;
  cpu_max_ms_ = 0.0;
  last_present_to_present_ms_ = 0.0;
  last_cpu_frame_ms_ = 0.0;
  frames_ = 0;
  dropped_frames_ = 0;
  missed_refreshes_ = 0;
  discontinuities_ = 0;
  unavailable_frames_ = 0;
  displayed_presents_ = 0;
  discarded_frames_ = 0;
  source_ = presentation_drop_source::none;
  session_start_ticks_ = now_ticks();
}

void presentation_pacer::frame_begin() noexcept { frame_begin_ticks_ = now_ticks(); }

void presentation_pacer::record_interval(double ms) noexcept {
  last_present_to_present_ms_ = ms;
  interval_sum_ms_ += ms;
  max_ms_ = std::max(max_ms_, ms);
  intervals_.add(ms);
  history_[history_cursor_] = static_cast<float>(ms);
  history_cursor_ = (history_cursor_ + 1) % history_size;
}

void presentation_pacer::count_misses(std::uint32_t missed) noexcept {
  if (missed == 0) return;
  ++dropped_frames_;
  missed_refreshes_ += missed;
}

void presentation_pacer::count_unavailable(const pending_frame& frame) noexcept {
  ++unavailable_frames_;
  const double refresh_ms = refresh_seconds_ * 1000.0;
  if (refresh_ms > 0.0 && frame.interval_ms > refresh_ms * 1.5) {
    count_misses(static_cast<std::uint32_t>((frame.interval_ms / refresh_ms) - 0.5));
  }
}

void presentation_pacer::update_source() noexcept {
  source_ = unavailable_frames_ == 0 ? presentation_drop_source::presentation_feedback
                                     : presentation_drop_source::interval_heuristic;
}

bool presentation_pacer::take_pending(std::uint64_t id, pending_frame& out) noexcept {
  for (std::size_t i = 0; i < pending_count_; ++i) {
    const std::size_t at = (pending_head_ + i) % pending_capacity;
    if (pending_[at].id != id) continue;
    out = pending_[at];
    // Feedback comes in commit order, so this is nearly always the oldest;
    // otherwise close the gap and keep the older ones waiting.
    if (i == 0) {
      pending_head_ = (pending_head_ + 1) % pending_capacity;
    } else {
      for (std::size_t j = i; j + 1 < pending_count_; ++j) {
        pending_[(pending_head_ + j) % pending_capacity] =
            pending_[(pending_head_ + j + 1) % pending_capacity];
      }
    }
    --pending_count_;
    return true;
  }
  return false;
}

void presentation_pacer::frame_end(std::uint64_t frame_id) noexcept {
  if (tick_frequency_ == 0) begin_session(refresh_seconds_);

  const std::int64_t now = now_ticks();
  const double to_ms = 1000.0 / static_cast<double>(tick_frequency_);
  last_cpu_frame_ms_ = frame_begin_ticks_ != 0
                           ? static_cast<double>(now - frame_begin_ticks_) * to_ms
                           : 0.0;

  if (!have_window_first_) {
    have_window_first_ = true;
    window_first_id_ = frame_id;
  }

  pending_frame frame;
  frame.id = frame_id;
  if (last_present_ticks_ != 0) {
    const double interval_ms = static_cast<double>(now - last_present_ticks_) * to_ms;
    record_interval(interval_ms);
    ++frames_;
    cpu_sum_ms_ += last_cpu_frame_ms_;
    cpu_max_ms_ = std::max(cpu_max_ms_, last_cpu_frame_ms_);
    cpu_times_.add(last_cpu_frame_ms_);
    frame.interval_ms = interval_ms;
  }

  if (pending_count_ == pending_capacity) {
    // The compositor has stopped answering. The oldest frame cannot be judged.
    const pending_frame oldest = pending_[pending_head_];
    pending_head_ = (pending_head_ + 1) % pending_capacity;
    --pending_count_;
    if (oldest.interval_ms >= 0.0) {
      count_unavailable(oldest);
      update_source();
    }
  }
  pending_[(pending_head_ + pending_count_) % pending_capacity] = frame;
  ++pending_count_;

  last_present_ticks_ = now;
}

void presentation_pacer::on_feedback(const presentation_sample& sample) noexcept {
  if (!have_window_first_ || sample.frame_id < window_first_id_) return;
  pending_frame frame;
  if (!take_pending(sample.frame_id, frame)) return;

  if (sample.presented && sample.refresh_ns > 0) feedback_refresh_ns_ = sample.refresh_ns;
  const presentation_display_sample display = statistics_.observe(sample);
  if (display.discontinuity) ++discontinuities_;

  if (display.discarded) {
    // Drawn and never shown: a dropped frame whatever the clock says, the
    // window's baseline frame included.
    ++discarded_frames_;
    ++dropped_frames_;
    if (frame.interval_ms >= 0.0) update_source();
    return;
  }
  // The window's first present only seeds the counter, as the first present
  // seeds the clock interval.
  if (frame.interval_ms < 0.0) return;

  if (display.valid) {
    displayed_presents_ += display.presents;
    count_misses(reconcile_feedback_misses(display.missed_refreshes,
                                           display.presented_interval_ms,
                                           refresh_seconds_ * 1000.0));
  } else {
    // Not authoritative (no VSYNC / HW_CLOCK, no way to count refreshes), a
    // discontinuity, or the first authoritative present after one: the
    // display cannot judge this frame, so the clock does, and says so.
    count_unavailable(frame);
  }
  update_source();
}

void presentation_pacer::expire_pending() noexcept {
  bool counted = false;
  for (std::size_t i = 0; i < pending_count_; ++i) {
    const pending_frame& frame = pending_[(pending_head_ + i) % pending_capacity];
    if (frame.interval_ms < 0.0) continue;
    count_unavailable(frame);
    counted = true;
  }
  pending_head_ = 0;
  pending_count_ = 0;
  if (counted) update_source();
}

presentation_pace_stats presentation_pacer::stats() const noexcept {
  presentation_pace_stats s;
  s.frames = frames_;
  s.dropped_frames = dropped_frames_;
  s.missed_refreshes = missed_refreshes_;
  s.statistics_discontinuities = discontinuities_;
  s.statistics_unavailable_frames = unavailable_frames_;
  s.displayed_presents = displayed_presents_;
  s.discarded_frames = discarded_frames_;
  s.pending_feedback = pending_count_;
  s.last_present_to_present_ms = last_present_to_present_ms_;
  s.last_cpu_frame_ms = last_cpu_frame_ms_;
  s.max_ms = max_ms_;
  s.refresh_interval_ms = refresh_seconds_ * 1000.0;
  s.source = source_;

  if (tick_frequency_ > 0) {
    s.elapsed_seconds = static_cast<double>(now_ticks() - session_start_ticks_) /
                        static_cast<double>(tick_frequency_);
  }

  const std::uint64_t total = intervals_.count();
  if (total == 0) return s;

  s.mean_ms = interval_sum_ms_ / static_cast<double>(total);
  s.p50_ms = intervals_.percentile(0.50);
  s.p99_ms = intervals_.percentile(0.99);
  s.cpu_mean_ms = cpu_sum_ms_ / static_cast<double>(total);
  s.cpu_p99_ms = cpu_times_.percentile(0.99);
  s.cpu_max_ms = cpu_max_ms_;
  return s;
}

}  // namespace mv::gfx
