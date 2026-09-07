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

#include <cstdint>

#include "player/audio_sink.h"
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

}  // namespace mv::player
