// SPDX-License-Identifier: GPL-2.0-or-later
// PR 5b — the A/V clock. plan/05: "Budget 2-4 weeks and treat it as a
// first-class subsystem, not glue."
//
// Audio is the master. Video presents against it. When there is no audio the
// master is a monotonic host clock seeded at playback start — plan/05 calls
// that "QPC", and std::chrono::steady_clock is the D9-clean spelling of it
// (QueryPerformanceCounter on MSVC, mach_absolute_time on macOS). That is not
// a deviation from the plan.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/spsc_ring.h"
#include "player/audio_block.h"
#include "player/audio_sink.h"
#include "player/presenter.h"
#include "player/video_source.h"

namespace mv::player {

// Why a frame was not shown once, in the order the presenter decides them.
// dropped_late and held_starved are FAULTS. held_cadence is NOT: showing one
// frame for two vblanks is the correct, normal result of 24p or 30p on a 60 Hz
// display (3:2 pulldown). Counting them together makes 24p-on-60Hz read as
// permanently broken, so they are separate numbers and always will be.
struct present_counters {
  std::uint64_t presented     = 0;
  std::uint64_t dropped_late  = 0;  // fault: frame missed its deadline by > 1 interval
  std::uint64_t held_cadence  = 0;  // normal: source fps < refresh
  std::uint64_t held_starved  = 0;  // fault: queue empty, held previous frame
  std::uint64_t silence_fills = 0;  // fault: audio underrun, wrote silence
  std::uint64_t device_rebuilds = 0;
};

// Published wait-free via core/spsc_ring.h publish_slot<clock_stats>: the audio
// thread publishes, the render thread and the F3 overlay acquire. POD only —
// player/ never makes an ImGui call; all ImGui stays in shell/present_lab.cpp.
struct clock_stats {
  bool                  audio_master = true;
  clock_fallback_reason fallback     = clock_fallback_reason::none;
  decoder_kind          decoder      = decoder_kind::none;

  audio_endpoint_info   endpoint{};

  time_ns audio_clock_ns = 0;
  time_ns video_pts_ns   = 0;

  // Instantaneous A/V error at present: err = video_pts - audio_clock.
  // Positive = video ahead. NOTE: this is flat BY CONSTRUCTION — the presenter
  // drops and holds precisely to force it flat — so a flat err_ms graph proves
  // nothing on its own. The evidence that the clock is right is the SLOPE
  // below plus counters that stop growing.
  double err_ms_last = 0.0;
  double err_ms_mean = 0.0;
  double err_ms_p50  = 0.0;
  double err_ms_p99  = 0.0;
  double err_ms_min  = 0.0;  // signed, both tails reported
  double err_ms_max  = 0.0;

  // The actual gate: least-squares slope of err over the long-horizon series.
  double drift_slope_ms_per_min = 0.0;

  // Diagnostic, NOT a gate. A real endpoint crystal genuinely differs from the
  // host clock by tens to hundreds of ppm; that is not a bug. Reported so an
  // off-rate endpoint or a wrong resampler ratio is distinguishable from a
  // clock defect instead of being blamed on one.
  double audio_vs_host_ppm = 0.0;
  double resampler_ratio   = 1.0;

  double playback_rate = 1.0;  // 0.25 .. 4.0, owned by 5c, in the clock from day one

  // Fixed constant, reported not tuned. plan/03 has the render loop wait on the
  // frame-latency waitable BEFORE recording, so an audio_clock sampled there is
  // already ~one refresh stale by the time the pixels are photons. The honest
  // target is audio_clock + one vblank + present-to-photon; we implement one
  // vblank exactly as plan/05 says and report the residual here. A fixed
  // non-zero p50 err is explained by this, not a defect.
  double present_bias_ms = 0.0;

  present_counters counters{};

  // Non-zero means the measurement above is not trustworthy — same contract as
  // gfx::pacer's statistics_discontinuities. A soak that ends with these
  // non-zero does not pass; it gets re-run.
  std::uint64_t position_discontinuities = 0;
  std::uint64_t host_clock_gaps          = 0;  // machine slept / power-throttled mid-soak
};

// Where "flat drift" is actually measured. No thread, no device, no endpoint,
// no allocation — which is exactly why it is the piece that gets tested
// headlessly, and why the 30-minute verify has a testable core at all.
//
// Two series on purpose. The per-present ring feeds the live graph; the 1 Hz
// long-horizon series is what the slope is fitted over. 240 presents is about
// four seconds and physically cannot show a 30-minute ramp, which is the whole
// thing we are trying to catch.
class drift_tracker {
 public:
  static constexpr std::size_t live_size    = 240;   // gfx::pacer::history_size idiom
  static constexpr std::size_t horizon_size = 2048;  // 1 Hz -> ~34 minutes

  void reset() noexcept;

  // [render-thread] One presented frame. `host_ns` is a monotonic host reading;
  // a jump in it means the machine slept or power-throttled, which poisons the
  // run and is counted rather than averaged over.
  void observe(double err_ms, time_ns host_ns) noexcept;
  void note_position_discontinuity() noexcept;

  // Least-squares slope over the long-horizon series, ms per minute. THE gate.
  // The instantaneous error is flat by construction — the presenter drops and
  // holds to force it flat — so only this slope distinguishes a correct clock
  // from a broken position query.
  [[nodiscard]] double slope_ms_per_min() const noexcept;

  void fill(clock_stats& out) const noexcept;

  [[nodiscard]] const std::array<float, live_size>& live() const noexcept;
  [[nodiscard]] std::size_t live_cursor() const noexcept;
  [[nodiscard]] std::span<const float> horizon() const noexcept;  // the CSV

 private:
  std::array<float, live_size>    live_{};
  std::array<float, horizon_size> horizon_{};
  std::size_t   live_cursor_    = 0;
  std::size_t   horizon_count_  = 0;
  time_ns       last_host_ns_   = 0;
  std::uint64_t discontinuities_ = 0;
  std::uint64_t host_gaps_       = 0;
};

// Owns the audio sink, the audio thread, the block ring and the drift series.
//
// The audio thread touches ONLY the ring and the sink: never FFmpeg, never a
// lock a decode worker holds, never an allocation (CLAUDE.md rule 1). Starved,
// it writes silence and counts it rather than stalling.
class av_clock {
 public:
  av_clock() noexcept;
  ~av_clock();

  av_clock(const av_clock&) = delete;
  av_clock& operator=(const av_clock&) = delete;

  // Opens the endpoint and starts the audio thread. An endpoint that will not
  // open still returns ok and falls back to the host clock with
  // fallback = device_open_failed: a clip must play even with no working audio
  // device. Only a programming error returns an error here.
  [[nodiscard]] expected start(std::uint32_t sample_rate, std::uint32_t channels) noexcept;

  // No audio track at all -> host clock is master from the start.
  void start_host_only() noexcept;
  void stop() noexcept;

  // [decode-thread][no-block] False when the ring is full; the caller backs off
  // rather than blocking.
  [[nodiscard]] bool submit(const audio_block& block) noexcept;

  // [any-thread][no-block] The master clock: stream-relative ns, rate-scaled.
  [[nodiscard]] time_ns now_ns() const noexcept;

  void set_rate(double rate) noexcept;
  void set_paused(bool paused) noexcept;
  void set_volume(float volume) noexcept;
  void set_muted(bool muted) noexcept;

  // Re-seed after a seek. Blocks at a stale generation are discarded.
  void seeked(time_ns to_ns, std::uint32_t generation) noexcept;

  // [render-thread][no-block] What the presenter decided, fed back so the
  // series and the counters see it. Republishes the stats snapshot.
  void record_present(const present_decision& decision, bool showed) noexcept;

  [[nodiscard]] clock_stats stats() const noexcept;

  // The overlay and the soak acquire here. shell -> player is legal.
  [[nodiscard]] publish_slot<clock_stats>& published() noexcept;

  // Test seam: inject a fake endpoint. Takes ownership.
  //
  // This is not a convenience. Two of the three clauses in PR 5b's verify line
  // — device loss recovery and the silent-clip fallback — cannot be tested
  // deterministically without it, and on a machine with no audio device they
  // cannot be tested at all.
  void set_sink_for_test(audio_sink* sink) noexcept;

 private:
  struct impl;
  impl* impl_ = nullptr;
};

}  // namespace mv::player
