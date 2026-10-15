// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "core/spsc_ring.h"
#include "shell/input_state.h"
#include "shell/loupe.h"

TEST_CASE("coalesced wheel input is conserved and never replayed", "[shell][input]") {
  mv::publish_slot<mv::shell::input_snapshot> slot;
  mv::shell::input_snapshot s;
  mv::shell::input_cursor cursor;
  s.wheel_total = 120;
  slot.publish(s);
  s.mouse_x = 50;
  slot.publish(s);  // movement after a wheel message
  s.wheel_total += 30;  // high-resolution wheel input
  slot.publish(s);
  REQUIRE(cursor.consume_wheel(slot.acquire()) == 1.25f);
  REQUIRE(cursor.consume_wheel(slot.acquire()) == 0.0f);
  s.wheel_total -= 60;
  slot.publish(s);
  REQUIRE(cursor.consume_wheel(slot.acquire()) == -0.5f);
}

TEST_CASE("coalesced keyboard pan steps are conserved and never replayed", "[shell][input]") {
  mv::publish_slot<mv::shell::input_snapshot> slot;
  mv::shell::input_snapshot s;
  mv::shell::input_cursor cursor;
  std::int64_t dx = 0;
  std::int64_t dy = 0;
  REQUIRE_FALSE(cursor.consume_pan(slot.acquire(), dx, dy));
  s.pan_steps_y -= 1;
  slot.publish(s);
  s.pan_steps_y -= 1;  // key-repeat before the render thread ran
  s.pan_steps_x += 1;
  slot.publish(s);
  REQUIRE(cursor.consume_pan(slot.acquire(), dx, dy));
  REQUIRE(dx == 1);
  REQUIRE(dy == -2);
  REQUIRE_FALSE(cursor.consume_pan(slot.acquire(), dx, dy));
}

TEST_CASE("a parked cursor is not repeated activity", "[shell][input]") {
  mv::shell::input_snapshot s;
  mv::shell::input_cursor cursor;
  s.mouse_in_client = true;
  ++s.activity_seq;
  REQUIRE(cursor.consume_activity(s));
  REQUIRE_FALSE(cursor.consume_activity(s));
  ++s.toggle_overlay_seq;
  ++s.activity_seq;
  REQUIRE(cursor.consume_activity(s));
  REQUIRE_FALSE(cursor.consume_activity(s));
}

TEST_CASE("the loupe sits at the cursor, or the centre, inside the canvas", "[shell][input][loupe]") {
  mv::shell::input_snapshot s;
  s.dpi_scale = 1.0f;
  // No cursor: the canvas centre.
  auto b = mv::shell::loupe_rect(s, 0.0f, 0.0f, 1000.0f, 800.0f);
  REQUIRE(b.size == 240.0f);
  REQUIRE(b.point_x == 500.0f);
  REQUIRE(b.point_y == 400.0f);
  REQUIRE(b.x == 380.0f);
  REQUIRE(b.y == 280.0f);
  // Arrows nudge by a twentieth of the shorter side.
  s.loupe_steps_x = 2;
  s.loupe_steps_y = -1;
  b = mv::shell::loupe_rect(s, 0.0f, 0.0f, 1000.0f, 800.0f);
  REQUIRE(b.point_x == 580.0f);
  REQUIRE(b.point_y == 360.0f);
  // A cursor above the canvas (over the command bar) is clamped into it, and
  // the square never leaves it.
  s.loupe_steps_x = 0;
  s.loupe_steps_y = 0;
  s.mouse_in_client = true;
  s.mouse_x = 5.0f;
  s.mouse_y = 5.0f;
  b = mv::shell::loupe_rect(s, 0.0f, 100.0f, 1000.0f, 700.0f);
  REQUIRE(b.point_x == 5.0f);
  REQUIRE(b.point_y == 100.0f);
  REQUIRE(b.x == 0.0f);
  REQUIRE(b.y == 100.0f);
  // Scaled by DPI, but never larger than the canvas.
  s.dpi_scale = 2.0f;
  b = mv::shell::loupe_rect(s, 0.0f, 0.0f, 1000.0f, 300.0f);
  REQUIRE(b.size == 300.0f);
}

TEST_CASE("the loupe looks at its point at 100 %, or twice the zoom past it", "[shell][input][loupe]") {
  mv::shell::loupe_box b;
  b.point_x = 580.0f;
  b.point_y = 400.0f;
  auto c = mv::shell::loupe_view(b, 0.0f, 0.0f, 1000.0f, 800.0f, 100.0f, 50.0f, 0.5f);
  REQUIRE(c.zoom == 1.0f);
  REQUIRE(c.pan_x == 260.0f);  // 80 screen px at 0.5 is 160 image px
  REQUIRE(c.pan_y == 50.0f);
  c = mv::shell::loupe_view(b, 0.0f, 0.0f, 1000.0f, 800.0f, 100.0f, 50.0f, 2.0f);
  REQUIRE(c.zoom == 4.0f);
  REQUIRE(c.pan_x == 140.0f);
  c = mv::shell::loupe_view(b, 0.0f, 0.0f, 1000.0f, 800.0f, 100.0f, 50.0f, 40.0f);
  REQUIRE(c.zoom == 64.0f);
}

// Issue #408: the home's theme and its ground effect's rest rule.
TEST_CASE("the home's ground runs every frame in front, 10 Hz behind, then rests", "[shell][home]") {
  using mv::shell::home_effect;
  using mv::shell::home_ground_clock;
  constexpr double kFast = home_ground_clock::kEveryFrame;
  constexpr double kSlow = home_ground_clock::kSlowTick;
  home_ground_clock c;
  // No input yet: nothing moves (the labs record launch as one).
  REQUIRE(c.interval(true, home_effect::rain, true, true, false, 0.0) == 0.0);
  c.input(0.0);
  // In front and in use: every display frame.
  REQUIRE(c.interval(true, home_effect::rain, true, true, false, 1.0) == kFast);
  // Behind another app but visible: 10 a second.
  REQUIRE(c.interval(true, home_effect::rain, false, true, false, 1.0) == kSlow);
  // In front but quiet for 5 s: 10 a second until a minute has passed, then rest.
  REQUIRE(c.interval(true, home_effect::rain, true, true, false, 5.0) == kSlow);
  REQUIRE(c.interval(true, home_effect::rain, false, true, false, 59.9) == kSlow);
  REQUIRE(c.interval(true, home_effect::rain, true, true, false, 60.0) == 0.0);
  // Never off the home, without a moving effect, hidden or covered, or under Reduce Motion.
  REQUIRE(c.interval(false, home_effect::rain, true, true, false, 1.0) == 0.0);
  REQUIRE(c.interval(true, home_effect::dots, true, true, false, 1.0) == 0.0);
  REQUIRE(c.interval(true, home_effect::none, true, true, false, 1.0) == 0.0);
  REQUIRE(c.interval(true, home_effect::rain, true, false, false, 1.0) == 0.0);
  REQUIRE(c.interval(true, home_effect::rain, true, true, true, 1.0) == 0.0);
  // In use, every pass ticks (the display paces the loop); the first does not
  // advance the clock, the next advances by the real frame time.
  REQUIRE(c.step(kFast, 1.0));
  REQUIRE(c.time == 0.0);
  REQUIRE(c.step(kFast, 1.0 + 1.0 / 60.0));
  REQUIRE(c.time > 0.0166);
  REQUIRE(c.step(kFast, 1.0 + 1.0 / 60.0 + 1.0 / 120.0));  // a ProMotion frame
  // Slow ticks wait their interval; a wait that wakes a hair early still ticks,
  // so the loop never spins.
  REQUIRE(c.step(kSlow, 1.5));
  REQUIRE_FALSE(c.step(kSlow, 1.55));
  REQUIRE(c.until_next(kSlow, 1.55) > 0.0);
  REQUIRE(c.step(kSlow, 1.6 - 0.001));
  // Dropping to the slow rate keeps the clock continuous; a late tick never jumps.
  const double t0 = c.time;
  REQUIRE(c.step(kSlow, 2.0));
  REQUIRE(c.time - t0 <= home_ground_clock::kMaxStep + 1e-9);
  // Resting, then a resume, does not jump the clock.
  const double before = c.time;
  REQUIRE_FALSE(c.step(0.0, 70.0));
  c.input(100.0);
  REQUIRE(c.step(kFast, 100.0));
  REQUIRE(c.time == before);
}

TEST_CASE("home effects are named as the chromes name them", "[shell][home]") {
  using mv::shell::home_effect;
  using mv::shell::home_effect_from_name;
  REQUIRE(home_effect_from_name("none") == static_cast<int>(home_effect::none));
  REQUIRE(home_effect_from_name("rain") == static_cast<int>(home_effect::rain));
  REQUIRE(home_effect_from_name("perlin-flow") == static_cast<int>(home_effect::perlin_flow));
  REQUIRE(home_effect_from_name("rai") == -1);
  REQUIRE(home_effect_from_name("rainy") == -1);
  REQUIRE(home_effect_from_name(nullptr) == -1);
  // A theme change is one redraw: the labs compare the whole struct.
  mv::shell::input_snapshot a;
  mv::shell::input_snapshot b = a;
  REQUIRE(a.home == b.home);
  b.home.accent = 0xFF0000FFu;
  REQUIRE_FALSE(a.home == b.home);
}
