// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #44: no clip plays under the gallery, headless. Both hosts run this rule.
#include <catch2/catch_test_macros.hpp>

#include "player/playback_hold.h"

using namespace mv::player;

namespace {
held_clip clip(std::uint64_t view, play_state state) { return {true, view, state}; }
constexpr held_clip kNone{};
}  // namespace

TEST_CASE("entering pauses a playing clip and leaving resumes it", "[hold]") {
  playback_hold h;
  REQUIRE(h.hold(5, clip(5, play_state::playing)) == hold_action::pause);
  REQUIRE(h.held());
  REQUIRE(h.release(5, clip(5, play_state::paused), true) == hold_action::play);
  REQUIRE_FALSE(h.held());
}

TEST_CASE("an already paused clip is left paused", "[hold]") {
  playback_hold h;
  REQUIRE(h.hold(5, clip(5, play_state::paused)) == hold_action::none);
  REQUIRE(h.release(5, clip(5, play_state::paused), true) == hold_action::none);
  REQUIRE(h.hold(6, clip(6, play_state::ended)) == hold_action::none);
  REQUIRE(h.release(6, clip(6, play_state::ended), true) == hold_action::none);
}

TEST_CASE("a clip selected under the hold does not autoplay, then or after", "[hold]") {
  playback_hold h;
  (void)h.hold(5, clip(5, play_state::playing));
  // The gallery moves to another clip: view 6 is adopted while held.
  REQUIRE_FALSE(h.autoplay(6));
  REQUIRE(h.release(6, clip(6, play_state::paused), true) == hold_action::none);
  REQUIRE_FALSE(h.autoplay(6));
  // A click on a tile selects and closes at once; the clip lands after release.
  (void)h.hold(7, kNone);
  (void)h.release(8, clip(7, play_state::paused), true);
  REQUIRE_FALSE(h.autoplay(8));
  // The next navigation is ordinary again.
  REQUIRE(h.autoplay(9));
}

TEST_CASE("coming back to the same item is not the same clip", "[hold]") {
  playback_hold h;
  (void)h.hold(5, clip(5, play_state::playing));
  // Away and back: the item reopens under a new view.
  REQUIRE(h.release(7, clip(7, play_state::paused), true) == hold_action::none);
}

TEST_CASE("a clip still opening on entry starts once the hold lifts", "[hold]") {
  playback_hold h;
  REQUIRE(h.hold(5, kNone) == hold_action::none);
  REQUIRE_FALSE(h.autoplay(5));
  // Arrives under the gallery, paused, then the gallery closes.
  REQUIRE(h.release(5, clip(5, play_state::paused), true) == hold_action::play);
  // Or it had not arrived yet by then.
  playback_hold late;
  (void)late.hold(5, kNone);
  REQUIRE(late.release(5, kNone, true) == hold_action::none);
  REQUIRE(late.autoplay(5));
}

TEST_CASE("a stale clip is silenced but not remembered", "[hold]") {
  playback_hold h;
  REQUIRE(h.hold(6, clip(5, play_state::playing)) == hold_action::pause);
  REQUIRE(h.release(6, clip(5, play_state::paused), true) == hold_action::none);
}

TEST_CASE("leaving for something new keeps the held clip paused", "[hold]") {
  playback_hold h;
  (void)h.hold(5, clip(5, play_state::playing));
  REQUIRE(h.release(5, clip(5, play_state::paused), false) == hold_action::none);
  REQUIRE_FALSE(h.autoplay(5));
}

TEST_CASE("play or pause under the hold is the user's call", "[hold]") {
  playback_hold h;
  (void)h.hold(5, clip(5, play_state::playing));
  h.user_transport();
  REQUIRE(h.release(5, clip(5, play_state::paused), true) == hold_action::none);
  // Outside a hold it changes nothing.
  h.user_transport();
  (void)h.hold(5, clip(5, play_state::playing));
  REQUIRE(h.release(5, clip(5, play_state::paused), true) == hold_action::play);
}

TEST_CASE("repeated transitions are idempotent", "[hold]") {
  playback_hold h;
  REQUIRE(h.hold(5, clip(5, play_state::playing)) == hold_action::pause);
  REQUIRE(h.hold(5, clip(5, play_state::paused)) == hold_action::none);
  REQUIRE(h.release(5, clip(5, play_state::paused), true) == hold_action::play);
  REQUIRE(h.release(5, clip(5, play_state::playing), true) == hold_action::none);
  REQUIRE(h.autoplay(6));
}
