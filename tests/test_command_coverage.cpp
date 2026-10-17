// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #239: every command_id is handled or explicitly excluded, per host.
// The rows live in shell/command_coverage.inc, which the dispatchers switch on.
#include <catch2/catch_test_macros.hpp>

#include "shell/command_coverage.h"
#include "shell/commands.h"

using namespace mv::shell;

TEST_CASE("every command id is handled or explicitly excluded", "[shell][commands]") {
  for (int i = 0; i < kCommandCount; ++i) {
    if (is_reserved_notification(i)) continue;
    const auto id = static_cast<command_id>(i);
    const bool mac = mac_dispatcher_handles(id);
    const bool win = win_dispatcher_handles(id);

    if (mac) {
      REQUIRE(mac_command_exclusion(id) == nullptr);
      REQUIRE(mac_command_area(id) != command_area::excluded);
    } else {
      REQUIRE(mac_command_exclusion(id) != nullptr);
      REQUIRE(mac_command_area(id) == command_area::excluded);
    }

    // Windows may handle an id outside run_command (the island switch, or Esc).
    // That is area excluded and still handles, with no exclusion string.
    if (win) {
      REQUIRE(win_command_exclusion(id) == nullptr);
    } else {
      REQUIRE(win_command_exclusion(id) != nullptr);
      REQUIRE(win_command_area(id) == command_area::excluded);
    }
    if (win_command_area(id) != command_area::excluded) REQUIRE(win);
  }

  // The omissions a hand-kept Mac list let through, and the ids Windows runs
  // outside run_command. A new row that drops either fails here.
  REQUIRE(mac_dispatcher_handles(command_id::typeahead));
  REQUIRE(mac_dispatcher_handles(command_id::game_toggle_3d));
  REQUIRE(mac_command_area(command_id::typeahead) == command_area::canvas);
  REQUIRE_FALSE(mac_dispatcher_handles(command_id::go_to));
  REQUIRE(mac_command_area(command_id::edit_workspace) == command_area::edit);
  REQUIRE(win_dispatcher_handles(command_id::back));
  REQUIRE(win_command_area(command_id::back) == command_area::excluded);
  REQUIRE(win_command_area(command_id::zoom_preset) == command_area::excluded);
  REQUIRE(win_dispatcher_handles(command_id::zoom_preset));
}
