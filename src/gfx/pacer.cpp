// SPDX-License-Identifier: GPL-2.0-or-later
#include "gfx/pacer.h"

#include <windows.h>

#include <algorithm>

#include "core/trace.h"

namespace mv::gfx {

namespace {

std::int64_t qpc_now() noexcept {
  LARGE_INTEGER t{};
  ::QueryPerformanceCounter(&t);
  return t.QuadPart;
}

std::int64_t qpc_frequency() noexcept {
  LARGE_INTEGER f{};
  ::QueryPerformanceFrequency(&f);
  return f.QuadPart;
}

}  // namespace

display_sample display_statistics::observe(HRESULT result,
                                            const DXGI_FRAME_STATISTICS& sample) noexcept {
  display_sample out;
  if (FAILED(result)) {
    out.discontinuity = result == DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
    have_previous_ = false;
    return out;
  }
  if (sample.PresentCount == 0 && sample.PresentRefreshCount == 0) {
    have_previous_ = false;
    return out;
  }
  if (have_previous_) {
    // UINT counters wrap naturally. Resets look like implausibly large deltas.
    const auto presents = sample.PresentCount - present_count_;
    const auto refreshes = sample.PresentRefreshCount - refresh_count_;
    if (presents == 0) {
      // Preserve the previous *presentation* baseline on duplicate polls.
      out.valid = refreshes == 0;
      out.discontinuity = refreshes != 0;
      return out;
    }
    if (presents > 1000 || refreshes > 1000 || refreshes < presents) {
      out.discontinuity = true;
    } else {
      out.valid = true;
      out.presents = presents;
      out.missed_refreshes = refreshes - presents;
    }
  }
  present_count_ = sample.PresentCount;
  refresh_count_ = sample.PresentRefreshCount;
  have_previous_ = true;
  return out;
}

void pacer::begin_session(double refresh_seconds) noexcept {
  qpc_frequency_ = qpc_frequency();
  session_start_qpc_ = qpc_now();
  refresh_seconds_ = refresh_seconds > 0.0 ? refresh_seconds : 0.0;
  reset_window();
}

void pacer::set_refresh(double refresh_seconds) noexcept {
  refresh_seconds_ = refresh_seconds > 0.0 ? refresh_seconds : 0.0;
}

void pacer::reset_window() noexcept {
  buckets_.fill(0);
  cpu_buckets_.fill(0);
  history_.fill(0.0f);
  history_cursor_ = 0;
  last_present_qpc_ = 0;
  display_statistics_ = {};
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
  source_ = drop_source::none;
  session_start_qpc_ = qpc_now();
}

void pacer::frame_begin() noexcept { frame_begin_qpc_ = qpc_now(); }

void pacer::record_interval(double ms) noexcept {
  last_present_to_present_ms_ = ms;
  interval_sum_ms_ += ms;
  max_ms_ = std::max(max_ms_, ms);

  auto index = static_cast<std::size_t>(ms / bucket_ms);
  if (index >= bucket_count) index = bucket_count - 1;
  ++buckets_[index];

  history_[history_cursor_] = static_cast<float>(ms);
  history_cursor_ = (history_cursor_ + 1) % history_size;
}

void pacer::frame_end(IDXGISwapChain* swapchain) noexcept {
  if (qpc_frequency_ == 0) begin_session(refresh_seconds_);

  const std::int64_t now = qpc_now();
  const double to_ms = 1000.0 / static_cast<double>(qpc_frequency_);

  last_cpu_frame_ms_ = frame_begin_qpc_ != 0
                           ? static_cast<double>(now - frame_begin_qpc_) * to_ms
                           : 0.0;

  DXGI_FRAME_STATISTICS fs{};
  const HRESULT result = swapchain ? swapchain->GetFrameStatistics(&fs) : E_FAIL;
  const auto display = display_statistics_.observe(result, fs);
  std::uint32_t missed_this_frame = display.missed_refreshes;

  // --- QPC interval, always ----------------------------------------------
  if (last_present_qpc_ != 0) {
    const double interval_ms = static_cast<double>(now - last_present_qpc_) * to_ms;
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
          reconcile_missed_refreshes(display.missed_refreshes, interval_ms, refresh_ms);
    }
    // A later valid sample must not erase earlier gaps in coverage.
    source_ = unavailable_frames_ == 0 ? drop_source::frame_statistics
                                       : drop_source::interval_heuristic;

    if (missed_this_frame > 0) {
      ++dropped_frames_;
      missed_refreshes_ += missed_this_frame;
      trace::frame_dropped(frames_, missed_this_frame);
    }
    trace::frame_present(frames_, interval_ms, last_cpu_frame_ms_, missed_this_frame);
  }

  last_present_qpc_ = now;
}

pace_stats pacer::stats() const noexcept {
  pace_stats s;
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

  if (qpc_frequency_ > 0) {
    s.elapsed_seconds = static_cast<double>(qpc_now() - session_start_qpc_) /
                        static_cast<double>(qpc_frequency_);
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
      if (seen >= target) {
        // Report the bucket's upper edge: percentiles should never flatter.
        return static_cast<double>(i + 1) * bucket_ms;
      }
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
