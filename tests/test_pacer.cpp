// SPDX-License-Identifier: GPL-2.0-or-later
//
// The pacer is the thing that decides whether PR 1's verify line holds, so its
// arithmetic is tested rather than trusted. The GPU-facing half (DXGI frame
// statistics) is exercised by the lab's own soak run, not here — a unit test
// cannot manufacture a vblank.

#include <catch2/catch_test_macros.hpp>

#include <thread>

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
