// SPDX-License-Identifier: GPL-2.0-or-later
// Issue #38: the transport's idle state, headless. Both hosts run this rule.
#include <catch2/catch_test_macros.hpp>

#include "shell/transport_autohide.h"

using namespace mv::shell;

namespace {
transport_view playing_clip() {
  transport_view v;
  v.clip = true;
  v.playing = true;
  v.pointer_on_canvas = true;
  return v;
}
}  // namespace

TEST_CASE("playing, the transport leaves after the idle interval", "[autohide]") {
  transport_autohide a;
  const auto v = playing_clip();
  a.activity(1000);
  (void)a.update(v, 1000);
  REQUIRE(a.shown());
  REQUIRE(a.due_in(v, 1000) == kTransportIdleMs);
  REQUIRE_FALSE(a.update(v, 1000 + kTransportIdleMs - 1));
  REQUIRE(a.shown());
  REQUIRE(a.update(v, 1000 + kTransportIdleMs));
  REQUIRE_FALSE(a.shown());
  // Hidden and playing: nothing more to time, so no timer and no repaint.
  REQUIRE(a.due_in(v, 1000 + kTransportIdleMs) == 0);
}

TEST_CASE("activity brings it straight back and restarts the clock", "[autohide]") {
  transport_autohide a;
  const auto v = playing_clip();
  a.activity(0);
  (void)a.update(v, 0);  // the clip arrives
  (void)a.update(v, kTransportIdleMs);
  REQUIRE_FALSE(a.shown());
  a.activity(10'000);
  REQUIRE(a.update(v, 10'000));
  REQUIRE(a.shown());
  REQUIRE(a.due_in(v, 10'500) == kTransportIdleMs - 500);
}

TEST_CASE("paused or ended keeps the controls up with no timer", "[autohide]") {
  transport_autohide a;
  auto v = playing_clip();
  a.activity(0);
  (void)a.update(v, 0);  // the clip arrives
  (void)a.update(v, kTransportIdleMs);
  REQUIRE_FALSE(a.shown());
  v.playing = false;  // pause, or the clip played out
  REQUIRE(a.update(v, kTransportIdleMs + 1));
  REQUIRE(a.shown());
  REQUIRE(a.due_in(v, kTransportIdleMs + 1) == 0);
  // Resuming counts the full interval from the resume, not from the pause.
  v.playing = true;
  (void)a.update(v, 50'000);
  REQUIRE(a.shown());
  REQUIRE(a.due_in(v, 50'000) == kTransportIdleMs);
}

TEST_CASE("held (hover, scrub, menu, focus) never hides, and re-checks", "[autohide]") {
  transport_autohide a;
  auto v = playing_clip();
  v.held = true;
  a.activity(0);
  (void)a.update(v, 60'000);
  REQUIRE(a.shown());
  REQUIRE(a.due_in(v, 60'000) == kTransportIdleMs);
  v.held = false;  // let go: a full interval from here
  (void)a.update(v, 60'000 + kTransportIdleMs - 1);
  REQUIRE(a.shown());
}

TEST_CASE("a screen reader keeps the controls", "[autohide]") {
  transport_autohide a;
  auto v = playing_clip();
  v.screen_reader = true;
  a.activity(0);
  (void)a.update(v, 60'000);
  REQUIRE(a.shown());
  REQUIRE(a.due_in(v, 60'000) == 0);
}

TEST_CASE("the pointer hides only in fullscreen, over the video", "[autohide]") {
  transport_autohide a;
  auto v = playing_clip();
  a.activity(0);
  (void)a.update(v, 0);  // the clip arrives
  (void)a.update(v, kTransportIdleMs);
  REQUIRE_FALSE(a.shown());
  REQUIRE_FALSE(a.pointer_hidden());  // windowed: the system pointer stays
  v.fullscreen = true;
  REQUIRE(a.update(v, kTransportIdleMs));
  REQUIRE(a.pointer_hidden());
  v.pointer_on_canvas = false;  // over chrome, or out of the window
  REQUIRE(a.update(v, kTransportIdleMs));
  REQUIRE_FALSE(a.pointer_hidden());
  v.pointer_on_canvas = true;
  (void)a.update(v, kTransportIdleMs);
  a.activity(kTransportIdleMs + 5);
  REQUIRE(a.update(v, kTransportIdleMs + 5));
  REQUIRE(a.shown());
  REQUIRE_FALSE(a.pointer_hidden());
}

TEST_CASE("no clip: shown, pointer visible, nothing to time", "[autohide]") {
  transport_autohide a;
  transport_view v;
  v.fullscreen = true;
  v.pointer_on_canvas = true;
  (void)a.update(v, 99'000);
  REQUIRE(a.shown());
  REQUIRE_FALSE(a.pointer_hidden());
  REQUIRE(a.due_in(v, 99'000) == 0);
  // A clip opened after a long still arrives with its controls up.
  auto clip = playing_clip();
  (void)a.update(clip, 99'001);
  REQUIRE(a.shown());
}

TEST_CASE("transport keys wake the controls; navigation does not", "[autohide]") {
  REQUIRE(is_transport_command(command_id::play_pause));
  REQUIRE(is_transport_command(command_id::jump_forward));
  REQUIRE(is_transport_command(command_id::frame_back));
  REQUIRE(is_transport_command(command_id::mute));
  REQUIRE(is_transport_command(command_id::rate_up));
  REQUIRE_FALSE(is_transport_command(command_id::next));
  REQUIRE_FALSE(is_transport_command(command_id::prev));
  REQUIRE_FALSE(is_transport_command(command_id::toggle_gallery));
}
