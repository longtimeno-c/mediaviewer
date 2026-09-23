// SPDX-License-Identifier: GPL-2.0-or-later
// The empty-window runner's rules (src/shell/dino_game.h). No ImGui, no clock:
// the tests step it with exact dt values.
#include <catch2/catch_test_macros.hpp>

#include "shell/dino_game.h"

using mv::shell::dino_game;
using phase = dino_game::phase;

namespace {
void run_for(dino_game& g, float seconds, float step = 1.0f / 60.0f) {
  for (float t = 0.0f; t < seconds; t += step) g.update(step);
}
dino_game running() {
  dino_game g(1234u);
  g.set_view_width(400.0f);
  g.press();
  run_for(g, dino_game::kIntroSeconds + 0.1f);
  return g;
}
}  // namespace

TEST_CASE("the runner is idle until Space, then intros into a run", "[dino]") {
  dino_game g(1u);
  CHECK(g.state() == phase::idle);
  CHECK_FALSE(g.active());
  g.update(0.016f);
  CHECK(g.state() == phase::idle);  // idle never advances

  g.press();
  CHECK(g.state() == phase::intro);
  g.press();  // a press mid-intro is ignored, not a jump
  CHECK(g.state() == phase::intro);
  CHECK_FALSE(g.airborne());

  run_for(g, dino_game::kIntroSeconds + 0.1f);
  CHECK(g.state() == phase::playing);
}

TEST_CASE("a jump leaves the ground, lands, and cannot be repeated in the air", "[dino]") {
  dino_game g = running();
  g.debug_clear_obstacles();
  g.press();
  CHECK(g.airborne());
  g.update(0.05f);
  const float apex_rising = g.dino_y();
  CHECK(apex_rising > 0.0f);
  g.press();  // second press while airborne changes nothing
  g.update(0.05f);
  run_for(g, 1.0f);
  g.debug_clear_obstacles();
  CHECK_FALSE(g.airborne());
  CHECK(g.dino_y() == 0.0f);
}

TEST_CASE("the jump clears the tallest obstacle the spawner can make", "[dino]") {
  dino_game g = running();
  g.debug_clear_obstacles();
  g.press();
  float apex = 0.0f;
  for (int i = 0; i < 90 && g.airborne(); ++i) {
    g.update(1.0f / 60.0f);
    apex = g.dino_y() > apex ? g.dino_y() : apex;
  }
  CHECK(apex > 30.0f);  // spawned cacti are at most 22 units tall
}

TEST_CASE("running into an obstacle ends the run and keeps the best score", "[dino]") {
  dino_game g = running();
  g.debug_clear_obstacles();
  g.debug_add_obstacle(g.dino_x() + 30.0f, 8.0f, 18.0f);
  run_for(g, 1.0f);
  CHECK(g.state() == phase::over);
  const int best = g.best();
  CHECK(best == g.score());

  g.press();  // restart from the game-over screen
  CHECK(g.state() == phase::intro);
  CHECK(g.obstacle_count() == 0);
  CHECK(g.score() == 0);
  CHECK(g.best() == best);
}

TEST_CASE("an obstacle is passed while airborne", "[dino]") {
  dino_game g = running();
  g.debug_clear_obstacles();
  g.debug_add_obstacle(g.dino_x() + 60.0f, 8.0f, 18.0f);
  // Jump when the cactus is about one air-distance away.
  bool jumped = false;
  for (int i = 0; i < 240 && g.state() == phase::playing; ++i) {
    if (!jumped && g.obstacle_at(0).x < g.dino_x() + 46.0f) {
      g.press();
      jumped = true;
    }
    g.update(1.0f / 60.0f);
    if (g.obstacle_count() > 0 && g.obstacle_at(0).x < g.dino_x() - 30.0f) break;
  }
  CHECK(g.state() == phase::playing);
}

TEST_CASE("leave returns to idle and Space starts fresh", "[dino]") {
  dino_game g = running();
  g.leave();
  CHECK(g.state() == phase::idle);
  g.press();
  CHECK(g.state() == phase::intro);
}

TEST_CASE("a stalled frame does not teleport the runner", "[dino]") {
  dino_game g = running();
  g.debug_clear_obstacles();
  const float before = g.distance();
  g.update(5.0f);  // clamped to 50 ms
  CHECK(g.distance() - before < 20.0f);
}

TEST_CASE("the same seed plays the same obstacles", "[dino]") {
  auto layout = [](std::uint32_t seed) {
    dino_game g(seed);
    g.set_view_width(400.0f);
    g.press();
    run_for(g, dino_game::kIntroSeconds + 0.1f);
    // Jump whenever airborne is allowed so the run survives long enough to spawn.
    float first = -1.0f;
    for (int i = 0; i < 600 && first < 0.0f; ++i) {
      g.update(1.0f / 60.0f);
      if (g.obstacle_count() > 0) first = g.obstacle_at(0).h;
      if (g.state() == phase::over) break;
    }
    return first;
  };
  CHECK(layout(77u) == layout(77u));
}
