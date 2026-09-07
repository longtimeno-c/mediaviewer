// SPDX-License-Identifier: GPL-2.0-or-later
//
// The pacer is the thing that decides whether PR 1's verify line holds, so its
// arithmetic is tested rather than trusted. The GPU-facing half (DXGI frame
// statistics) is exercised by the lab's own soak run, not here — a unit test
// cannot manufacture a vblank.

#include <catch2/catch_test_macros.hpp>

#include <thread>
#include <limits>

#include "gfx/pacer.h"

using namespace std::chrono_literals;

TEST_CASE("a fresh pacer reports nothing rather than zero", "[gfx][pacer]") {
  mv::gfx::pacer pacer;
  pacer.begin_session(1.0 / 60.0);

  const auto stats = pacer.stats();
  REQUIRE(stats.frames == 0);
  REQUIRE(stats.dropped_frames == 0);
  REQUIRE(stats.source == mv::gfx::drop_source::none);
  // No frames means the gate is not met. "Zero dropped frames" out of zero
  // frames is the kind of green number that certifies nothing.
  REQUIRE_FALSE(stats.meets_pr1_gate());
}

TEST_CASE("the first present establishes a baseline but is not itself an interval",
          "[gfx][pacer]") {
  mv::gfx::pacer pacer;
  pacer.begin_session(1.0 / 60.0);

  pacer.frame_begin();
  pacer.frame_end(nullptr);
  REQUIRE(pacer.stats().frames == 0);

  std::this_thread::sleep_for(2ms);
  pacer.frame_begin();
  pacer.frame_end(nullptr);
  REQUIRE(pacer.stats().frames == 1);
}

TEST_CASE("without DXGI statistics the source is labelled, not laundered",
          "[gfx][pacer]") {
  // Passing a null swapchain is the "frame statistics unavailable" case. The
  // pacer must fall back AND say so: meets_pr1_gate() requires the
  // authoritative source, because inferring zero drops from QPC alone is not
  // the D6 gate.
  mv::gfx::pacer pacer;
  pacer.begin_session(1.0 / 60.0);

  for (int i = 0; i < 4; ++i) {
    pacer.frame_begin();
    pacer.frame_end(nullptr);
    std::this_thread::sleep_for(1ms);
  }

  const auto stats = pacer.stats();
  REQUIRE(stats.frames >= 3);
  REQUIRE(stats.source == mv::gfx::drop_source::interval_heuristic);
  REQUIRE_FALSE(stats.meets_pr1_gate());
}

TEST_CASE("a long interval is counted as a dropped frame by the fallback",
          "[gfx][pacer]") {
  // 240 Hz claimed refresh against ~10 ms sleeps: every interval is a couple of
  // dozen missed vblanks, which the heuristic must notice.
  mv::gfx::pacer pacer;
  pacer.begin_session(1.0 / 240.0);

  pacer.frame_begin();
  pacer.frame_end(nullptr);
  for (int i = 0; i < 3; ++i) {
    std::this_thread::sleep_for(10ms);
    pacer.frame_begin();
    pacer.frame_end(nullptr);
  }

  const auto stats = pacer.stats();
  REQUIRE(stats.frames == 3);
  REQUIRE(stats.dropped_frames == 3);
  REQUIRE(stats.missed_refreshes > stats.dropped_frames);
}

TEST_CASE("percentiles do not flatter", "[gfx][pacer]") {
  mv::gfx::pacer pacer;
  pacer.begin_session(1.0 / 60.0);

  pacer.frame_begin();
  pacer.frame_end(nullptr);
  for (int i = 0; i < 20; ++i) {
    std::this_thread::sleep_for(2ms);
    pacer.frame_begin();
    pacer.frame_end(nullptr);
  }

  const auto stats = pacer.stats();
  REQUIRE(stats.frames == 20);
  REQUIRE(stats.p50_ms > 0.0);
  REQUIRE(stats.p99_ms >= stats.p50_ms);
  REQUIRE(stats.mean_ms > 0.0);

  // p99 is the bucket's UPPER edge, so it may exceed the true maximum by up to
  // one bucket width. That is the direction the rounding is meant to go — a
  // percentile that rounds down flatters the result — so the assertion allows
  // for it rather than demanding max >= p99, which is only true by luck.
  constexpr double bucket_width_ms = 0.05;
  REQUIRE(stats.max_ms >= stats.p99_ms - bucket_width_ms);
}

TEST_CASE("reset_window clears the histogram but keeps the refresh interval",
          "[gfx][pacer]") {
  // Going idle and coming back must not score the idle gap as a stall.
  mv::gfx::pacer pacer;
  pacer.begin_session(1.0 / 120.0);

  pacer.frame_begin();
  pacer.frame_end(nullptr);
  std::this_thread::sleep_for(20ms);
  pacer.frame_begin();
  pacer.frame_end(nullptr);
  REQUIRE(pacer.stats().dropped_frames > 0);

  pacer.reset_window();
  const auto stats = pacer.stats();
  REQUIRE(stats.frames == 0);
  REQUIRE(stats.dropped_frames == 0);
  REQUIRE(stats.max_ms == 0.0);
  REQUIRE(stats.refresh_interval_ms > 8.0);
  REQUIRE(stats.refresh_interval_ms < 8.5);
}

TEST_CASE("display statistics use presentation vblanks and retain duplicate baselines", "[gfx][pacer]") {
  mv::gfx::display_statistics tracker;
  DXGI_FRAME_STATISTICS fs{};
  fs.PresentCount = 10;
  fs.PresentRefreshCount = 100;
  fs.SyncRefreshCount = 500;
  REQUIRE_FALSE(tracker.observe(S_OK, fs).valid); // establishes baseline
  fs.SyncRefreshCount += 20; // scheduler clock changes without a displayed frame
  REQUIRE(tracker.observe(S_OK, fs).valid);
  ++fs.PresentCount;
  fs.PresentRefreshCount += 3;
  const auto missed = tracker.observe(S_OK, fs);
  REQUIRE(missed.valid);
  REQUIRE(missed.presents == 1);
  REQUIRE(missed.missed_refreshes == 2);
  fs.PresentCount += 3;
  fs.PresentRefreshCount += 3;
  REQUIRE(tracker.observe(S_OK, fs).missed_refreshes == 0);
}

TEST_CASE("display statistics handle wrap, resets and missing coverage", "[gfx][pacer]") {
  mv::gfx::display_statistics tracker;
  DXGI_FRAME_STATISTICS fs{};
  fs.PresentCount = 0xffffffffu;
  fs.PresentRefreshCount = 0xfffffffeu;
  (void)tracker.observe(S_OK, fs);
  fs.PresentCount = 0;
  fs.PresentRefreshCount = 1;
  REQUIRE(tracker.observe(S_OK, fs).missed_refreshes == 2);
  REQUIRE_FALSE(tracker.observe(E_FAIL, fs).valid);
  REQUIRE_FALSE(tracker.observe(S_OK, fs).valid); // re-establish, no bridging a gap
  REQUIRE(tracker.observe(DXGI_ERROR_FRAME_STATISTICS_DISJOINT, fs).discontinuity);
  fs.PresentCount = 100;
  fs.PresentRefreshCount = 200;
  (void)tracker.observe(S_OK, fs);
  fs.PresentCount = 1;
  fs.PresentRefreshCount = 1;
  REQUIRE(tracker.observe(S_OK, fs).discontinuity);
}

TEST_CASE("PR1 gate requires a full refresh-matched trusted window", "[gfx][gate]") {
  mv::gfx::pace_stats s;
  s.frames = s.displayed_presents = 3600;
  s.elapsed_seconds = 60.0;
  s.refresh_interval_ms = s.mean_ms = 1000.0 / 60.0;
  s.p50_ms = 16.7;
  s.max_ms = 17.0;
  s.source = mv::gfx::drop_source::frame_statistics;
  REQUIRE(s.meets_pr1_gate());
  SECTION("too short") { s.elapsed_seconds = 59.0; }
  SECTION("unknown refresh") { s.refresh_interval_ms = 0.0; }
  SECTION("incorrect cadence") { s.p50_ms = 8.35; }
  SECTION("too few application frames") { s.frames = 100; }
  SECTION("stale display statistics") { s.displayed_presents = 0; }
  SECTION("statistics gap followed by valid samples") { s.statistics_unavailable_frames = 1; }
  SECTION("discontinuity") { s.statistics_discontinuities = 1; }
  SECTION("drop") { s.dropped_frames = 1; }
  SECTION("bad numeric value") { s.mean_ms = std::numeric_limits<double>::quiet_NaN(); }
  SECTION("stall") { s.max_ms = 40; }
  REQUIRE_FALSE(s.meets_pr1_gate());
}

TEST_CASE("idle gate requires actual CPU measurement and zero presentations", "[gfx][gate]") {
  mv::gfx::idle_stats s;
  s.elapsed_seconds = 60;
  s.cpu_percent = 0.05;
  REQUIRE(s.meets_pr1_gate());
  SECTION("short") { s.elapsed_seconds = 59; }
  SECTION("CPU busy") { s.cpu_percent = 2; }
  SECTION("CPU unavailable") { s.cpu_percent = -1; }
  SECTION("presented") { s.presents = 1; }
  SECTION("input interfered") { s.input_events = 1; }
  REQUIRE_FALSE(s.meets_pr1_gate());
}
