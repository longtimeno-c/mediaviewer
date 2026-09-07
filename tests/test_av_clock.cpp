// SPDX-License-Identifier: GPL-2.0-or-later
// The drift measurement, tested headlessly — no device, no endpoint, no clip.
//
// These tests exist because of one specific failure mode. plan/05: "Steady-state
// drift must be flat — a slow ramp means your audio position query is wrong."
// The instantaneous A/V error is flat BY CONSTRUCTION, because the presenter
// drops and holds precisely to force it flat, so a flat err_ms graph passes even
// when the clock is completely broken. The slope over the long horizon is the
// only thing that tells the two apart, and it is what the 30-minute verify
// actually rests on. So it gets tested against a known ramp, not eyeballed.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "player/av_clock.h"

using Catch::Approx;
using namespace mv::player;

namespace {

constexpr time_ns ns_per_second = 1'000'000'000;
constexpr time_ns vblank_60hz = 16'666'667;

// Feeds `seconds` of presents at 60 Hz, with err_ms produced by `shape`.
template <typename Shape>
void run_presents(drift_tracker& tracker, int seconds, Shape shape) {
  const int frames = seconds * 60;
  for (int i = 0; i < frames; ++i) {
    const time_ns host_ns = static_cast<time_ns>(i) * vblank_60hz;
    tracker.observe(shape(i, host_ns), host_ns);
  }
}

clock_stats filled(const drift_tracker& tracker) {
  clock_stats out;
  tracker.fill(out);
  return out;
}

}  // namespace

TEST_CASE("a fresh tracker reports nothing rather than zero-as-a-value", "[drift]") {
  drift_tracker tracker;
  tracker.reset();
  const clock_stats stats = filled(tracker);

  CHECK(stats.err_ms_mean == 0.0);
  CHECK(stats.drift_slope_ms_per_min == 0.0);
  CHECK(stats.position_discontinuities == 0);
  CHECK(stats.host_clock_gaps == 0);
  CHECK(tracker.horizon().empty());
}

TEST_CASE("a flat error series has zero slope", "[drift]") {
  // The healthy case: bounded jitter around a fixed offset, no ramp. This is
  // what a correct clock looks like and it must not trip the gate.
  drift_tracker tracker;
  tracker.reset();

  run_presents(tracker, 600, [](int i, time_ns) {
    // Deterministic sawtooth of +-4 ms — far more jitter than a real endpoint,
    // to prove the slope estimate is not fooled by noise.
    return ((i % 7) - 3) * 1.3;
  });

  const clock_stats stats = filled(tracker);
  INFO("slope was " << stats.drift_slope_ms_per_min << " ms/min");
  // The gate is 1 ms/min. Ten minutes of heavy jitter must land far inside it.
  CHECK(std::abs(stats.drift_slope_ms_per_min) < 0.1);
}

TEST_CASE("a known ramp is recovered at the right rate", "[drift]") {
  // THE test. A wrong audio position query does not look like noise, it looks
  // like a slow linear ramp. Inject exactly 2 ms per minute and require the
  // tracker to report 2 ms per minute — if this regresses, the 30-minute soak
  // silently starts passing broken clocks.
  drift_tracker tracker;
  tracker.reset();

  constexpr double ramp_ms_per_min = 2.0;
  run_presents(tracker, 900, [](int, time_ns host_ns) {
    const double minutes = static_cast<double>(host_ns) / (60.0 * ns_per_second);
    return ramp_ms_per_min * minutes;
  });

  const clock_stats stats = filled(tracker);
  INFO("slope was " << stats.drift_slope_ms_per_min << " ms/min");
  CHECK(stats.drift_slope_ms_per_min == Approx(ramp_ms_per_min).margin(0.05));
}

TEST_CASE("a ramp buried in jitter is still recovered", "[drift]") {
  // The realistic version of the failure: a small ramp under noise several times
  // its size. Least squares over ~1800 one-hertz samples is what makes this
  // recoverable at all, and it is why the horizon series exists separately from
  // the 240-frame live ring.
  drift_tracker tracker;
  tracker.reset();

  constexpr double ramp_ms_per_min = 1.5;
  run_presents(tracker, 1800, [](int i, time_ns host_ns) {
    const double minutes = static_cast<double>(host_ns) / (60.0 * ns_per_second);
    const double jitter = ((i % 11) - 5) * 1.1;  // +-5.5 ms, deterministic
    return ramp_ms_per_min * minutes + jitter;
  });

  const clock_stats stats = filled(tracker);
  INFO("slope was " << stats.drift_slope_ms_per_min << " ms/min");
  CHECK(stats.drift_slope_ms_per_min == Approx(ramp_ms_per_min).margin(0.2));
  // And the thing that proves the point. Within the rolling window the error
  // spread is just the jitter — about 11 ms peak to peak — with no hint that the
  // clock has walked 45 ms over the run. A reviewer watching only the live graph
  // sees a stable band and calls it healthy. Only the slope shows the drift.
  const double window_spread = stats.err_ms_max - stats.err_ms_min;
  INFO("rolling-window spread was " << window_spread << " ms");
  CHECK(window_spread < 15.0);
}

TEST_CASE("the live window is a rolling window, not the whole run", "[drift]") {
  // plan/05: "Track drift over a rolling window." The percentiles must follow
  // recent behaviour, so a bad first minute does not haunt the overlay forever.
  drift_tracker tracker;
  tracker.reset();

  run_presents(tracker, 60, [](int, time_ns) { return 50.0; });
  CHECK(filled(tracker).err_ms_mean == Approx(50.0));

  // 240 presents is exactly the live window; five seconds at 60 Hz overwrites it.
  const time_ns base = 60 * ns_per_second;
  for (int i = 0; i < 300; ++i) {
    tracker.observe(1.0, base + static_cast<time_ns>(i) * vblank_60hz);
  }
  CHECK(filled(tracker).err_ms_mean == Approx(1.0));
}

TEST_CASE("both error tails are reported signed", "[drift]") {
  // A distribution skewed one way is a bias; skewed both ways is jitter, and
  // they have different causes. Collapsing to an absolute value hides which.
  drift_tracker tracker;
  tracker.reset();

  tracker.observe(-8.0, 0);
  tracker.observe(3.0, vblank_60hz);
  tracker.observe(-1.0, 2 * vblank_60hz);

  const clock_stats stats = filled(tracker);
  CHECK(stats.err_ms_min == Approx(-8.0));
  CHECK(stats.err_ms_max == Approx(3.0));
  CHECK(stats.err_ms_last == Approx(-1.0));
  // p99 is of the magnitude: a large negative error is just as late as a large
  // positive one.
  CHECK(stats.err_ms_p99 == Approx(8.0));
}

TEST_CASE("a host clock gap is counted, not smoothed over", "[drift]") {
  // The machine slept or power-throttled mid-run. plan/12's honesty rule and
  // gfx::pacer's precedent: a run that trips this is re-run, not averaged.
  drift_tracker tracker;
  tracker.reset();

  tracker.observe(0.0, 0);
  tracker.observe(0.0, vblank_60hz);
  CHECK(filled(tracker).host_clock_gaps == 0);

  // Five seconds between two consecutive presents is not scheduling jitter.
  tracker.observe(0.0, vblank_60hz + 5 * ns_per_second);
  CHECK(filled(tracker).host_clock_gaps == 1);
}

TEST_CASE("a position discontinuity is surfaced rather than averaged", "[drift]") {
  // IAudioClock::GetPosition can jump on a device change or a stream reset. One
  // jump poisons a regression slope, so it is reported — same contract as
  // gfx::pacer refusing its gate on statistics_discontinuities.
  drift_tracker tracker;
  tracker.reset();

  run_presents(tracker, 5, [](int, time_ns) { return 0.0; });
  CHECK(filled(tracker).position_discontinuities == 0);

  tracker.note_position_discontinuity();
  tracker.note_position_discontinuity();
  CHECK(filled(tracker).position_discontinuities == 2);
}

TEST_CASE("the horizon series is one sample per second", "[drift]") {
  // 240 presents is about four seconds and physically cannot show a 30-minute
  // ramp. The horizon is what the slope is fitted over, and the CSV the verify
  // line calls "the overlay to prove it" is written from it.
  drift_tracker tracker;
  tracker.reset();

  run_presents(tracker, 120, [](int, time_ns) { return 2.0; });

  const auto horizon = tracker.horizon();
  CHECK(horizon.size() == 120);
  for (const float value : horizon) CHECK(value == Approx(2.0f));
}

TEST_CASE("reset clears the series but not the shape of it", "[drift]") {
  // A seek is a deliberate discontinuity, not evidence of one: av_clock resets
  // the tracker so a scrub does not read as a drift ramp.
  drift_tracker tracker;
  tracker.reset();

  run_presents(tracker, 30, [](int, time_ns) { return 5.0; });
  tracker.note_position_discontinuity();
  REQUIRE(filled(tracker).position_discontinuities == 1);

  tracker.reset();
  const clock_stats stats = filled(tracker);
  CHECK(stats.position_discontinuities == 0);
  CHECK(stats.err_ms_mean == 0.0);
  CHECK(tracker.horizon().empty());
}
