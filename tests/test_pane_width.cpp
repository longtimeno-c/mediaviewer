// SPDX-License-Identifier: GPL-3.0-or-later
// shell/pane_width.h: the right column's width (docs/plans/edit-refresh.md §2.1).
#include "catch_compat.h"

#include <cmath>
#include <limits>

#include "shell/drag_frames.h"
#include "shell/pane_width.h"

using namespace mv::shell;

TEST_CASE("the inspector clamps to the theme minimum and the maximum", "[pane-width]") {
  CHECK(clamp_inspector(360.0f, 1600.0f, 0.0f) == 360.0f);
  CHECK(clamp_inspector(100.0f, 1600.0f, 0.0f) == 280.0f);
  CHECK(clamp_inspector(2000.0f, 1600.0f, 0.0f) == 720.0f);
  // The theme's own minimum (density, type scale) wins when it is wider.
  CHECK(clamp_inspector(300.0f, 1600.0f, 350.0f) == 350.0f);
  CHECK(clamp_inspector(300.0f, 1600.0f, 200.0f) == 300.0f);
  // No preference yet, or a broken one: the default.
  CHECK(clamp_inspector(0.0f, 1600.0f, 0.0f) == 360.0f);
  CHECK(clamp_inspector(std::numeric_limits<float>::quiet_NaN(), 1600.0f, 0.0f) == 360.0f);
}

TEST_CASE("the inspector never takes more than 60 % of the client", "[pane-width]") {
  // 0.6 * 1000 - 44 = 556.
  CHECK(clamp_inspector(720.0f, 1000.0f, 0.0f) == 556.0f);
  // Without a rail, 600.
  const pane_width_limits no_rail{.rail = 0.0f};
  CHECK(clamp_inspector(720.0f, 1000.0f, 0.0f, no_rail) == 600.0f);
  // Nor so wide the canvas gets under 320: 700 - 44 - 320 = 336.
  CHECK(clamp_inspector(720.0f, 700.0f, 0.0f) == 336.0f);
  // The cap never goes under the minimum.
  CHECK(clamp_inspector(720.0f, 644.0f, 0.0f) == 280.0f);
}

TEST_CASE("a client too narrow for an inspector shows the rail only", "[pane-width]") {
  // 44 + 280 + 320 = 644: below that the canvas would be under 320.
  CHECK(clamp_inspector(360.0f, 643.0f, 0.0f) == 0.0f);
  CHECK(clamp_inspector(360.0f, 644.0f, 0.0f) == 280.0f);
  CHECK(clamp_inspector(360.0f, 0.0f, 0.0f) == 0.0f);
  // A wider theme minimum moves the threshold.
  CHECK(clamp_inspector(360.0f, 700.0f, 340.0f) == 0.0f);
  CHECK(docked_right(0.0f, true, false) == 44.0f);
  // Without a rail to collapse to, the pane still shows at its minimum (or
  // half the client, if that is less).
  const pane_width_limits no_rail{.rail = 0.0f};
  CHECK(shown_inspector(360.0f, 560.0f, 0.0f, no_rail) == 280.0f);
  CHECK(shown_inspector(360.0f, 400.0f, 0.0f, no_rail) == 200.0f);
  CHECK(shown_inspector(360.0f, 1600.0f, 0.0f, no_rail) == 360.0f);
  CHECK(shown_inspector(360.0f, 500.0f, 0.0f) == 0.0f);
}

TEST_CASE("the docked width is the rail plus the inspector", "[pane-width]") {
  CHECK(docked_right(360.0f, false, false) == 0.0f);
  CHECK(docked_right(360.0f, true, false) == 404.0f);
  CHECK(docked_right(360.0f, true, true) == 44.0f);
  const pane_width_limits no_rail{.rail = 0.0f};
  CHECK(docked_right(360.0f, true, false, no_rail) == 360.0f);
  CHECK(docked_right(360.0f, false, false, no_rail) == 0.0f);
}

TEST_CASE("a release near a breakpoint snaps to it", "[pane-width]") {
  CHECK(snap_inspector(354.0f) == 360.0f);
  CHECK(snap_inspector(366.0f) == 360.0f);
  CHECK(snap_inspector(353.0f) == 353.0f);
  CHECK(snap_inspector(556.0f) == 560.0f);
  CHECK(snap_inspector(567.0f) == 567.0f);
  CHECK(snap_inspector(720.0f) == 720.0f);
}

TEST_CASE("the stored preference rounds to 4 and 0 means the default", "[pane-width]") {
  CHECK(stored_width(361.0f) == 360);
  CHECK(stored_width(362.5f) == 364);
  CHECK(stored_width(0.0f) == 0);
  CHECK(preferred_width(0) == 360.0f);
  CHECK(preferred_width(500) == 500.0f);
  CHECK(preferred_width(9000) == 720.0f);
  CHECK(preferred_width(12) == 280.0f);
}

TEST_CASE("pane wider and narrower step by 40 within the limits", "[pane-width]") {
  CHECK(step_inspector(360.0f, 1) == 400.0f);
  CHECK(step_inspector(360.0f, -1) == 320.0f);
  CHECK(step_inspector(700.0f, 1) == 720.0f);
  CHECK(step_inspector(300.0f, -1) == 280.0f);
  CHECK(step_inspector(0.0f, 1) == 400.0f);
}

TEST_CASE("width classes and the Curves square", "[pane-width]") {
  CHECK(class_of(280.0f) == width_class::compact);
  CHECK(class_of(339.0f) == width_class::compact);
  CHECK(class_of(340.0f) == width_class::regular);
  CHECK(class_of(559.0f) == width_class::regular);
  CHECK(class_of(560.0f) == width_class::wide);
  CHECK(class_of(719.0f) == width_class::wide);
  CHECK(class_of(720.0f) == width_class::xwide);
  // Fully visible at 280 / 360 / 720: never wider than the content.
  CHECK(curve_side(280.0f) == 252.0f);
  CHECK(curve_side(360.0f) == 332.0f);
  CHECK(curve_side(720.0f) == 340.0f);
}

TEST_CASE("a drag of the edge sizes the column from the pointer in a fixed client", "[pane-width]") {
  // 2026-11-05: in a 1280 pt window the edge at 876 dragged to 560. The
  // column is the client less the rail less the pointer, clamped; the client
  // (the window) never changes for it, so the canvas left is the pointer.
  const float client = 1280.0f;
  const pane_width_limits l{};
  const auto at = [&](float leading) { return shown_inspector(client - l.rail - leading, client, 0.0f, l); };
  CHECK(at(876.0f) == 360.0f);
  CHECK(at(560.0f) == 676.0f);  // under 720 and 60 % (724): the pointer wins
  CHECK(client - l.rail - at(560.0f) == 560.0f);
  // Past the caps the column stops and the canvas keeps the rest.
  CHECK(at(300.0f) == 720.0f);
  CHECK(client - l.rail - at(300.0f) >= l.canvas_min);
  // The canvas left beside a column dragged as far as it goes is never under
  // 320, whatever the client.
  for (float c = 644.0f; c <= 3000.0f; c += 37.0f) {
    const float w = shown_inspector(1.0e6f, c, 0.0f, l);
    CHECK(c - l.rail - w >= l.canvas_min);
  }
}

TEST_CASE("the drag log counts intervals only between two live presents", "[pane-width][pane-drag-soak]") {
  drag_frame_log log;
  log.on_present(0.0, true, 1.0 / 60.0);  // not armed: counted, nothing logged
  CHECK(log.presents() == 1);
  CHECK(log.intervals().empty());
  log.arm();
  log.on_present(1.000, false, 1.0 / 60.0);
  log.on_present(1.016, true, 1.0 / 60.0);   // the one before was not live
  log.on_present(1.033, true, 1.0 / 60.0);
  log.on_present(1.083, true, 1.0 / 60.0);   // 50 ms: over 2x
  log.on_present(1.100, false, 1.0 / 60.0);  // released
  const auto ms = log.intervals();
  REQUIRE(ms.size() == 2);
  CHECK(std::fabs(ms[0] - 17.0f) < 0.01f);
  CHECK(log.presents() == 6);
  const auto s = summarise(ms, log.refresh_ms());
  CHECK(s.frames == 2);
  CHECK(s.over_2x == 1);
  CHECK(std::fabs(s.max_ms - 50.0f) < 0.01f);
  CHECK(std::fabs(s.refresh_ms - 16.667f) < 0.01f);
}
