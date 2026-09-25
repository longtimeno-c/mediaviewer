// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/metal_pacer.h"

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

metal_display_sample display_link_statistics::observe(const display_link_tick& tick,
                                                      double refresh_seconds) noexcept {
  metal_display_sample out;
  if (!tick.valid) {
    have_previous_ = false;
    return out;
  }
  if (have_previous_) {
    const double dt = tick.target_seconds - last_target_;
    if (!(dt > 0.0) || !(refresh_seconds > 0.0)) {
      out.discontinuity = true;
    } else {
      const double frames = dt / refresh_seconds;
      if (frames > 1000.0) {
        out.discontinuity = true;
      } else {
        out.valid = true;
        out.presents = 1;
        const auto n = static_cast<std::uint32_t>(frames + 0.5);
        out.missed_refreshes = n > 0 ? n - 1 : 0;
        // GPU said the drawable showed up a refresh late.
        if (tick.presented_seconds > 0.0) {
          const double late = tick.presented_seconds - tick.target_seconds;
          if (late >= refresh_seconds * 0.5 && out.missed_refreshes == 0) {
            out.missed_refreshes = 1;
          }
        }
      }
    }
  }
  last_target_ = tick.target_seconds;
  have_previous_ = true;
  return out;
}

std::int64_t metal_pacer::now_ticks() const noexcept { return clock_now(); }
std::int64_t metal_pacer::ticks_per_second() const noexcept { return clock_frequency(); }

void metal_pacer::begin_session(double refresh_seconds) noexcept {
  tick_frequency_ = ticks_per_second();
  session_start_ticks_ = now_ticks();
  refresh_seconds_ = refresh_seconds > 0.0 ? refresh_seconds : 0.0;
  reset_window();
}

void metal_pacer::set_refresh(double refresh_seconds) noexcept {
  refresh_seconds_ = refresh_seconds > 0.0 ? refresh_seconds : 0.0;
}

void metal_pacer::reset_window() noexcept {
  buckets_.fill(0);
  cpu_buckets_.fill(0);
  history_.fill(0.0f);
  history_cursor_ = 0;
  last_present_ticks_ = 0;
  link_statistics_ = {};
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
  source_ = metal_drop_source::none;
  session_start_ticks_ = now_ticks();
}

void metal_pacer::frame_begin() noexcept { frame_begin_ticks_ = now_ticks(); }

void metal_pacer::record_interval(double ms) noexcept {
  last_present_to_present_ms_ = ms;
  interval_sum_ms_ += ms;
  max_ms_ = std::max(max_ms_, ms);

  auto index = static_cast<std::size_t>(ms / bucket_ms);
  if (index >= bucket_count) index = bucket_count - 1;
  ++buckets_[index];

  history_[history_cursor_] = static_cast<float>(ms);
  history_cursor_ = (history_cursor_ + 1) % history_size;
}

void metal_pacer::frame_end(const display_link_tick& tick) noexcept {
  if (tick_frequency_ == 0) begin_session(refresh_seconds_);

  const std::int64_t now = now_ticks();
  const double to_ms = 1000.0 / static_cast<double>(tick_frequency_);

  last_cpu_frame_ms_ = frame_begin_ticks_ != 0
                           ? static_cast<double>(now - frame_begin_ticks_) * to_ms
                           : 0.0;

  const auto display = link_statistics_.observe(tick, refresh_seconds_);
  std::uint32_t missed_this_frame = display.missed_refreshes;

  if (last_present_ticks_ != 0) {
    const double interval_ms = static_cast<double>(now - last_present_ticks_) * to_ms;
    record_interval(interval_ms);
    ++frames_;

    cpu_sum_ms_ += last_cpu_frame_ms_;
    cpu_max_ms_ = std::max(cpu_max_ms_, last_cpu_frame_ms_);
    auto cpu_index = static_cast<std::size_t>(last_cpu_frame_ms_ / bucket_ms);
    if (cpu_index >= bucket_count) cpu_index = bucket_count - 1;
    ++cpu_buckets_[cpu_index];

    displayed_presents_ += display.presents;
    if (display.discontinuity) ++discontinuities_;
    const double refresh_ms = refresh_seconds_ * 1000.0;
    if (!display.valid) {
      ++unavailable_frames_;
      if (refresh_ms > 0.0 && interval_ms > refresh_ms * 1.5) {
        missed_this_frame =
            static_cast<std::uint32_t>((interval_ms / refresh_ms) - 0.5);
      }
    } else {
      missed_this_frame =
          reconcile_link_misses(display.missed_refreshes, interval_ms, refresh_ms);
    }
    source_ = unavailable_frames_ == 0 ? metal_drop_source::display_link
                                       : metal_drop_source::interval_heuristic;

    if (missed_this_frame > 0) {
      ++dropped_frames_;
      missed_refreshes_ += missed_this_frame;
    }
  }

  last_present_ticks_ = now;
}

metal_pace_stats metal_pacer::stats() const noexcept {
  metal_pace_stats s;
  s.frames = frames_;
  s.dropped_frames = dropped_frames_;
  s.missed_refreshes = missed_refreshes_;
  s.statistics_discontinuities = discontinuities_;
  s.statistics_unavailable_frames = unavailable_frames_;
  s.displayed_presents = displayed_presents_;
  s.last_present_to_present_ms = last_present_to_present_ms_;
  s.last_cpu_frame_ms = last_cpu_frame_ms_;
  s.max_ms = max_ms_;
  s.refresh_interval_ms = refresh_seconds_ * 1000.0;
  s.source = source_;

  if (tick_frequency_ > 0) {
    s.elapsed_seconds = static_cast<double>(now_ticks() - session_start_ticks_) /
                        static_cast<double>(tick_frequency_);
  }

  std::uint64_t total = 0;
  for (auto count : buckets_) total += count;
  if (total == 0) return s;

  s.mean_ms = interval_sum_ms_ / static_cast<double>(total);

  const auto percentile = [total](const std::array<std::uint32_t, bucket_count>& histogram,
                                  double fraction) {
    const auto target = static_cast<std::uint64_t>(std::ceil(static_cast<double>(total) * fraction));
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < bucket_count; ++i) {
      seen += histogram[i];
      if (seen >= target) return static_cast<double>(i + 1) * bucket_ms;
    }
    return static_cast<double>(bucket_count) * bucket_ms;
  };

  s.p50_ms = percentile(buckets_, 0.50);
  s.p99_ms = percentile(buckets_, 0.99);
  s.cpu_mean_ms = cpu_sum_ms_ / static_cast<double>(total);
  s.cpu_p99_ms = percentile(cpu_buckets_, 0.99);
  s.cpu_max_ms = cpu_max_ms_;
  return s;
}

}  // namespace mv::gfx
