// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "shell/navigation.h"
#include "shell/settings.h"

using mv::shell::step_index;

TEST_CASE("folder stepping wraps at the ends only when wrap is on", "[shell][navigation]") {
  REQUIRE(step_index(0, 1, 5, false) == 1u);
  REQUIRE(step_index(4, 1, 5, true) == 0u);
  REQUIRE_FALSE(step_index(4, 1, 5, false).has_value());
  REQUIRE(step_index(0, -1, 5, true) == 4u);
  REQUIRE_FALSE(step_index(0, -1, 5, false).has_value());
  REQUIRE(step_index(2, 10, 5, true) == 2u);  // PageDown past the end wraps round
  REQUIRE_FALSE(step_index(0, 1, 0, true).has_value());
  REQUIRE(step_index(0, 1, 1, true) == 0u);
}

TEST_CASE("wrap is a setting, on by default, round-tripped with the others",
          "[shell][navigation]") {
  mv::shell::view_settings settings;
  REQUIRE(settings.wrap);
  REQUIRE((settings.flags() & mv::shell::kSettingWrap) != 0);
  settings.wrap = false;
  const auto round = mv::shell::view_settings::from_flags(settings.flags());
  REQUIRE_FALSE(round.wrap);
  REQUIRE(round.filmstrip_for_folder == settings.filmstrip_for_folder);
}
