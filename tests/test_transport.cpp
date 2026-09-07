// SPDX-License-Identifier: GPL-2.0-or-later
// PR 5c transport policy, headless. No clip, no device, no clock.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

#include "player/transport.h"

using namespace mv::player;

namespace {
constexpr time_ns sec(double v) { return static_cast<time_ns>(v * 1'000'000'000.0); }
}  // namespace

// --- seek mode --------------------------------------------------------------

TEST_CASE("dragging the scrubber seeks to a keyframe, releasing is exact", "[transport]") {
  // plan/05's fast-then-accurate rule. Getting this backwards makes scrubbing
  // feel like treacle, which is the whole thing 5c's verify line asks for.
  REQUIRE(make_seek(sec(30), /*dragging=*/true, 1).mode == seek_mode::keyframe);
  REQUIRE(make_seek(sec(30), /*dragging=*/false, 1).mode == seek_mode::exact);
}

TEST_CASE("a negative seek target is clamped to the start", "[transport]") {
  REQUIRE(make_seek(sec(-5), false, 1).target_ns == 0);
}

TEST_CASE("the seek carries the generation that discards in-flight frames", "[transport]") {
  REQUIRE(make_seek(sec(10), false, 42).generation == 42);
}

// --- speed ------------------------------------------------------------------

TEST_CASE("rate 1.0 produces no filter at all", "[transport]") {
  // Not "atempo=1.000": a no-op filter still costs a resample pass per block.
  REQUIRE(build_atempo_chain(1.0).empty());
}

TEST_CASE("rates inside atempo's range use a single instance", "[transport]") {
  REQUIRE(build_atempo_chain(1.5) == "atempo=1.500");
  REQUIRE(build_atempo_chain(0.5) == "atempo=0.500");
  REQUIRE(build_atempo_chain(2.0) == "atempo=2.000");
}

TEST_CASE("the extremes are chained, because atempo caps at 0.5-2.0", "[transport]") {
  // plan/05 names this case explicitly. One instance cannot do 0.25x or 4x.
  REQUIRE(build_atempo_chain(0.25) == "atempo=0.500,atempo=0.500");
  REQUIRE(build_atempo_chain(4.0) == "atempo=2.000,atempo=2.000");
}

TEST_CASE("an uneven ratio splits rather than assuming a fixed chain", "[transport]") {
  REQUIRE(build_atempo_chain(3.0) == "atempo=2.000,atempo=1.500");
}

TEST_CASE("every chain multiplies back to the requested rate", "[transport]") {
  // The property that actually matters: whatever the split, the product is the
  // speed the user asked for.
  for (double rate : {0.25, 0.3, 0.5, 0.75, 1.25, 1.5, 2.0, 2.5, 3.0, 3.7, 4.0}) {
    const std::string chain = build_atempo_chain(rate);
    REQUIRE_FALSE(chain.empty());

    double product = 1.0;
    std::size_t pos = 0;
    while ((pos = chain.find("atempo=", pos)) != std::string::npos) {
      pos += 7;
      product *= std::stod(chain.substr(pos));
    }
    REQUIRE(product > rate - 0.005);
    REQUIRE(product < rate + 0.005);
  }
}

TEST_CASE("every atempo instance in a chain is within FFmpeg's legal range", "[transport]") {
  // A chain that multiplies correctly but contains an out-of-range instance
  // fails at filter-graph construction, at runtime, on the user's machine.
  for (double rate : {0.25, 0.26, 0.49, 0.51, 1.99, 2.01, 3.99, 4.0}) {
    const std::string chain = build_atempo_chain(rate);
    std::size_t pos = 0;
    while ((pos = chain.find("atempo=", pos)) != std::string::npos) {
      pos += 7;
      const double factor = std::stod(chain.substr(pos));
      REQUIRE(factor >= 0.5);
      REQUIRE(factor <= 2.0);
    }
  }
}

TEST_CASE("rates outside 0.25-4.0 are clamped, and NaN falls back to 1.0", "[transport]") {
  REQUIRE(clamp_rate(0.01) == min_rate);
  REQUIRE(clamp_rate(99.0) == max_rate);
  REQUIRE(clamp_rate(0.0) == 1.0);
  REQUIRE(clamp_rate(-2.0) == 1.0);
  REQUIRE(clamp_rate(std::nan("")) == 1.0);
}

// --- A-B loop ---------------------------------------------------------------

TEST_CASE("no loop is set means no wrap", "[transport]") {
  time_ns to = -1;
  REQUIRE_FALSE(loop_wrap(ab_loop{}, sec(100), &to));
}

TEST_CASE("playback wraps to A once it passes B", "[transport]") {
  const ab_loop loop{sec(10), sec(20)};
  time_ns to = -1;
  REQUIRE_FALSE(loop_wrap(loop, sec(15), &to));
  REQUIRE(loop_wrap(loop, sec(20), &to));
  REQUIRE(to == sec(10));
}

TEST_CASE("marking B before A still loops", "[transport]") {
  // The user's intent is unambiguous; silently doing nothing is the wrong
  // answer to a reversed pair.
  const ab_loop reversed{sec(20), sec(10)};
  time_ns to = -1;
  REQUIRE(loop_wrap(reversed, sec(25), &to));
  REQUIRE(to == sec(10));
}

TEST_CASE("a zero-length loop does not wrap", "[transport]") {
  time_ns to = -1;
  REQUIRE_FALSE(loop_wrap(ab_loop{sec(10), sec(10)}, sec(30), &to));
}

// --- frame step -------------------------------------------------------------

TEST_CASE("stepping forward lands inside the next frame", "[transport]") {
  const double fps = 25.0;
  const time_ns frame = sec(1) / 25;
  const time_ns target = step_target(sec(10), +1, fps);
  REQUIRE(target > sec(10) + frame / 2);
  REQUIRE(target < sec(10) + frame + frame);
}

TEST_CASE("stepping backward lands inside the previous frame", "[transport]") {
  const double fps = 25.0;
  const time_ns frame = sec(1) / 25;
  const time_ns target = step_target(sec(10), -1, fps);
  REQUIRE(target < sec(10) - frame / 2);
  REQUIRE(target > sec(10) - frame - frame);
}

TEST_CASE("repeated steps advance rather than sticking on one frame", "[transport]") {
  // The half-frame bias exists for this: without it a position sitting exactly
  // on a boundary steps onto the same frame twice.
  const double fps = 30.0;
  time_ns pos = 0;
  time_ns previous = -1;
  for (int i = 0; i < 20; ++i) {
    pos = step_target(pos, +1, fps);
    REQUIRE(pos > previous);
    previous = pos;
  }
}

TEST_CASE("stepping back at the start clamps to zero rather than going negative",
          "[transport]") {
  REQUIRE(step_target(0, -1, 25.0) == 0);
}

TEST_CASE("a clip with no usable frame rate can still be stepped", "[transport]") {
  REQUIRE(step_target(sec(10), +1, 0.0) > sec(10));
  REQUIRE(step_target(sec(10), +1, -5.0) > sec(10));
}

TEST_CASE("a step of zero frames is a no-op", "[transport]") {
  REQUIRE(step_target(sec(10), 0, 25.0) == sec(10));
}

// --- resume -----------------------------------------------------------------

TEST_CASE("the first few seconds are not worth resuming", "[transport]") {
  REQUIRE_FALSE(should_store_resume(sec(5), sec(600)));
}

TEST_CASE("a position mid-clip is stored", "[transport]") {
  REQUIRE(should_store_resume(sec(300), sec(600)));
}

TEST_CASE("watching to the end does not store a resume on the credits", "[transport]") {
  // Reopening two seconds before the end is worse than starting over.
  REQUIRE_FALSE(should_store_resume(sec(598), sec(600)));
}

TEST_CASE("a stored position near the end restarts from the beginning", "[transport]") {
  REQUIRE(resume_start_position(sec(595), sec(600)) == 0);
  REQUIRE(resume_start_position(sec(300), sec(600)) == sec(300));
}

TEST_CASE("an unknown duration never stores or restores a resume", "[transport]") {
  REQUIRE_FALSE(should_store_resume(sec(300), 0));
  REQUIRE(resume_start_position(sec(300), 0) == 0);
}
