// SPDX-License-Identifier: GPL-2.0-or-later
// The drop/hold/show rules from plan/05, tested headlessly — no device, no
// audio endpoint, no clip. That is the whole point of keeping choose() pure.
#include <catch2/catch_test_macros.hpp>

#include "player/presenter.h"

using namespace mv::player;

namespace {

constexpr time_ns ms(double v) { return static_cast<time_ns>(v * 1'000'000.0); }
constexpr time_ns vblank_60hz = 16'666'667;

presenter_input at(time_ns clock, time_ns next, bool has_next = true, double rate = 1.0) {
  presenter_input in;
  in.master_clock_ns = clock;
  in.vblank_ns = vblank_60hz;
  in.next_pts_ns = next;
  in.has_next = has_next;
  in.playback_rate = rate;
  return in;
}

}  // namespace

TEST_CASE("starved queue holds rather than stalling", "[presenter]") {
  // plan/05: "if the queue is starved, hold the current frame rather than
  // stalling." Never show, never drop.
  const auto d = choose(at(ms(1000), 0, /*has_next=*/false));
  REQUIRE(d.action == present_action::hold_starved);
}

TEST_CASE("a frame due within this vblank is shown", "[presenter]") {
  const time_ns clock = ms(1000);
  const auto d = choose(at(clock, clock + vblank_60hz));
  REQUIRE(d.action == present_action::show);
}

TEST_CASE("a frame not yet due is a cadence hold, not a drop", "[presenter]") {
  // 24p on 60 Hz: the next frame is ~41.7 ms out while a vblank is 16.7 ms, so
  // most vblanks hold. This is correct 3:2 cadence. If this ever returns drop,
  // every 24p clip reads as permanently broken.
  const time_ns clock = ms(1000);
  const auto d = choose(at(clock, clock + ms(41.7)));
  REQUIRE(d.action == present_action::hold_cadence);
}

TEST_CASE("cadence holds are never counted as late", "[presenter]") {
  // Walk a 24p stream across 60 Hz vblanks and assert we only ever show or
  // hold_cadence — no drops on perfectly healthy content.
  constexpr time_ns frame_24p = 41'666'667;
  time_ns clock = 0;
  time_ns next_pts = 0;
  int shown = 0, held = 0;

  for (int i = 0; i < 600; ++i) {
    const auto d = choose(at(clock, next_pts));
    REQUIRE(d.action != present_action::drop);
    REQUIRE(d.action != present_action::hold_starved);
    if (d.action == present_action::show) {
      ++shown;
      next_pts += frame_24p;
    } else {
      ++held;
    }
    clock += vblank_60hz;
  }
  // 600 vblanks at 60 Hz is 10 s, which is ~240 frames of 24p.
  REQUIRE(shown > 230);
  REQUIRE(shown < 250);
  REQUIRE(held > 0);
}

TEST_CASE("a frame late by more than one interval is dropped", "[presenter]") {
  const time_ns clock = ms(1000);
  // Target is clock + one vblank; this PTS is a full 100 ms behind that.
  const auto d = choose(at(clock, clock - ms(100)));
  REQUIRE(d.action == present_action::drop);
}

TEST_CASE("a frame late by less than one interval is still shown", "[presenter]") {
  const time_ns clock = ms(1000);
  const auto d = choose(at(clock, clock + vblank_60hz - ms(5)));
  REQUIRE(d.action == present_action::show);
}

TEST_CASE("playback rate scales the lateness tolerance", "[presenter]") {
  // At 4x, a vblank of wall clock covers 4 vblanks of stream time, so the
  // tolerance must shrink or we present stale frames and call it on time.
  const time_ns clock = ms(1000);
  const time_ns late_by = vblank_60hz / 2;  // half an interval at 1x

  REQUIRE(choose(at(clock, clock + vblank_60hz - late_by, true, 1.0)).action ==
          present_action::show);
  REQUIRE(choose(at(clock, clock + vblank_60hz - late_by, true, 4.0)).action ==
          present_action::drop);
}

TEST_CASE("reported error is signed and in milliseconds", "[presenter]") {
  const time_ns clock = ms(1000);
  const auto ahead = choose(at(clock, clock + vblank_60hz + ms(10)));
  REQUIRE(ahead.err_ms > 9.0);
  const auto behind = choose(at(clock, clock + vblank_60hz - ms(10)));
  REQUIRE(behind.err_ms < -9.0);
}
