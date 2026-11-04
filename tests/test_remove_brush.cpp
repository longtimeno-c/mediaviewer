// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Slice 7: the Remove brush (shell/remove_brush.h).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "shell/remove_brush.h"

using mv::shell::kBrushDabs;
using mv::shell::remove_brush;

TEST_CASE("remove brush: strokes spaced evenly, undone one at a time, and a mask of them", "[remove-brush]") {
  remove_brush b;
  b.set_frame(2000, 1000);
  CHECK(b.empty());
  CHECK_FALSE(b.drag(0.5f, 0.5f));  // no stroke yet
  CHECK_FALSE(b.begin(0.1f, 0.1f, 0.0f));
  REQUIRE(b.begin(0.1f, 0.5f, 0.01f));  // radius 20 px
  // Across 1000 px of the 2000 px side: one dab every 7 px.
  REQUIRE(b.drag(0.6f, 0.5f));
  b.end();
  CHECK(b.strokes() == 1);
  const std::size_t across = b.dabs().size();
  CHECK(across == 1 + 142);
  CHECK(b.dabs().back().u > 0.59f);
  // Down 500 px of the 1000 px side, the same density in pixels.
  REQUIRE(b.begin(0.9f, 0.0f, 0.01f));
  REQUIRE(b.drag(0.9f, 0.5f));
  b.end();
  CHECK(b.dabs().size() - across == 1 + 71);
  CHECK_FALSE(b.drag(0.9f, 0.9f));  // released: nothing more

  const auto m = b.mask(2000, 1000);
  CHECK(m[500 * 2000 + 600] == 255);   // on the first stroke
  CHECK(m[530 * 2000 + 600] == 0);     // 30 px below it
  CHECK(m[250 * 2000 + 1800] == 255);  // on the second
  CHECK(m[900 * 2000 + 1800] == 0);    // past its end
  CHECK(std::all_of(m.begin(), m.end(), [](std::uint8_t v) { return v == 0 || v == 255; }));
  CHECK(b.mask(0, 0).empty());

  CHECK(b.undo_stroke());
  CHECK(b.dabs().size() == across);
  CHECK(b.undo_stroke());
  CHECK(b.empty());
  CHECK_FALSE(b.undo_stroke());

  // Full: the stroke stops adding, and says so.
  REQUIRE(b.begin(0.0f, 0.5f, 0.001f));
  CHECK_FALSE(b.drag(1.0f, 0.5f));
  CHECK(b.full());
  CHECK(b.dabs().size() == kBrushDabs);
  CHECK_FALSE(b.begin(0.5f, 0.5f, 0.01f));
  b.clear();
  CHECK(b.empty());
  CHECK(b.strokes() == 0);
}

TEST_CASE("remove brush: redo, erase dabs and the hand-off", "[remove-brush]") {
  remove_brush b;
  b.set_frame(1000, 1000);
  REQUIRE(b.begin(0.2f, 0.5f, 0.05f));
  REQUIRE(b.drag(0.8f, 0.5f));
  b.end();
  const std::size_t painted = b.dabs().size();
  CHECK_FALSE(b.can_redo());
  REQUIRE(b.undo_stroke());
  CHECK(b.can_redo());
  REQUIRE(b.redo_stroke());
  CHECK(b.dabs().size() == painted);
  CHECK(b.strokes() == 1);
  CHECK_FALSE(b.redo_stroke());

  // An erase stroke over the middle takes it out of the mask and the hand-off.
  REQUIRE(b.begin(0.5f, 0.5f, 0.08f, /*erase=*/true));
  b.end();
  CHECK(b.dabs().back().erase == 1);
  const auto m = b.mask(1000, 1000);
  CHECK(m[500 * 1000 + 500] == 0);
  CHECK(m[500 * 1000 + 250] == 255);
  const auto flat = b.handoff();
  REQUIRE(flat.size() % 3 == 0);
  CHECK(flat.size() / 3 < painted);
  for (std::size_t i = 0; i < flat.size(); i += 3) CHECK(std::abs(flat[i] - 0.5f) > 0.07f);

  // A new stroke ends the redo chain; clear forgets it too.
  REQUIRE(b.undo_stroke());
  REQUIRE(b.begin(0.1f, 0.1f, 0.01f));
  b.end();
  CHECK_FALSE(b.can_redo());
  b.clear();
  CHECK_FALSE(b.redo_stroke());
}
