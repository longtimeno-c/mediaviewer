// SPDX-License-Identifier: GPL-2.0-or-later
// PR 5b - the A/V clock: audio master, host-clock fallback, drift.
//
// OWNER: mediaviewer-08 (5b).
//
// The master clock is derived from samples ACTUALLY PLAYED — the endpoint's own
// position — never from a wall clock. plan/05 is explicit about why: a wall
// clock drifts slowly against the endpoint's crystal, the ramp takes minutes to
// become visible, and by the time it does it looks like a decode bug rather
// than a clock bug. The whole 30-minute verify exists to catch exactly that.
#include "player/av_clock.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "core/trace.h"

namespace mv::player {

namespace {

constexpr std::int64_t ns_per_second = 1'000'000'000;

// A step this large between two consecutive presents is not scheduling jitter,
// it is the machine having slept or hard-throttled. Presents arrive at the
// refresh interval, so two seconds is ~120 missed frames.
constexpr time_ns host_gap_threshold_ns = 2 * ns_per_second;

// std::chrono::steady_clock is the D9-clean spelling of what plan/05 calls QPC:
// QueryPerformanceCounter on MSVC, mach_absolute_time on macOS. Using it here
// rather than <windows.h> is what lets a Core Audio host reuse this file
// unchanged (tools/check-hostable-core.ps1 would reject the alternative).
[[nodiscard]] time_ns host_now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

[[nodiscard]] double percentile_of(const float* sorted, std::size_t count,
                                   double fraction) noexcept {
  if (count == 0) return 0.0;
  const auto index = static_cast<std::size_t>(fraction * static_cast<double>(count - 1) + 0.5);
  return static_cast<double>(sorted[std::min(index, count - 1)]);
}

}  // namespace

// ---------------------------------------------------------------------------
// drift_tracker
// ---------------------------------------------------------------------------

void drift_tracker::reset() noexcept {
  live_.fill(0.0f);
  horizon_.fill(0.0f);
  live_cursor_ = 0;
  horizon_count_ = 0;
  last_host_ns_ = 0;
  discontinuities_ = 0;
  host_gaps_ = 0;
}

void drift_tracker::observe(double err_ms, time_ns host_ns) noexcept {
  // `live_cursor_` counts every observation rather than wrapping, so the number
  // of valid entries is min(live_cursor_, live_size). The accessor wraps it for
  // the overlay. This is the only way to know how much of the array is real
  // without a separate counter.
  live_[live_cursor_ % live_size] = static_cast<float>(err_ms);
  ++live_cursor_;

  if (last_host_ns_ != 0 && host_ns - last_host_ns_ > host_gap_threshold_ns) {
    // Counted, not smoothed. A soak that slept mid-run is re-run, not averaged.
    ++host_gaps_;
  }
  last_host_ns_ = host_ns;

  // 1 Hz sample-and-hold into the long-horizon series. `host_ns` is nanoseconds
  // since playback start, so the bucket index IS the elapsed second and needs no
  // boundary state of its own.
  //
  // Sampling rather than averaging within the second is deliberate: the slope's
  // standard error over ~1800 samples is far below the 1 ms/min gate even with
  // several ms of per-frame noise, and sampling needs no accumulator.
  if (host_ns < 0) return;
  const auto bucket = static_cast<std::size_t>(host_ns / ns_per_second);
  if (bucket >= horizon_size) return;
  if (bucket >= horizon_count_) {
    // A gap leaves holes; fill them forward, which is what sample-and-hold means.
    for (std::size_t i = horizon_count_; i <= bucket; ++i) {
      horizon_[i] = static_cast<float>(err_ms);
    }
    horizon_count_ = bucket + 1;
  }
}

void drift_tracker::note_position_discontinuity() noexcept { ++discontinuities_; }

double drift_tracker::slope_ms_per_min() const noexcept {
  const std::size_t n = horizon_count_;
  if (n < 2) return 0.0;

  // x is the elapsed second, y is err in ms. Ordinary least squares.
  double sum_x = 0.0;
  double sum_y = 0.0;
  double sum_xy = 0.0;
  double sum_xx = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const auto x = static_cast<double>(i);
    const auto y = static_cast<double>(horizon_[i]);
    sum_x += x;
    sum_y += y;
    sum_xy += x * y;
    sum_xx += x * x;
  }
  const auto count = static_cast<double>(n);
  const double denominator = count * sum_xx - sum_x * sum_x;
  if (std::abs(denominator) < 1e-9) return 0.0;
  const double slope_per_second = (count * sum_xy - sum_x * sum_y) / denominator;
  return slope_per_second * 60.0;
}

void drift_tracker::fill(clock_stats& out) const noexcept {
  out.position_discontinuities = discontinuities_;
  out.host_clock_gaps = host_gaps_;
  out.drift_slope_ms_per_min = slope_ms_per_min();

  const std::size_t valid = std::min(live_cursor_, live_size);
  if (valid == 0) {
    out.err_ms_last = 0.0;
    out.err_ms_mean = 0.0;
    out.err_ms_p50 = 0.0;
    out.err_ms_p99 = 0.0;
    out.err_ms_min = 0.0;
    out.err_ms_max = 0.0;
    return;
  }

  // Fixed-size stack copy; no allocation, and 240 floats is a few microseconds
  // of partial sort. Percentiles come from the rolling window, matching
  // plan/05's "track drift over a rolling window".
  std::array<float, live_size> scratch{};
  std::copy_n(live_.begin(), valid, scratch.begin());
  std::sort(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(valid));

  double sum = 0.0;
  for (std::size_t i = 0; i < valid; ++i) sum += static_cast<double>(scratch[i]);

  out.err_ms_last = static_cast<double>(live_[(live_cursor_ - 1) % live_size]);
  out.err_ms_mean = sum / static_cast<double>(valid);
  out.err_ms_p50 = percentile_of(scratch.data(), valid, 0.50);
  // Both tails, signed. An error distribution skewed one way is a bias; skewed
  // both ways is jitter, and they have different causes.
  out.err_ms_min = static_cast<double>(scratch[0]);
  out.err_ms_max = static_cast<double>(scratch[valid - 1]);

  // p99 of |err| is the useful tail statistic — a large negative error is just
  // as late as a large positive one — but the signed extremes above are kept so
  // a systematic offset is still visible.
  std::array<float, live_size> magnitude{};
  for (std::size_t i = 0; i < valid; ++i) magnitude[i] = std::abs(scratch[i]);
  std::sort(magnitude.begin(), magnitude.begin() + static_cast<std::ptrdiff_t>(valid));
  out.err_ms_p99 = percentile_of(magnitude.data(), valid, 0.99);
}

const std::array<float, drift_tracker::live_size>& drift_tracker::live() const noexcept {
  return live_;
}

std::size_t drift_tracker::live_cursor() const noexcept { return live_cursor_ % live_size; }

std::span<const float> drift_tracker::horizon() const noexcept {
  return std::span<const float>(horizon_.data(), horizon_count_);
}

// ---------------------------------------------------------------------------
// av_clock
// ---------------------------------------------------------------------------

struct av_clock::impl {
  // Endpoint. `owns_sink` is false only for a test-injected sink whose lifetime
  // the caller keeps — but set_sink_for_test takes ownership, so it is true
  // there too; the flag exists for the not-started case.
  audio_sink* sink = nullptr;
  bool sink_open = false;
  time_ns retry_after_ns = 0;

  // Decode -> pump handoff. plan/02: SPSC rings of POD, nothing else.
  spsc_ring<audio_block, audio_ring_slots> ring;

  std::thread pump;
  std::atomic<bool> running{false};
  std::mutex wake_mutex;
  std::condition_variable wake;

  // Anchoring. The endpoint plays everything written to it in order and
  // contiguously, so the PTS of the sample at endpoint position P is
  // anchor_pts + (P - anchor_played). Both are re-taken on seek.
  std::atomic<time_ns> anchor_pts_ns{0};
  std::atomic<time_ns> anchor_played_ns{0};
  std::atomic<bool> anchored{false};

  // Host-clock fallback, seeded at playback start (plan/05).
  std::atomic<time_ns> host_start_ns{0};

  std::atomic<bool> audio_master{true};
  std::atomic<clock_fallback_reason> fallback{clock_fallback_reason::none};

  std::atomic<bool> paused{false};
  std::atomic<time_ns> paused_at_ns{0};
  // Silence written while paused is not a fault, so it is subtracted out of the
  // reported count rather than making a paused clip look like it underran.
  std::atomic<std::uint64_t> silence_baseline{0};

  std::atomic<double> rate{1.0};
  std::atomic<std::uint32_t> generation{0};
  std::uint32_t pump_generation = 0;
  std::atomic<std::uint64_t> device_rebuilds{0};
  std::atomic<std::uint64_t> recovery_discontinuities{0};
  std::atomic<double> resampler_ratio{1.0};

  // Written only by record_present (render thread), like gfx::pacer's members.
  drift_tracker drift;
  clock_stats current{};
  publish_slot<clock_stats> published;

  // Partially-consumed block held between pump iterations.
  audio_block pending{};
  std::uint32_t pending_offset = 0;  // frames of `pending` already accepted

  // Times the pump found nothing to give the endpoint while playing. The
  // portable audio_sink does not expose the endpoint's own underrun tally, so
  // this counts the cause rather than the effect: an empty ring at a moment the
  // endpoint needed audio is exactly what makes it write silence.
  std::atomic<std::uint64_t> starved_feeds{0};

  // Readers currently inside a sink call. rebuild_endpoint waits for this to
  // drain before closing the endpoint, so a render thread reading the clock can
  // never be holding a COM pointer the pump is releasing. The render thread only
  // ever increments and decrements — it never waits, which is the point: a
  // mutex here would be a lock the render thread takes and a worker holds.
  mutable std::atomic<int> sink_readers{0};

  // Scoped reader admission. Returns false when the endpoint is being rebuilt,
  // in which case the caller uses the host clock for this sample.
  class sink_guard {
   public:
    explicit sink_guard(const impl& owner) noexcept : owner_(owner) {
      owner_.sink_readers.fetch_add(1, std::memory_order_acq_rel);
      admitted_ = owner_.anchored.load(std::memory_order_acquire) && owner_.sink != nullptr;
      if (!admitted_) owner_.sink_readers.fetch_sub(1, std::memory_order_acq_rel);
    }
    ~sink_guard() {
      if (admitted_) owner_.sink_readers.fetch_sub(1, std::memory_order_acq_rel);
    }
    sink_guard(const sink_guard&) = delete;
    sink_guard& operator=(const sink_guard&) = delete;
    [[nodiscard]] bool admitted() const noexcept { return admitted_; }

   private:
    const impl& owner_;
    bool admitted_ = false;
  };

  [[nodiscard]] time_ns elapsed_host_ns() const noexcept {
    return host_now_ns() - host_start_ns.load(std::memory_order_relaxed);
  }

  // Both run on the pump thread. They are members of impl rather than of
  // av_clock because impl is private and a free function could not name it.
  void pump_loop() noexcept;
  void rebuild_endpoint() noexcept;
  [[nodiscard]] time_ns master_now_ns() const noexcept;

  // Overlays the live atomic-backed state onto a stats block. Called by both
  // record_present and stats(), so a caller that asks before the first frame is
  // presented still sees the truth about the master clock rather than defaults.
  void overlay_live_state(clock_stats& out) const noexcept;
};

av_clock::av_clock() noexcept : impl_(new (std::nothrow) impl) {}

av_clock::~av_clock() {
  stop();
  if (impl_ != nullptr && impl_->sink != nullptr) {
    destroy_audio_sink(impl_->sink);
    impl_->sink = nullptr;
  }
  delete impl_;
  impl_ = nullptr;
}

expected av_clock::start(std::uint32_t sample_rate, std::uint32_t channels) noexcept {
  if (impl_ == nullptr) return err(status::out_of_memory);
  if (sample_rate == 0 || channels == 0) return err(status::invalid_arg);

  stop();
  impl_->drift.reset();
  impl_->host_start_ns.store(host_now_ns(), std::memory_order_relaxed);
  impl_->anchored.store(false, std::memory_order_relaxed);
  impl_->paused.store(false, std::memory_order_relaxed);
  impl_->pending_offset = 0;
  impl_->pending.frames = 0;

  if (impl_->sink == nullptr) {
    auto created = create_audio_sink();
    if (!created) {
      // No sink at all. Not fatal: the clip still plays on the host clock.
      impl_->audio_master.store(false, std::memory_order_relaxed);
      impl_->fallback.store(clock_fallback_reason::device_open_failed,
                            std::memory_order_relaxed);
      MV_LOG_ERROR("av_clock: no audio sink; falling back to the host clock");
      return {};
    }
    impl_->sink = created.value();
  }

  if (const auto opened = impl_->sink->open(sample_rate, channels); !opened) {
    // The endpoint refused. plan/05 and the verify line both require playback to
    // continue; only the master changes.
    impl_->audio_master.store(false, std::memory_order_relaxed);
    impl_->fallback.store(clock_fallback_reason::device_open_failed, std::memory_order_relaxed);
    MV_LOG_ERROR("av_clock: endpoint open failed (%s); host clock is master",
                 status_name(opened.error()));
    impl_->retry_after_ns = host_now_ns() + ns_per_second;
    impl_->running.store(true, std::memory_order_release);
    impl_->pump = std::thread([this] { impl_->pump_loop(); });
    return {};
  }

  impl_->sink_open = true;
  impl_->audio_master.store(true, std::memory_order_relaxed);
  impl_->fallback.store(clock_fallback_reason::none, std::memory_order_relaxed);
  impl_->silence_baseline.store(0, std::memory_order_relaxed);

  impl_->running.store(true, std::memory_order_release);
  impl_->pump = std::thread([this] { impl_->pump_loop(); });
  return {};
}

void av_clock::start_host_only() noexcept {
  if (impl_ == nullptr) return;
  stop();
  impl_->drift.reset();
  impl_->host_start_ns.store(host_now_ns(), std::memory_order_relaxed);
  impl_->anchor_pts_ns.store(0, std::memory_order_relaxed);
  impl_->anchored.store(false, std::memory_order_relaxed);
  impl_->paused.store(false, std::memory_order_relaxed);
  impl_->audio_master.store(false, std::memory_order_relaxed);
  impl_->fallback.store(clock_fallback_reason::no_audio_track, std::memory_order_relaxed);
}

void av_clock::stop() noexcept {
  if (impl_ == nullptr) return;
  impl_->running.store(false, std::memory_order_release);
  impl_->wake.notify_all();
  if (impl_->pump.joinable()) impl_->pump.join();
  if (impl_->sink != nullptr && impl_->sink_open) {
    impl_->sink->close();
    impl_->sink_open = false;
  }
}

bool av_clock::submit(const audio_block& block) noexcept {
  if (impl_ == nullptr) return false;
  if (block.generation != impl_->generation.load(std::memory_order_relaxed)) {
    // Stale by the time it arrived. Accepted and dropped, not rejected: telling
    // the decoder to retry a block from before the seek would spin it.
    return true;
  }
  if (!impl_->running.load()) return true; // No working endpoint: decode may drain.
  const bool pushed = impl_->ring.try_push(block);
  if (pushed) impl_->wake.notify_one();
  return pushed;
}

void av_clock::impl::overlay_live_state(clock_stats& out) const noexcept {
  out.audio_master = audio_master.load(std::memory_order_acquire);
  out.fallback = fallback.load(std::memory_order_acquire);
  out.playback_rate = rate.load(std::memory_order_relaxed);
  out.resampler_ratio = resampler_ratio.load(std::memory_order_relaxed);
  out.counters.device_rebuilds = device_rebuilds.load(std::memory_order_relaxed);

  const sink_guard guard(*this);
  if (!guard.admitted()) return;

  out.endpoint = sink->info();
  std::uint64_t discontinuities = 0;
  out.audio_clock_ns = sink->played_ns(&discontinuities);

  const std::uint64_t silence = starved_feeds.load(std::memory_order_relaxed);
  const std::uint64_t baseline = silence_baseline.load(std::memory_order_relaxed);
  out.counters.silence_fills = silence > baseline ? silence - baseline : 0;

  // ppm of the endpoint against the host clock. Reported, never gated: a real
  // crystal is tens to hundreds of ppm off the host and that is not a defect.
  const time_ns elapsed = elapsed_host_ns();
  if (elapsed > ns_per_second && out.audio_clock_ns > 0) {
    const double audio_s = static_cast<double>(out.audio_clock_ns) / 1e9;
    const double host_s = static_cast<double>(elapsed) / 1e9;
    out.audio_vs_host_ppm = (audio_s / host_s - 1.0) * 1e6;
  }
}

time_ns av_clock::impl::master_now_ns() const noexcept {
  if (paused.load(std::memory_order_acquire)) {
    return paused_at_ns.load(std::memory_order_relaxed);
  }

  const double scale = rate.load(std::memory_order_relaxed);
  const time_ns anchor = anchor_pts_ns.load(std::memory_order_relaxed);

  if (audio_master.load(std::memory_order_acquire)) {
    const sink_guard guard(*this);
    if (guard.admitted()) {
      std::uint64_t discontinuities = 0;
      const time_ns played = sink->played_ns(&discontinuities);
      const time_ns since_anchor = played - anchor_played_ns.load(std::memory_order_relaxed);
      return anchor + static_cast<time_ns>(static_cast<double>(since_anchor) * scale);
    }
  }

  // Host master: seeded at playback start, per plan/05. A clip with no audio
  // track playing at correct speed is exactly this path — the third clause of
  // the verify line.
  return anchor + static_cast<time_ns>(static_cast<double>(elapsed_host_ns()) * scale);
}

time_ns av_clock::now_ns() const noexcept {
  return impl_ == nullptr ? 0 : impl_->master_now_ns();
}

void av_clock::set_rate(double rate) noexcept {
  if (impl_ == nullptr) return;
  const double clamped = rate > 0.0 ? std::clamp(rate, 0.25, 4.0) : 1.0;
  // Re-anchor at the current position before the rate changes, or every sample
  // already played gets retroactively re-scaled and the clock jumps.
  const time_ns current = now_ns();
  impl_->anchor_pts_ns.store(current, std::memory_order_relaxed);
  const impl::sink_guard guard(*impl_);
  if (guard.admitted()) {
    std::uint64_t ignored = 0;
    impl_->anchor_played_ns.store(impl_->sink->played_ns(&ignored), std::memory_order_relaxed);
  }
  impl_->host_start_ns.store(host_now_ns(), std::memory_order_relaxed);
  impl_->rate.store(clamped, std::memory_order_relaxed);
}

void av_clock::set_paused(bool paused) noexcept {
  if (impl_ == nullptr) return;
  if (paused == impl_->paused.load(std::memory_order_acquire)) return;

  if (paused) {
    impl_->paused_at_ns.store(now_ns(), std::memory_order_relaxed);
    impl_->paused.store(true, std::memory_order_release);
    // The endpoint keeps running and will write silence for the whole pause.
    // That silence is not an underrun, so baseline it out instead of letting a
    // paused clip report thousands of faults.
    if (impl_->sink != nullptr) {
      impl_->silence_baseline.store(impl_->starved_feeds.load(std::memory_order_relaxed),
                                   std::memory_order_relaxed);
    }
  } else {
    // Re-anchor on resume: the endpoint position advanced while we were frozen.
    impl_->anchor_pts_ns.store(impl_->paused_at_ns.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
    const impl::sink_guard guard(*impl_);
    if (guard.admitted()) {
      std::uint64_t ignored = 0;
      impl_->anchor_played_ns.store(impl_->sink->played_ns(&ignored), std::memory_order_relaxed);
      impl_->silence_baseline.store(impl_->starved_feeds.load(std::memory_order_relaxed),
                                   std::memory_order_relaxed);
    }
    impl_->host_start_ns.store(host_now_ns(), std::memory_order_relaxed);
    impl_->paused.store(false, std::memory_order_release);
    impl_->wake.notify_all();
  }
}

void av_clock::set_volume(float volume) noexcept {
  if (impl_ != nullptr && impl_->sink != nullptr) impl_->sink->set_volume(volume);
}

void av_clock::set_muted(bool muted) noexcept {
  if (impl_ != nullptr && impl_->sink != nullptr) impl_->sink->set_muted(muted);
}

void av_clock::seeked(time_ns to_ns, std::uint32_t generation) noexcept {
  if (impl_ == nullptr) return;
  impl_->generation.store(generation, std::memory_order_release);

  // Only the pump consumes the SPSC ring and touches its partial block.
  // The generation is a reset request, never cross-thread queue surgery.
  impl_->paused_at_ns.store(to_ns, std::memory_order_relaxed);
  impl_->anchor_pts_ns.store(to_ns, std::memory_order_relaxed);
  impl_->anchored.store(false, std::memory_order_release);
  impl_->host_start_ns.store(host_now_ns(), std::memory_order_relaxed);

  // The series is about steady-state drift; a seek is a deliberate
  // discontinuity, not evidence of one. Restarting it keeps a scrub from
  // showing up as a drift ramp.
  impl_->drift.reset();
}

void av_clock::record_present(const present_decision& decision, bool showed) noexcept {
  if (impl_ == nullptr) return;
  auto& stats = impl_->current;
  auto& counters = stats.counters;

  switch (decision.action) {
    case present_action::show:         ++counters.presented;    break;
    case present_action::hold_cadence: ++counters.held_cadence; break;
    case present_action::hold_starved: ++counters.held_starved; break;
    case present_action::drop:         ++counters.dropped_late; break;
  }

  const time_ns elapsed = impl_->elapsed_host_ns();

  // Only an actual present carries a meaningful A/V error: a cadence hold is
  // the same frame measured again and would weight the series towards whatever
  // the last shown frame's error was.
  if (showed) {
    impl_->drift.observe(decision.err_ms, elapsed);
  }

  // Position discontinuities are pulled through the tracker so a jump poisons
  // the reported slope's trustworthiness rather than being averaged into it.
  {
    const impl::sink_guard guard(*impl_);
    if (guard.admitted()) {
      std::uint64_t discontinuities = 0;
      (void)impl_->sink->played_ns(&discontinuities);
      for (std::uint64_t i = stats.position_discontinuities; i < discontinuities; ++i) {
        impl_->drift.note_position_discontinuity();
      }
    }
  }

  stats.video_pts_ns = decision.target_ns + static_cast<time_ns>(decision.err_ms * 1'000'000.0);
  impl_->overlay_live_state(stats);
  impl_->drift.fill(stats);
  impl_->published.publish(stats);
}

clock_stats av_clock::stats() const noexcept {
  if (impl_ == nullptr) return {};
  // Counters come from `current`, which only the render thread writes — same
  // contract as gfx::pacer::stats(): a reader may catch it mid-update, and a
  // lock here would be a lock the render thread has to take. Everything backed
  // by an atomic is re-read live, so a caller asking before the first present
  // still learns which clock is master rather than getting the defaults.
  clock_stats out = impl_->current;
  impl_->overlay_live_state(out);
  impl_->drift.fill(out);
  out.position_discontinuities += impl_->recovery_discontinuities.load();
  return out;
}

publish_slot<clock_stats>& av_clock::published() noexcept { return impl_->published; }

void av_clock::set_sink_for_test(audio_sink* sink) noexcept {
  if (impl_ == nullptr) return;
  stop();
  if (impl_->sink != nullptr) destroy_audio_sink(impl_->sink);
  impl_->sink = sink;
  impl_->sink_open = false;
}

void av_clock::impl::pump_loop() noexcept {
  auto& state = *this;

  while (state.running.load(std::memory_order_acquire)) {
    const auto requested_generation = state.generation.load(std::memory_order_acquire);
    if (state.pump_generation != requested_generation) {
      state.anchored.store(false, std::memory_order_release);
      while (state.sink_readers.load() != 0) std::this_thread::yield();
      state.pending.frames = state.pending_offset = 0;
      if (state.sink && state.sink_open) {
        const auto endpoint = state.sink->info();
        state.sink->close();
        state.sink_open = false;
        if (state.sink->open(endpoint.sample_rate, endpoint.channels)) state.sink_open = true;
        state.audio_master.store(state.sink_open);
      }
      state.pump_generation = requested_generation;
    }
    // Checked FIRST, before any of the paths below that `continue`. An endpoint
    // unplugged while the decoder happens to be starved must still be noticed —
    // the verify line is about recovery, and recovery that only fires when audio
    // is flowing is not recovery.
    if (state.sink != nullptr && (state.sink->device_changed() ||
        (!state.sink_open && host_now_ns() >= state.retry_after_ns))) {
      state.rebuild_endpoint(); state.retry_after_ns = host_now_ns() + ns_per_second;
    }

    if (state.sink) state.sink->set_paused(state.paused.load());
    if (state.paused.load(std::memory_order_acquire)) {
      std::unique_lock<std::mutex> lock(state.wake_mutex);
      state.wake.wait_for(lock, std::chrono::milliseconds(5));
      continue;
    }

    // Refill the held block if it is spent.
    if (state.pending_offset >= state.pending.frames) {
      if (!state.ring.try_pop(state.pending)) {
        // The decoder is behind. Counted here because this is where the cause
        // is visible; the endpoint's own silence write happens a period later.
        if (state.anchored.load(std::memory_order_acquire)) {
          state.starved_feeds.fetch_add(1, std::memory_order_relaxed);
        }
        // Nothing decoded yet. This thread is neither the UI nor the render
        // thread, so a bounded wait here is not plan/03 rule 1's "never Sleep"
        // — that rule governs the present loop, which is untouched by this.
        std::unique_lock<std::mutex> lock(state.wake_mutex);
        state.wake.wait_for(lock, std::chrono::milliseconds(2));
        continue;
      }
      state.pending_offset = 0;
      if (state.pending.generation != state.generation.load(std::memory_order_acquire)) {
        state.pending.frames = 0;  // stale: discard without writing it
        continue;
      }
      if (!state.sink_open) { state.pending.frames = 0; continue; }
      if (!state.anchored.load(std::memory_order_acquire) && state.sink != nullptr) {
        // The first sample of this block is the one the anchor is taken on: the
        // endpoint plays what we write, in order, so from here on
        // master = anchor_pts + (played - anchor_played).
        std::uint64_t ignored = 0;
        state.anchor_pts_ns.store(state.pending.pts_ns, std::memory_order_relaxed);
        state.anchor_played_ns.store(state.sink->played_ns(&ignored), std::memory_order_relaxed);
        state.anchored.store(true, std::memory_order_release);
      }
    }

    if (state.sink == nullptr || state.pending.frames == 0) continue;

    const std::uint32_t channels = state.pending.channels == 0 ? 1 : state.pending.channels;
    const std::uint32_t remaining = state.pending.frames - state.pending_offset;
    const std::uint32_t accepted = state.sink->write(
        state.pending.samples + static_cast<std::size_t>(state.pending_offset) * channels,
        remaining);
    state.pending_offset += accepted;

    if (accepted < remaining) {
      // Endpoint buffer full — the normal steady state, not an error. Wait for
      // roughly one engine period rather than spinning a core.
      std::unique_lock<std::mutex> lock(state.wake_mutex);
      state.wake.wait_for(lock, std::chrono::milliseconds(2));
    }
  }
}

void av_clock::impl::rebuild_endpoint() noexcept {
  auto& state = *this;
  if (state.sink == nullptr) return;

  const audio_endpoint_info previous = state.sink->info();
  const time_ns position = master_now_ns();

  // Fall back for the duration of the rebuild so now_ns() keeps advancing and
  // video does not stall waiting for a clock.
  state.audio_master.store(false, std::memory_order_release);
  state.fallback.store(clock_fallback_reason::device_lost, std::memory_order_release);
  state.anchor_pts_ns.store(position, std::memory_order_relaxed);
  state.host_start_ns.store(host_now_ns(), std::memory_order_relaxed);
  state.anchored.store(false, std::memory_order_release);

  // anchored is already false, so no new reader is admitted; wait for the ones
  // already inside to leave before the COM objects go away. This is the pump
  // thread waiting on the render thread, never the other way round.
  while (state.sink_readers.load(std::memory_order_acquire) > 0) {
    std::this_thread::yield();
  }

  state.sink->close();
  state.sink_open = false;
  state.device_rebuilds.fetch_add(1, std::memory_order_relaxed);

  const std::uint32_t endpoint_rate = previous.sample_rate != 0 ? previous.sample_rate : 48000;
  const std::uint32_t endpoint_channels = previous.channels != 0 ? previous.channels : 2;

  if (const auto opened = state.sink->open(endpoint_rate, endpoint_channels); !opened) {
    // Still gone — a genuinely unplugged device with nothing to fall back to.
    // Stay on the host clock; the next device change will try again.
    MV_LOG_ERROR("av_clock: endpoint rebuild failed (%s); staying on the host clock",
                 status_name(opened.error()));
    return;
  }

  state.sink_open = true;
  state.silence_baseline.store(state.starved_feeds.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
  state.audio_master.store(true, std::memory_order_release);
  state.fallback.store(clock_fallback_reason::none, std::memory_order_release);
  // The new endpoint's position starts from zero: the next block re-anchors,
  // and the position jump is a discontinuity by definition.
  state.recovery_discontinuities.fetch_add(1);
}

}  // namespace mv::player
