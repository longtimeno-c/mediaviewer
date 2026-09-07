// SPDX-License-Identifier: GPL-2.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "core/spsc_ring.h"
#include "shell/input_state.h"

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
