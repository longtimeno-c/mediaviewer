// SPDX-License-Identifier: GPL-2.0-or-later
// Metal pacer arithmetic. No GPU: the Darwin lab feeds it display-link
// timestamps; this file checks the same counters a 60 s soak will be judged by.

#include <catch2/catch_test_macros.hpp>

#include <thread>

#include "gfx/metal_pacer.h"

using namespace std::chrono_literals;

TEST_CASE("a fresh metal pacer reports nothing rather than zero", "[gfx][metal_pacer]") {
  mv::gfx::metal_pacer pacer;
  pacer.begin_session(1.0 / 60.0);
  const auto stats = pacer.stats();
  REQUIRE(stats.frames == 0);
  REQUIRE(stats.source == mv::gfx::metal_drop_source::none);
  REQUIRE_FALSE(stats.meets_pr16_gate());
}

TEST_CASE("the first present is a baseline, not an interval", "[gfx][metal_pacer]") {
  mv::gfx::metal_pacer pacer;
  pacer.begin_session(1.0 / 60.0);
  mv::gfx::display_link_tick tick;
  tick.valid = true;
  tick.target_seconds = 1.0;

  pacer.frame_begin();
  pacer.frame_end(tick);
  REQUIRE(pacer.stats().frames == 0);

  tick.target_seconds = 1.0 + 1.0 / 60.0;
  std::this_thread::sleep_for(2ms);
  pacer.frame_begin();
  pacer.frame_end(tick);
  REQUIRE(pacer.stats().frames == 1);
  REQUIRE(pacer.stats().displayed_presents == 1);
  REQUIRE(pacer.stats().source == mv::gfx::metal_drop_source::display_link);
}

TEST_CASE("without a display-link tick the source is labelled, not laundered",
          "[gfx][metal_pacer]") {
  mv::gfx::metal_pacer pacer;
  pacer.begin_session(1.0 / 60.0);
  mv::gfx::display_link_tick missing;

  for (int i = 0; i < 4; ++i) {
    pacer.frame_begin();
    pacer.frame_end(missing);
    std::this_thread::sleep_for(1ms);
  }
  const auto stats = pacer.stats();
  REQUIRE(stats.frames >= 3);
  REQUIRE(stats.source == mv::gfx::metal_drop_source::interval_heuristic);
  REQUIRE_FALSE(stats.meets_pr16_gate());
}

TEST_CASE("a 2x target step is one missed refresh", "[gfx][metal_pacer]") {
  mv::gfx::display_link_statistics tracker;
  mv::gfx::display_link_tick tick;
  tick.valid = true;
  tick.target_seconds = 10.0;
  REQUIRE_FALSE(tracker.observe(tick, 1.0 / 60.0).valid);

  tick.target_seconds = 10.0 + 2.0 / 60.0;
  const auto sample = tracker.observe(tick, 1.0 / 60.0);
  REQUIRE(sample.valid);
  REQUIRE(sample.presents == 1);
  REQUIRE(sample.missed_refreshes == 1);
}

TEST_CASE("a 1x target step is a clean present", "[gfx][metal_pacer]") {
  mv::gfx::display_link_statistics tracker;
  mv::gfx::display_link_tick tick;
  tick.valid = true;
  tick.target_seconds = 0.0;
  (void)tracker.observe(tick, 1.0 / 120.0);
  tick.target_seconds = 1.0 / 120.0;
  const auto sample = tracker.observe(tick, 1.0 / 120.0);
  REQUIRE(sample.valid);
  REQUIRE(sample.missed_refreshes == 0);
}

TEST_CASE("a rewind is a discontinuity, not a drop", "[gfx][metal_pacer]") {
  mv::gfx::display_link_statistics tracker;
  mv::gfx::display_link_tick tick;
  tick.valid = true;
  tick.target_seconds = 5.0;
  (void)tracker.observe(tick, 1.0 / 60.0);
  tick.target_seconds = 4.0;
  const auto sample = tracker.observe(tick, 1.0 / 60.0);
  REQUIRE(sample.discontinuity);
  REQUIRE_FALSE(sample.valid);
}

TEST_CASE("an implausible jump is a discontinuity", "[gfx][metal_pacer]") {
  mv::gfx::display_link_statistics tracker;
  mv::gfx::display_link_tick tick;
  tick.valid = true;
  tick.target_seconds = 0.0;
  (void)tracker.observe(tick, 1.0 / 60.0);
  tick.target_seconds = 100.0;
  const auto sample = tracker.observe(tick, 1.0 / 60.0);
  REQUIRE(sample.discontinuity);
}

TEST_CASE("a late presentedTime counts as a miss when targets were clean",
          "[gfx][metal_pacer]") {
  mv::gfx::display_link_statistics tracker;
  mv::gfx::display_link_tick tick;
  tick.valid = true;
  tick.target_seconds = 0.0;
  (void)tracker.observe(tick, 1.0 / 60.0);
  tick.target_seconds = 1.0 / 60.0;
  tick.presented_seconds = tick.target_seconds + 1.0 / 60.0;
  const auto sample = tracker.observe(tick, 1.0 / 60.0);
  REQUIRE(sample.valid);
  REQUIRE(sample.missed_refreshes == 1);
}

TEST_CASE("idle-gap link misses are not drops when the clock is one refresh",
          "[gfx][metal_pacer]") {
  REQUIRE(mv::gfx::reconcile_link_misses(57, 16.7, 16.68) == 0);
  REQUIRE(mv::gfx::reconcile_link_misses(1, 33.4, 16.68) == 1);
  REQUIRE(mv::gfx::reconcile_link_misses(0, 16.7, 16.68) == 0);
  REQUIRE(mv::gfx::reconcile_link_misses(114, 16.7, 0.0) == 114);
}

TEST_CASE("reset_window clears drops but keeps the refresh interval", "[gfx][metal_pacer]") {
  mv::gfx::metal_pacer pacer;
  pacer.begin_session(1.0 / 120.0);
  mv::gfx::display_link_tick tick;
  tick.valid = true;
  tick.target_seconds = 0.0;
  pacer.frame_begin();
  pacer.frame_end(tick);
  std::this_thread::sleep_for(20ms);
  tick.target_seconds = 0.2;
  pacer.frame_begin();
  pacer.frame_end(tick);
  REQUIRE(pacer.stats().dropped_frames > 0);

  pacer.reset_window();
  const auto stats = pacer.stats();
  REQUIRE(stats.frames == 0);
  REQUIRE(stats.dropped_frames == 0);
  REQUIRE(stats.refresh_interval_ms > 8.0);
  REQUIRE(stats.refresh_interval_ms < 8.5);
}

TEST_CASE("zero frames cannot pass the PR 16 gate", "[gfx][metal_pacer]") {
  mv::gfx::metal_pace_stats s;
  s.elapsed_seconds = 60.0;
  s.refresh_interval_ms = 16.666;
  s.source = mv::gfx::metal_drop_source::display_link;
  REQUIRE_FALSE(s.meets_pr16_gate());
}

TEST_CASE("a clock-only perfect cadence still fails the PR 16 gate", "[gfx][metal_pacer]") {
  mv::gfx::metal_pace_stats s;
  s.elapsed_seconds = 60.0;
  s.refresh_interval_ms = 16.666667;
  s.frames = 3600;
  s.displayed_presents = 3600;
  s.mean_ms = 16.666667;
  s.p50_ms = 16.666667;
  s.max_ms = 16.7;
  s.source = mv::gfx::metal_drop_source::interval_heuristic;
  REQUIRE_FALSE(s.meets_pr16_gate());
  s.source = mv::gfx::metal_drop_source::display_link;
  REQUIRE(s.meets_pr16_gate());
}

TEST_CASE("idle gate matches PR 1: 60 s, ~0 % CPU, zero presents, zero input",
          "[gfx][metal_pacer]") {
  mv::gfx::metal_idle_stats idle;
  REQUIRE_FALSE(idle.meets_pr16_gate());
  idle.elapsed_seconds = 60.0;
  idle.cpu_percent = 0.4;
  REQUIRE(idle.meets_pr16_gate());
  idle.presents = 1;
  REQUIRE_FALSE(idle.meets_pr16_gate());
  idle.presents = 0;
  idle.input_events = 1;
  REQUIRE_FALSE(idle.meets_pr16_gate());
  idle.input_events = 0;
  idle.cpu_percent = 1.1;
  REQUIRE_FALSE(idle.meets_pr16_gate());
}

TEST_CASE("percentiles do not flatter", "[gfx][metal_pacer]") {
  mv::gfx::metal_pacer pacer;
  pacer.begin_session(1.0 / 60.0);
  mv::gfx::display_link_tick tick;
  tick.valid = true;
  tick.target_seconds = 0.0;
  pacer.frame_begin();
  pacer.frame_end(tick);
  for (int i = 1; i <= 20; ++i) {
    std::this_thread::sleep_for(2ms);
    tick.target_seconds = static_cast<double>(i) / 60.0;
    pacer.frame_begin();
    pacer.frame_end(tick);
  }
  const auto stats = pacer.stats();
  REQUIRE(stats.frames == 20);
  REQUIRE(stats.p99_ms >= stats.p50_ms);
  constexpr double bucket_width_ms = 0.05;
  REQUIRE(stats.max_ms >= stats.p99_ms - bucket_width_ms);
}
