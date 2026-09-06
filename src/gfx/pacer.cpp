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

void pacer::begin_session(double refresh_seconds) noexcept {
  qpc_frequency_ = qpc_frequency();
  session_start_qpc_ = qpc_now();
  refresh_seconds_ = refresh_seconds > 0.0 ? refresh_seconds : 1.0 / 60.0;
  reset_window();
}

void pacer::set_refresh(double refresh_seconds) noexcept {
  if (refresh_seconds > 0.0) refresh_seconds_ = refresh_seconds;
}

void pacer::reset_window() noexcept {
  buckets_.fill(0);
  cpu_buckets_.fill(0);
  history_.fill(0.0f);
  history_cursor_ = 0;
  last_present_qpc_ = 0;
  have_last_statistics_ = false;
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

  std::uint32_t missed_this_frame = 0;

  // --- Authoritative path: vblanks, counted by DXGI ---------------------
  bool statistics_valid = false;
  bool discontinuity = false;
  if (swapchain != nullptr) {
    DXGI_FRAME_STATISTICS fs{};
    if (SUCCEEDED(swapchain->GetFrameStatistics(&fs))) {
      // A zeroed sample is not a measurement. GetFrameStatistics succeeds
      // before the first present has been scheduled and hands back an
      // all-zero struct; treating that as a baseline makes the NEXT sample
      // look like a million missed vblanks, which is exactly the nonsense a
      // gate must not report.
      const bool populated = fs.PresentCount != 0 || fs.SyncRefreshCount != 0;

      if (have_last_statistics_ && populated) {
        // Unsigned wraparound is intentional: these counters are UINT and roll.
        const std::uint32_t presents = fs.PresentCount - last_present_count_;
        const std::uint32_t refreshes = fs.SyncRefreshCount - last_sync_refresh_count_;

        // A single frame cannot plausibly miss this many vblanks. Beyond it,
        // the counters were reset or resynced rather than the display starving.
        constexpr std::uint32_t implausible = 1000;

        if (presents == 0) {
          // No newly DISPLAYED present since the last sample. PresentCount
          // advances when the compositor shows a frame, not when Present()
          // returns, so a same-value sample is the common case rather than a
          // fault — and counting it as one made two thirds of a clean soak
          // look like a measurement failure.
          //
          // Nothing is lost by skipping: the counters are cumulative, so a
          // vblank missed now shows up in the next sample where PresentCount
          // does move, as refreshes exceeding presents.
          statistics_valid = true;
        } else if (presents > implausible || refreshes > implausible) {
          discontinuity = true;
        } else {
          statistics_valid = true;
          // One present should consume exactly one refresh. Anything above that
          // is a vblank the display showed without a new frame from us.
          if (refreshes > presents) missed_this_frame = refreshes - presents;
        }
      }

      if (populated) {
        last_present_count_ = fs.PresentCount;
        last_sync_refresh_count_ = fs.SyncRefreshCount;
        have_last_statistics_ = true;
      }
    }
  }

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

    if (discontinuity) {
      ++discontinuities_;
      source_ = drop_source::frame_statistics;
    } else if (statistics_valid) {
      source_ = drop_source::frame_statistics;
    } else {
      // Fallback, and it is labelled as such in pace_stats::source so nobody
      // reads it as the D6 gate. 1.5x refresh is the conventional threshold:
      // above it, the only explanation is a missed vblank.
      source_ = drop_source::interval_heuristic;
      const double refresh_ms = refresh_seconds_ * 1000.0;
      if (refresh_ms > 0.0 && interval_ms > refresh_ms * 1.5) {
        missed_this_frame =
            static_cast<std::uint32_t>((interval_ms / refresh_ms) - 0.5);
      }
    }

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
    const auto target = static_cast<std::uint64_t>(static_cast<double>(total) * fraction);
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
