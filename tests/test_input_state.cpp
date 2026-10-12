// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

#include "core/spsc_ring.h"
#include "shell/compare.h"
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

// Issue #208: `|` two-up compare (shell/compare.h), shared by both labs.
TEST_CASE("the compare split halves the canvas in whole pixels with a gap",
          "[shell][input][compare]") {
  auto s = mv::shell::split_compare(0.0f, 1001.0f, 1.0f);
  REQUIRE(s.valid);
  REQUIRE(s.left_x == 0.0f);
  REQUIRE(s.left_w == 496.0f);  // (1001 - 8) / 2, floored
  REQUIRE(s.right_x == 504.0f);
  REQUIRE(s.right_w == 497.0f);
  REQUIRE(s.left_w + mv::shell::kCompareGapDip + s.right_w == 1001.0f);

  // After a docked left chrome, scaled by DPI: the halves end where the canvas does.
  s = mv::shell::split_compare(200.0f, 1000.0f, 2.0f);
  REQUIRE(s.valid);
  REQUIRE(s.left_x == 200.0f);
  REQUIRE(s.left_w == 492.0f);
  REQUIRE(s.right_x == 708.0f);
  REQUIRE(s.right_x + s.right_w == 1200.0f);

  // Too narrow to show two pictures: one-up.
  REQUIRE_FALSE(mv::shell::split_compare(0.0f, 60.0f, 1.0f).valid);
}

TEST_CASE("the wheel over the left half zooms toward the same spot", "[shell][input][compare]") {
  const auto s = mv::shell::split_compare(0.0f, 1008.0f, 1.0f);  // 500 | 8 | 500
  REQUIRE(s.right_x == 508.0f);
  REQUIRE(mv::shell::compare_cursor_x(100.0f, s) == 608.0f);
  REQUIRE(mv::shell::compare_cursor_x(700.0f, s) == 700.0f);   // already in the right half
  REQUIRE(mv::shell::compare_cursor_x(504.0f, s) == 1007.0f);  // the gap clamps inside
  const mv::shell::compare_split none;
  REQUIRE(mv::shell::compare_cursor_x(100.0f, none) == 100.0f);
}

TEST_CASE("compare links the camera: a burst frame is the identical view",
          "[shell][input][compare]") {
  // Same size: the camera is shared as is, pan, zoom and 100 % alike.
  auto c = mv::shell::linked_camera(1234.5f, 678.0f, 1.0f, 6000.0f, 4000.0f, 6000.0f, 4000.0f,
                                    800.0f, 900.0f);
  REQUIRE(c.pan_x == 1234.5f);
  REQUIRE(c.pan_y == 678.0f);
  REQUIRE(c.zoom == 1.0f);

  // Half the resolution, same aspect: the same region at the same apparent size.
  c = mv::shell::linked_camera(3000.0f, 1000.0f, 0.5f, 6000.0f, 4000.0f, 3000.0f, 2000.0f, 800.0f,
                               900.0f);
  REQUIRE(c.pan_x == 1500.0f);
  REQUIRE(c.pan_y == 500.0f);
  REQUIRE(std::abs(c.zoom - 1.0f) < 1e-5f);

  // At fit, a different aspect is fitted too (zoom relative to each one's fit).
  const float pane_w = 800.0f;
  const float pane_h = 900.0f;
  const float fit_cur = std::min(pane_w / 6000.0f, pane_h / 4000.0f);
  const float fit_prev = std::min(pane_w / 4000.0f, pane_h / 6000.0f);
  c = mv::shell::linked_camera(3000.0f, 2000.0f, fit_cur, 6000.0f, 4000.0f, 4000.0f, 6000.0f,
                               pane_w, pane_h);
  REQUIRE(c.pan_x == 2000.0f);
  REQUIRE(c.pan_y == 3000.0f);
  REQUIRE(std::abs(c.zoom - fit_prev) < 1e-6f);

  // Nothing to link to: the camera passes through.
  c = mv::shell::linked_camera(10.0f, 20.0f, 2.0f, 0.0f, 0.0f, 100.0f, 100.0f, 800.0f, 900.0f);
  REQUIRE(c.pan_x == 10.0f);
  REQUIRE(c.zoom == 2.0f);
}

TEST_CASE("compare is a level on the snapshot, off by default", "[shell][input][compare]") {
  mv::publish_slot<mv::shell::input_snapshot> slot;
  mv::shell::input_snapshot s;
  REQUIRE_FALSE(s.compare);
  s.compare = true;
  slot.publish(s);
  REQUIRE(slot.acquire().compare);
}
