// SPDX-License-Identifier: GPL-2.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "shell/commands.h"
#include "shell/key_router.h"

using namespace mv::shell;

namespace {

key_event down(key k, std::uint8_t mods = mod_none) { return {k, mods, false, false}; }
key_event rep(key k, std::uint8_t mods = mod_none) { return {k, mods, true, false}; }
key_event up(key k) { return {k, mod_none, false, true}; }

view_state still() {
  view_state s;
  s.item = item_kind::still;
  return s;
}

view_state clip() {
  view_state s;
  s.item = item_kind::clip;
  return s;
}

}  // namespace

TEST_CASE("no two bindings share a key, modifiers and mode", "[shell][commands]") {
  std::set<std::tuple<int, int, int>> seen;
  for (const auto& b : default_bindings()) {
    REQUIRE(b.modes != 0);
    REQUIRE(b.command != command_id::none);
    // PR 7: Open RAW / Open JPEG ship listed but unbound (plan/04, plan/16).
    if (b.k == key::none) {
      REQUIRE((b.command == command_id::open_raw || b.command == command_id::open_jpeg));
      continue;
    }
    for (int m = 0; m < kModeCount; ++m) {
      if ((b.modes & (1 << m)) == 0) continue;
      INFO("key " << static_cast<int>(b.k) << " mods " << int(b.mods) << " mode " << m);
      REQUIRE(seen.insert({static_cast<int>(b.k), b.mods, m}).second);
    }
  }
}

TEST_CASE("every command is bound unless it is island-only", "[shell][commands]") {
  std::set<int> bound;
  for (const auto& b : default_bindings()) {
    bound.insert(static_cast<int>(b.command));
    if (b.hold != command_id::none) bound.insert(static_cast<int>(b.hold));
    if (b.release != command_id::none) bound.insert(static_cast<int>(b.release));
    if (b.policy == repeat_policy::tap_hold) {
      REQUIRE(b.hold != command_id::none);
      REQUIRE(b.release != command_id::none);
    }
    if (b.policy == repeat_policy::momentary) REQUIRE(b.release != command_id::none);
  }
  for (const auto& info : command_infos()) {
    INFO(info.name);
    REQUIRE((info.keyless || bound.count(static_cast<int>(info.id)) == 1));
  }
}

TEST_CASE("command ids are dense, named and unique", "[shell][commands]") {
  std::set<std::string> names;
  std::set<int> ids;
  for (const auto& info : command_infos()) {
    const int id = static_cast<int>(info.id);
    REQUIRE(id > 0);
    REQUIRE(id < kCommandCount);
    REQUIRE_FALSE(is_reserved_notification(id));
    // A retired id may keep a named row so the wire enum stays documented, but
    // only a keyless one: it is never bound, listed in Settings, or shown by `?`.
    if (is_retired_command(id)) REQUIRE(info.keyless);
    REQUIRE(std::strlen(info.name) > 0);
    REQUIRE(names.insert(info.name).second);
    REQUIRE(ids.insert(id).second);
    REQUIRE(find_command(info.id) == &info);
  }
  // Every id below count is either a command or a reserved wire notification.
  for (int id = 1; id < kCommandCount; ++id) {
    INFO("id " << id);
    // `palette` (76) is retired since the Ctrl+K palette was dropped
    // (plan/12 2026-09-13); it keeps its value so later ids do not shift.
    REQUIRE((ids.count(id) == 1 || is_reserved_notification(id) || is_retired_command(id)));
  }
  for (const auto& b : default_bindings()) REQUIRE(find_command(b.command) != nullptr);
}

TEST_CASE("the PR 3-5 island command ids keep their wire values", "[shell][commands]") {
  REQUIRE(static_cast<int>(command_id::open) == 1);
  REQUIRE(static_cast<int>(command_id::fit) == 2);
  REQUIRE(static_cast<int>(command_id::zoom_preset) == 6);
  REQUIRE(static_cast<int>(command_id::prev) == 9);
  REQUIRE(static_cast<int>(command_id::next) == 10);
  REQUIRE(static_cast<int>(command_id::gallery_activate) == 14);
  REQUIRE(static_cast<int>(command_id::toggle_filmstrip) == 17);
  REQUIRE(static_cast<int>(command_id::back) == 21);
  REQUIRE(static_cast<int>(command_id::clipping) == 46);
  REQUIRE(static_cast<int>(command_id::help) == 75);
  REQUIRE(static_cast<int>(command_id::reveal_in_explorer) == 80);
  REQUIRE(static_cast<int>(command_id::open_settings) == 84);
}

TEST_CASE("number row is zoom, never rating", "[shell][commands]") {
  key_router r;
  REQUIRE(r.lookup(char_key('0'), mod_none, mode::browse)->command == command_id::fit);
  REQUIRE(r.lookup(char_key('1'), mod_none, mode::browse)->command == command_id::one_to_one);
  REQUIRE(r.lookup(char_key('2'), mod_none, mode::browse)->command == command_id::zoom_200);
  REQUIRE(r.lookup(char_key('3'), mod_none, mode::browse)->command == command_id::zoom_400);
  REQUIRE(r.lookup(char_key('4'), mod_none, mode::browse)->command == command_id::fill);
  REQUIRE(r.lookup(char_key('0'), mod_ctrl, mode::browse)->command == command_id::reset_view);
}

TEST_CASE("PR 5 transport keys are table rows in video mode only", "[shell][router]") {
  key_router r;
  REQUIRE(r.lookup(char_key('A'), mod_none, mode::video)->command == command_id::prev);
  REQUIRE(r.lookup(char_key('D'), mod_none, mode::video)->command == command_id::next);
  REQUIRE(r.lookup(char_key('J'), mod_none, mode::video)->command == command_id::jump_back);
  REQUIRE(r.lookup(char_key('K'), mod_none, mode::video)->command == command_id::pause);
  REQUIRE(r.lookup(char_key('L'), mod_none, mode::video)->command == command_id::jump_forward);
  REQUIRE(r.lookup(char_key(','), mod_none, mode::video)->command == command_id::frame_back);
  REQUIRE(r.lookup(char_key('.'), mod_none, mode::video)->command == command_id::frame_forward);
  REQUIRE(r.lookup(char_key('Q'), mod_none, mode::video)->command == command_id::skim_back);
  REQUIRE(r.lookup(char_key('E'), mod_none, mode::video)->command == command_id::skim_forward);
  REQUIRE(r.lookup(char_key('Q'), mod_shift, mode::video)->command == command_id::rate_down);
  REQUIRE(r.lookup(char_key('E'), mod_shift, mode::video)->command == command_id::rate_up);
  // Q/E on a still fall through to the island rather than being swallowed.
  REQUIRE(r.lookup(char_key('Q'), mod_none, mode::browse) == nullptr);
  REQUIRE(r.lookup(char_key('E'), mod_none, mode::browse) == nullptr);
  const auto q = r.on_key(down(char_key('Q')), still());
  REQUIRE_FALSE(q.handled);
}

TEST_CASE("mode is derived from state, not remembered", "[shell][router]") {
  view_state s;
  REQUIRE(resolve_mode(s) == mode::browse);
  s.item = item_kind::clip;
  REQUIRE(resolve_mode(s) == mode::video);
  s.item = item_kind::animation;
  REQUIRE(resolve_mode(s) == mode::video);
  s.slideshow = true;
  REQUIRE(resolve_mode(s) == mode::slideshow);
  s.focus = focus_kind::filmstrip;
  REQUIRE(resolve_mode(s) == mode::island);
  // Command bar, transport and a flyout HWND are not a mode: A/D and Q/E
  // still do what they do on the canvas.
  s.focus = focus_kind::command_bar;
  REQUIRE(resolve_mode(s) == mode::slideshow);
  s.slideshow = false;
  s.item = item_kind::clip;
  REQUIRE(resolve_mode(s) == mode::video);
  s.focus = focus_kind::transport;
  REQUIRE(resolve_mode(s) == mode::video);
  s.focus = focus_kind::gallery;
  REQUIRE(resolve_mode(s) == mode::island);
}

TEST_CASE("F11 toggles fullscreen across viewing modes without repeating", "[shell][router]") {
  for (const auto focus : {focus_kind::canvas, focus_kind::command_bar,
                           focus_kind::filmstrip, focus_kind::gallery, focus_kind::transport}) {
    key_router r;
    auto s = still();
    s.focus = focus;
    for (const bool gallery : {false, true}) {
      s.gallery_open = gallery;
      REQUIRE(r.on_key(down(key::f11), s).command == command_id::fullscreen);
      REQUIRE_FALSE(r.on_key(rep(key::f11), s).handled);
    }
    s.focus = focus_kind::text;
    REQUIRE_FALSE(r.on_key(down(key::f11), s).handled);
  }
}

TEST_CASE("Settings owns shortcuts and Escape until XAML dispatch completes", "[shell][router]") {
  key_router r;
  for (const auto focus : {focus_kind::canvas, focus_kind::command_bar, focus_kind::filmstrip,
                           focus_kind::gallery, focus_kind::transport, focus_kind::text}) {
    auto s = clip();
    s.focus = focus;
    s.settings_open = true;
    s.fullscreen = true;
    s.gallery_open = true;
    s.slideshow = true;
    // Existing viewer bindings must neither run nor eat a replacement chord.
    for (const auto& b : default_bindings()) {
      REQUIRE_FALSE(r.on_key(down(b.k, b.mods), s).handled);
      REQUIRE_FALSE(r.on_key(rep(b.k, b.mods), s).handled);
    }
    REQUIRE_FALSE(r.on_key(down(key::escape), s).handled);
    s.settings_open = false;
    s.focus = focus_kind::canvas;
    REQUIRE(r.on_key(down(key::f11), s).command == command_id::fullscreen);
  }
}

TEST_CASE("entering Settings can release active viewer holds without firing taps", "[shell][router]") {
  key_router r;
  REQUIRE(r.on_key(down(char_key('Z')), still()).command == command_id::loupe);
  REQUIRE(r.on_key(down(char_key('Q')), clip()).handled);
  command_id released[key_router::kHeldSlots]{};
  REQUIRE(r.cancel_holds(released) == 1);
  REQUIRE(released[0] == command_id::loupe_release);
  auto s = clip();
  s.settings_open = true;
  REQUIRE_FALSE(r.on_key(up(char_key('Q')), s).handled);
  REQUIRE_FALSE(r.on_key(up(char_key('Z')), s).handled);
}

TEST_CASE("gallery thumbnail sizing has its own defaults and supports remapping", "[shell][router]") {
  key_router r;
  r.rebuild(default_bindings());
  auto s = still();
  s.gallery_open = true;
  for (const auto focus : {focus_kind::canvas, focus_kind::gallery, focus_kind::command_bar}) {
    s.focus = focus;
    REQUIRE(r.on_key(down(char_key('+')), s).command == command_id::gallery_larger);
    REQUIRE(r.on_key(down(char_key('=')), s).command == command_id::gallery_larger);
    REQUIRE(r.on_key(rep(char_key('-')), s).command == command_id::gallery_smaller);
  }
  s.focus = focus_kind::text;
  REQUIRE_FALSE(r.on_key(down(char_key('+')), s).handled);
  REQUIRE(r.on_key(down(char_key('+')), still()).command == command_id::zoom_in);
  REQUIRE(r.on_key(down(char_key('-')), still()).command == command_id::zoom_out);
  auto show = still();
  show.slideshow = true;
  REQUIRE(r.on_key(down(char_key('+')), show).command == command_id::slideshow_faster);

  // A Settings remap to a letter must not be swallowed by gallery typeahead.
  std::vector<binding> remapped(default_bindings().begin(), default_bindings().end());
  for (auto& b : remapped) {
    if (b.command == command_id::gallery_larger && b.k == char_key('+')) b.k = char_key('U');
  }
  r.rebuild(remapped);
  s.focus = focus_kind::gallery;
  REQUIRE(r.on_key(down(char_key('U')), s).command == command_id::gallery_larger);
  REQUIRE_FALSE(r.on_key(down(char_key('+')), s).handled);
}

TEST_CASE("visible gallery owns row navigation and Enter before focus moves", "[shell][router]") {
  key_router r;
  for (const auto focus : {focus_kind::canvas, focus_kind::command_bar,
                           focus_kind::gallery, focus_kind::filmstrip}) {
    auto s = clip();
    s.gallery_open = true;
    s.focus = focus;
    REQUIRE(resolve_mode(s) == mode::gallery);
    REQUIRE(r.on_key(down(char_key('W')), s).command == command_id::gallery_up);
    REQUIRE(r.on_key(rep(char_key('S')), s).command == command_id::gallery_down);
    REQUIRE(r.on_key(down(key::up), s).command == command_id::gallery_up);
    REQUIRE(r.on_key(down(key::down), s).command == command_id::gallery_down);
    REQUIRE(r.on_key(down(char_key('A')), s).command == command_id::prev);
    REQUIRE(r.on_key(down(char_key('D')), s).command == command_id::next);
    REQUIRE(r.on_key(down(key::enter), s).command == command_id::gallery_open_selected);
    REQUIRE_FALSE(r.on_key(rep(key::enter), s).handled);
    s.focus = focus_kind::text;
    REQUIRE_FALSE(r.on_key(down(char_key('W')), s).handled);
    REQUIRE_FALSE(r.on_key(down(key::enter), s).handled);
  }
  REQUIRE_FALSE(r.on_key(down(key::enter), still()).handled);
  REQUIRE_FALSE(r.on_key(down(key::enter), clip()).handled);
  REQUIRE(r.on_key(down(key::f5), still()).command == command_id::slideshow_start);
  REQUIRE(r.on_key(down(key::f5), clip()).command == command_id::slideshow_start);
  // No default Enter binding can start fullscreen or a slideshow in any mode.
  for (const auto& b : default_bindings()) {
    if (b.k != key::enter || b.mods != mod_none) continue;
    REQUIRE(b.command == command_id::gallery_open_selected);
  }
}

TEST_CASE("Space: next on a still, play/pause on a clip, pause in a slideshow", "[shell][router]") {
  key_router r;
  REQUIRE(r.on_key(down(key::space), still()).command == command_id::next);
  REQUIRE(r.on_key(down(key::space), view_state{}).command == command_id::next);
  REQUIRE(r.on_key(down(key::space), clip()).command == command_id::play_pause);
  view_state show = clip();
  show.slideshow = true;
  REQUIRE(r.on_key(down(key::space), show).command == command_id::slideshow_pause);
  // The slideshow lands on a still: Space still pauses the slideshow and does
  // not reach a clip that is no longer there.
  show.item = item_kind::still;
  REQUIRE(r.on_key(down(key::space), show).command == command_id::slideshow_pause);
  show.slideshow = false;
  REQUIRE(r.on_key(down(key::space), show).command == command_id::next);
  REQUIRE(r.on_key(down(key::backspace), show).command == command_id::prev);
}

TEST_CASE("Esc walks out and never quits", "[shell][router]") {
  key_router r;
  view_state s = still();
  // Nothing to leave: unhandled, so the window stays open.
  auto none = r.on_key(down(key::escape), s);
  REQUIRE_FALSE(none.handled);
  REQUIRE(none.command == command_id::none);

  s.fullscreen = true;
  s.slideshow = true;
  s.gallery_open = true;
  s.pane_open = true;
  s.crop = true;
  s.focus = focus_kind::filmstrip;
  const back_target order[] = {back_target::crop, back_target::pane, back_target::gallery,
                               back_target::slideshow, back_target::fullscreen,
                               back_target::canvas_focus};
  for (const auto want : order) {
    const auto got = r.on_key(down(key::escape), s);
    REQUIRE(got.handled);
    REQUIRE(got.command == command_id::back);
    REQUIRE(got.back == want);
    switch (want) {
      case back_target::crop: s.crop = false; break;
      case back_target::pane: s.pane_open = false; break;
      case back_target::gallery: s.gallery_open = false; break;
      case back_target::slideshow: s.slideshow = false; break;
      case back_target::fullscreen: s.fullscreen = false; break;
      case back_target::canvas_focus: s.focus = focus_kind::canvas; break;
      default: break;
    }
  }
  REQUIRE_FALSE(r.on_key(down(key::escape), s).handled);

  s.fullscreen = true;
  s.slideshow = true;
  REQUIRE(r.on_key(down(key::escape), s).back == back_target::slideshow);
  // Held Esc does not walk out a second level.
  REQUIRE(r.on_key(rep(key::escape), s).command == command_id::none);
}

TEST_CASE("a focused text control owns every key but Esc", "[shell][router]") {
  key_router r;
  view_state s = still();
  s.focus = focus_kind::text;
  s.fullscreen = true;
  for (const auto k : {key::left, key::space, char_key('D'), char_key('0'), key::f3,
                       key::del, key::enter}) {
    REQUIRE_FALSE(r.on_key(down(k), s).handled);
  }
  REQUIRE_FALSE(r.on_key(down(char_key('O'), mod_ctrl), s).handled);
  const auto esc = r.on_key(down(key::escape), s);
  REQUIRE(esc.handled);
  REQUIRE(esc.back == back_target::blur_text);
}

TEST_CASE("island focus keeps in-pane traversal and still takes global keys", "[shell][router]") {
  key_router r;
  view_state s = still();
  s.focus = focus_kind::filmstrip;
  // The strip's own KeyDown handles these; the router must not also step.
  REQUIRE_FALSE(r.on_key(down(key::left), s).handled);
  REQUIRE_FALSE(r.on_key(down(key::right), s).handled);
  REQUIRE_FALSE(r.on_key(down(key::space), s).handled);
  REQUIRE_FALSE(r.on_key(down(key::enter), s).handled);
  REQUIRE_FALSE(r.on_key(down(key::home), s).handled);
  REQUIRE(r.on_key(down(key::f3), s).command == command_id::overlay);
  // Review note 38: characters are the strip's typeahead, not commands (this
  // used to assert D → next and 0 → fit from the strip).
  REQUIRE(r.on_key(down(char_key('O'), mod_ctrl), s).command == command_id::open);
}

TEST_CASE("typing in the strip or gallery reaches typeahead, not the router", "[shell][router]") {
  key_router r;
  for (const auto focus : {focus_kind::filmstrip, focus_kind::gallery}) {
    view_state s = still();
    s.focus = focus;
    for (const char c : {'D', 'S', 'C', '0', '?', 'G', 'T', 'F', 'A', '1'}) {
      REQUIRE_FALSE(r.on_key(down(char_key(c)), s).handled);
      REQUIRE_FALSE(r.on_key(down(char_key(c), mod_shift), s).handled);
    }
    REQUIRE(r.on_key(down(key::f3), s).command == command_id::overlay);
    REQUIRE(r.on_key(down(char_key('O'), mod_ctrl), s).command == command_id::open);
  }
  // On the canvas the same letters are still commands.
  REQUIRE(r.on_key(down(char_key('D')), still()).command == command_id::next);
  REQUIRE(r.on_key(down(char_key('0')), still()).command == command_id::fit);
}

TEST_CASE("command bar and transport do not swallow A/D/Q/E", "[shell][router]") {
  key_router r;
  view_state s = still();
  s.focus = focus_kind::command_bar;
  REQUIRE(r.on_key(down(char_key('D')), s).command == command_id::next);
  REQUIRE(r.on_key(down(char_key('A')), s).command == command_id::prev);
  REQUIRE(r.on_key(down(char_key('0')), s).command == command_id::fit);
  s = clip();
  s.focus = focus_kind::transport;
  REQUIRE(r.on_key(down(char_key('D')), s).command == command_id::next);
  REQUIRE(r.on_key(down(char_key('E')), s).command == command_id::skim_forward);
  REQUIRE(r.on_key(up(char_key('E')), s).command == command_id::none);
  REQUIRE(r.on_key(down(char_key('E'), mod_shift), s).command == command_id::rate_up);
  REQUIRE(r.on_key(down(key::space), s).command == command_id::play_pause);
  // A `?` flyout is command-bar-classified; Esc still closes it first.
  s.focus = focus_kind::command_bar;
  s.popup_open = true;
  REQUIRE(r.on_key(down(key::escape), s).back == back_target::popup);
  REQUIRE(r.on_key(down(char_key('D')), s).command == command_id::next);
}

TEST_CASE("describe_commands marks hold commands as not runnable", "[shell][commands]") {
  // The fifth column is 0 for a command that needs a key-up (hold Z, hold Q).
  for (const binding& b : default_bindings()) {
    if (b.policy == repeat_policy::momentary) {
      REQUIRE_FALSE(palette_runnable(b.command));
      REQUIRE_FALSE(palette_runnable(b.release));
    } else if (b.policy == repeat_policy::tap_hold) {
      REQUIRE(palette_runnable(b.command));  // the tap is an ordinary command
      if (b.hold != b.command) REQUIRE_FALSE(palette_runnable(b.hold));
      REQUIRE_FALSE(palette_runnable(b.release));
    }
  }
  REQUIRE_FALSE(palette_runnable(command_id::loupe));
  REQUIRE_FALSE(palette_runnable(command_id::hold_previous));
  REQUIRE(palette_runnable(command_id::skim_forward));  // tap skip is runnable
  REQUIRE_FALSE(palette_runnable(command_id::skim_settle));
  REQUIRE(palette_runnable(command_id::rate_up));
  REQUIRE(palette_runnable(command_id::fit));
  const std::string table = describe_commands();
  REQUIRE(table.find("\tLoupe\thold Z\t0\t") != std::string::npos);
  REQUIRE(table.find("\tFit\t0\t1\t") != std::string::npos);
}

TEST_CASE("edge keys ignore typematic repeat; walk keys repeat", "[shell][router]") {
  key_router r;
  const auto s = still();
  REQUIRE(r.on_key(down(key::f3), s).handled);
  REQUIRE_FALSE(r.on_key(rep(key::f3), s).handled);
  REQUIRE_FALSE(r.on_key(rep(char_key('G')), s).handled);
  for (const auto k : {key::left, key::right, char_key('A'), char_key('D'), char_key('+'),
                       char_key('-'), key::space}) {
    const auto got = r.on_key(rep(k), s);
    REQUIRE(got.handled);
    REQUIRE(got.command != command_id::none);
  }
}

TEST_CASE("Q/E: a tap skips, a hold skims and settles on release", "[shell][router]") {
  key_router r;
  const auto s = clip();
  const key q = char_key('Q');
  const key e = char_key('E');

  auto first = r.on_key(down(q), s);
  REQUIRE(first.handled);
  REQUIRE(first.command == command_id::skim_back);  // tap fires on down
  auto tap_up = r.on_key(up(q), s);
  REQUIRE(tap_up.handled);
  REQUIRE(tap_up.command == command_id::none);  // already skipped; no second fire

  REQUIRE(r.on_key(down(e), s).command == command_id::skim_forward);
  REQUIRE(r.on_key(rep(e), s).command == command_id::skim_forward);
  REQUIRE(r.on_key(rep(e), s).command == command_id::skim_forward);
  REQUIRE(r.on_key(up(e), s).command == command_id::skim_settle);

  // A stray key-up with nothing held is not ours.
  REQUIRE_FALSE(r.on_key(up(e), s).handled);

  // Focus arrives mid-hold: the first thing seen is a repeat.
  REQUIRE(r.on_key(rep(q), s).command == command_id::skim_back);
  REQUIRE(r.on_key(up(q), s).command == command_id::skim_settle);

  REQUIRE(r.on_key(down(q), s).command == command_id::skim_back);
  command_id released[key_router::kHeldSlots]{};
  REQUIRE(r.cancel_holds(released) == 0);  // an unfinished tap owes nothing
  REQUIRE_FALSE(r.on_key(up(q), s).handled);
}

TEST_CASE("holds are per key: a second hold cannot swallow the first release", "[shell][router]") {
  key_router r;
  const auto s = clip();
  const key z = char_key('Z');
  const key q = char_key('Q');
  const key e = char_key('E');
  REQUIRE(r.on_key(down(z), s).command == command_id::loupe);
  REQUIRE(r.on_key(down(q), s).command == command_id::skim_back);
  REQUIRE(r.on_key(up(q), s).command == command_id::none);
  REQUIRE(r.on_key(up(z), s).command == command_id::loupe_release);

  // Hold Q, then hold E: both settle.
  REQUIRE(r.on_key(down(q), s).command == command_id::skim_back);
  REQUIRE(r.on_key(rep(q), s).command == command_id::skim_back);
  REQUIRE(r.on_key(down(e), s).command == command_id::skim_forward);
  REQUIRE(r.on_key(rep(e), s).command == command_id::skim_forward);
  REQUIRE(r.on_key(up(q), s).command == command_id::skim_settle);
  REQUIRE(r.on_key(up(e), s).command == command_id::skim_settle);

  // Hold \ then Z.
  const key bs = char_key('\\');
  REQUIRE(r.on_key(down(bs), s).command == command_id::hold_previous);
  REQUIRE(r.on_key(down(z), s).command == command_id::loupe);
  REQUIRE(r.on_key(up(bs), s).command == command_id::hold_previous_release);
  REQUIRE(r.on_key(up(z), s).command == command_id::loupe_release);
}

TEST_CASE("losing activation fires the releases held keys owe", "[shell][router]") {
  key_router r;
  const auto s = clip();
  REQUIRE(r.on_key(down(char_key('Z')), s).command == command_id::loupe);
  REQUIRE(r.on_key(down(char_key('E')), s).command == command_id::skim_forward);
  REQUIRE(r.on_key(rep(char_key('E')), s).command == command_id::skim_forward);
  command_id released[key_router::kHeldSlots]{};
  const auto n = r.cancel_holds(released);
  REQUIRE(n == 2);
  const bool loupe = released[0] == command_id::loupe_release || released[1] == command_id::loupe_release;
  const bool settle = released[0] == command_id::skim_settle || released[1] == command_id::skim_settle;
  REQUIRE(loupe);
  REQUIRE(settle);
  // Nothing held any more: the late key-ups are not ours.
  REQUIRE_FALSE(r.on_key(up(char_key('Z')), s).handled);
  REQUIRE_FALSE(r.on_key(up(char_key('E')), s).handled);
  // A truncated buffer writes what fits and still forgets everything.
  REQUIRE(r.on_key(down(char_key('Z')), s).handled);
  REQUIRE(r.cancel_holds(std::span<command_id>{}) == 0);
  REQUIRE_FALSE(r.on_key(up(char_key('Z')), s).handled);
}

TEST_CASE("pending commands are bound and not yet claimed as landed", "[shell][commands]") {
  std::set<int> bound;
  for (const auto& b : default_bindings()) bound.insert(static_cast<int>(b.command));
  std::set<int> seen;
  for (const auto id : pending_commands()) {
    INFO(find_command(id)->name);
    REQUIRE(find_command(id) != nullptr);
    REQUIRE_FALSE(find_command(id)->keyless);
    REQUIRE(seen.insert(static_cast<int>(id)).second);
  }
  // PR 6's verify commands must not be pending once their slice lands.
  // 6g gate: this list is empty before PR 6 is proposed.
  // PR 6 is complete: every bound command is handled.
  REQUIRE(pending_commands().empty());
}

TEST_CASE("holding Z turns the arrows into loupe nudges", "[shell][router]") {
  key_router r;
  view_state s = still();
  REQUIRE(r.on_key(down(char_key('Z')), s).command == command_id::loupe);
  s.loupe_held = true;  // the app publishes the level; the router reads it
  REQUIRE(resolve_mode(s) == mode::loupe);
  REQUIRE(r.on_key(down(key::left), s).command == command_id::loupe_nudge_left);
  REQUIRE(r.on_key(rep(key::right), s).command == command_id::loupe_nudge_right);
  REQUIRE(r.on_key(rep(key::up), s).command == command_id::loupe_nudge_up);
  REQUIRE(r.on_key(down(key::down), s).command == command_id::loupe_nudge_down);
  // A / D still walk; zoom keys still work; F3 is global.
  REQUIRE(r.on_key(down(char_key('D')), s).command == command_id::next);
  REQUIRE(r.on_key(down(char_key('A')), s).command == command_id::prev);
  REQUIRE(r.on_key(down(char_key('1')), s).command == command_id::one_to_one);
  REQUIRE(r.on_key(down(key::f3), s).command == command_id::overlay);
  // The loupe layers over the mode underneath: the cull keys keep working.
  REQUIRE(r.on_key(down(key::space), s).command == command_id::next);
  REQUIRE(r.on_key(down(key::insert), s).command == command_id::toggle_mark);
  REQUIRE(r.on_key(down(key::space, mod_shift), s).command == command_id::toggle_mark);
  REQUIRE(r.on_key(down(key::del), s).command == command_id::delete_to_recycle_bin);
  REQUIRE(r.on_key(down(key::home), s).command == command_id::first);
  view_state on_clip = clip();
  on_clip.loupe_held = true;
  REQUIRE(r.on_key(down(key::space), on_clip).command == command_id::play_pause);
  REQUIRE(r.on_key(down(char_key('K')), on_clip).command == command_id::pause);
  REQUIRE(r.on_key(down(key::left), on_clip).command == command_id::loupe_nudge_left);
  // Z's own repeat is swallowed and its key-up releases.
  REQUIRE(r.on_key(rep(char_key('Z')), s).handled);
  REQUIRE(r.on_key(up(char_key('Z')), s).command == command_id::loupe_release);
  s.loupe_held = false;
  REQUIRE(r.on_key(down(key::left), s).command == command_id::prev);
  // Island focus wins over a held loupe: the strip keeps its arrows.
  s.loupe_held = true;
  s.focus = focus_kind::filmstrip;
  REQUIRE_FALSE(r.on_key(down(key::left), s).handled);
}

TEST_CASE("key labels read the way the ? sheet shows them", "[shell][commands]") {
  REQUIRE(key_label(char_key('O'), mod_ctrl) == "Ctrl+O");
  REQUIRE(key_label(char_key('O'), mod_ctrl | mod_shift) == "Ctrl+Shift+O");
  REQUIRE(key_label(key::space, mod_shift) == "Shift+Space");
  REQUIRE(key_label(key::f3, mod_none) == "F3");
  REQUIRE(key_label(char_key('?'), mod_none) == "?");
  REQUIRE(key_label(key::page_down, mod_none) == "PageDown");
  REQUIRE(key_label(key::escape, mod_none) == "Esc");
}

TEST_CASE("the command table the chrome gets lists every PR 6 verify binding",
          "[shell][commands]") {
  const std::string table = describe_commands();
  const auto has = [&table](const char* name, const char* keys) {
    const std::string needle = std::string("\t") + name + "\t" + keys + "\t";
    return table.find(needle) != std::string::npos;
  };
  // plan/10 PR 6 verify: open, next/prev, zoom/fit/100 %, mark, copy-to,
  // delete to Recycle Bin, fullscreen, slideshow start/stop, and `?` itself.
  REQUIRE(has("Open media…", "Ctrl+O"));
  REQUIRE(has("Open folder…", "Ctrl+Shift+O"));
  REQUIRE(has("Show in Explorer", "Ctrl+E"));
  REQUIRE(has("Keyboard shortcuts", "?"));
  REQUIRE(has("Next", "Right"));
  REQUIRE(has("Previous", "Left"));
  REQUIRE(has("Next", "Space"));
  REQUIRE(has("Zoom in", "+"));
  REQUIRE(has("Fit", "0"));
  REQUIRE(has("Zoom 100 %", "1"));
  REQUIRE(has("Toggle mark", "Insert"));
  REQUIRE(has("Copy to last folder", "F7"));
  REQUIRE(has("Copy to…", "Shift+F7"));
  REQUIRE(has("Delete to Recycle Bin", "Delete"));
  REQUIRE(has("Fullscreen", "F"));
  REQUIRE(has("Slideshow", "F5"));
  REQUIRE(has("Pause slideshow", "Space"));
  REQUIRE(has("Keyboard shortcuts", "?"));
  REQUIRE(has("Settings", "Ctrl+,"));
  REQUIRE(has("Loupe", "hold Z"));
  REQUIRE(has("Skip forward 2 s", "E"));
  REQUIRE(has("Skip forward 2 s", "hold E"));
  REQUIRE(has("Faster", "Shift+E"));
  // Island-only and release commands are not offered to run.
  REQUIRE(table.find("\tSelect item\t") == std::string::npos);
  REQUIRE(table.find("\tLoupe off\t") == std::string::npos);
  REQUIRE(table.find("\tBack\t") == std::string::npos);
  // Every line is well formed: five tab-separated fields.
  std::size_t lines = 0;
  for (std::size_t pos = 0; pos < table.size();) {
    const std::size_t end = table.find('\n', pos);
    REQUIRE(end != std::string::npos);
    const std::string row = table.substr(pos, end - pos);
    REQUIRE(std::count(row.begin(), row.end(), '\t') == 5);
    pos = end + 1;
    ++lines;
  }
  REQUIRE(lines >= default_bindings().size());
}

TEST_CASE("Esc closes an open popup before anything else", "[shell][router]") {
  key_router r;
  view_state s = still();
  s.popup_open = true;
  s.fullscreen = true;
  s.gallery_open = true;
  s.focus = focus_kind::command_bar;  // the flyout has focus
  const auto esc = r.on_key(down(key::escape), s);
  REQUIRE(esc.handled);
  REQUIRE(esc.back == back_target::popup);
  // A text field inside the popup (go-to / find) blurs, which closes it too.
  s.focus = focus_kind::text;
  REQUIRE(r.on_key(down(key::escape), s).back == back_target::blur_text);
}

TEST_CASE("momentary keys fire on down and release on up", "[shell][router]") {
  key_router r;
  const auto s = still();
  const key z = char_key('Z');
  REQUIRE(r.on_key(down(z), s).command == command_id::loupe);
  const auto held = r.on_key(rep(z), s);
  REQUIRE(held.handled);
  REQUIRE(held.command == command_id::none);
  REQUIRE(r.on_key(up(z), s).command == command_id::loupe_release);
}

TEST_CASE("unbound keys and Alt combinations fall through", "[shell][router]") {
  key_router r;
  const auto s = still();
  REQUIRE_FALSE(r.on_key(down(key::f4, mod_alt), s).handled);  // Alt+F4 reaches DefWindowProc
  REQUIRE_FALSE(r.on_key(down(key::tab), s).handled);
  REQUIRE_FALSE(r.on_key(down(char_key('Y')), s).handled);
  REQUIRE(r.on_key(down(char_key('W'), mod_ctrl), s).command == command_id::close_window);
  REQUIRE(r.on_key(down(char_key('O'), mod_ctrl), s).command == command_id::open);
  REQUIRE(r.on_key(down(char_key('O'), mod_ctrl | mod_shift), s).command == command_id::open_folder);
  REQUIRE(r.on_key(down(char_key('E'), mod_ctrl), s).command == command_id::reveal_in_explorer);
  REQUIRE(r.on_key(down(char_key('?')), s).command == command_id::help);
  REQUIRE(r.on_key(down(char_key(','), mod_ctrl), s).command == command_id::open_settings);
  REQUIRE(r.lookup(key::none, mod_none, mode::browse) == nullptr);
  REQUIRE(r.lookup(key::count, mod_none, mode::browse) == nullptr);
}

TEST_CASE("R is reset-stats except in a slideshow, where it shuffles", "[shell][router]") {
  key_router r;
  view_state s = still();
  REQUIRE(r.on_key(down(char_key('R')), s).command == command_id::reset_stats);
  s.slideshow = true;
  REQUIRE(r.on_key(down(char_key('R')), s).command == command_id::shuffle);
  REQUIRE(r.on_key(down(char_key('+')), s).command == command_id::slideshow_faster);
}

TEST_CASE("symbol keys are characters, independent of the Shift that made them", "[shell][router]") {
  key_router r;
  const auto s = still();
  REQUIRE(char_key('?') != key::none);
  REQUIRE(r.on_key(down(char_key('?')), s).command == command_id::help);
  REQUIRE(r.on_key(down(char_key('=')), s).command == command_id::zoom_in);
  REQUIRE(char_key('d') == char_key('D'));
  REQUIRE(char_key(' ') == key::none);
}

TEST_CASE("`;` plays a Live Photo once: edge only, on a still or over its motion",
          "[shell][router][pairing]") {
  key_router r;
  const key semi = char_key(';');
  REQUIRE(r.on_key(down(semi), still()).command == command_id::play_motion);
  // The motion playing is a clip on screen; `;` again stops it.
  REQUIRE(r.on_key(down(semi), clip()).command == command_id::play_motion);
  // Hold-to-play on a repeating key is forbidden (plan/04).
  REQUIRE_FALSE(r.on_key(rep(semi), still()).handled);
  // Filmstrip focus: `;` is typeahead, not a command.
  view_state strip = still();
  strip.focus = focus_kind::filmstrip;
  REQUIRE_FALSE(r.on_key(down(semi), strip).handled);
  REQUIRE(describe_commands().find("\tPlay Live Photo motion\t;\t") != std::string::npos);
}

TEST_CASE("Esc ends a Live Photo's motion before anything under it", "[shell][router][pairing]") {
  key_router r;
  view_state s = clip();
  s.motion_playing = true;
  s.fullscreen = true;
  s.slideshow = false;
  REQUIRE(r.on_key(down(key::escape), s).back == back_target::motion);
  s.popup_open = true;  // a `?` flyout over it still closes first
  REQUIRE(r.on_key(down(key::escape), s).back == back_target::popup);
}

TEST_CASE("Open RAW / Open JPEG are listed for Settings but not routed or in `?`",
          "[shell][commands][pairing]") {
  struct reset {
    reset() { reset_live_bindings(); }
    ~reset() { reset_live_bindings(); }
  } guard;
  int raw_row = -1;
  const auto def = default_bindings();
  for (int i = 0; i < static_cast<int>(def.size()); ++i) {
    if (def[static_cast<std::size_t>(i)].command == command_id::open_raw) raw_row = i;
  }
  REQUIRE(raw_row >= 0);
  REQUIRE(def[static_cast<std::size_t>(raw_row)].k == key::none);
  const std::string table = describe_commands();
  REQUIRE(table.find("\tOpen RAW of pair\t\t") != std::string::npos);
  REQUIRE(table.find("\tOpen JPEG of pair\t\t") != std::string::npos);
  // Settings gives it a free key; the router then routes it.
  REQUIRE(rebind_live(raw_row, char_key('R'), mod_ctrl));
  key_router r;
  r.rebuild(live_bindings());
  REQUIRE(r.on_key(down(char_key('R'), mod_ctrl), still()).command == command_id::open_raw);
}

TEST_CASE("remapping a key updates the live table the router and ? share", "[shell][commands]") {
  struct reset {
    reset() { reset_live_bindings(); }
    ~reset() { reset_live_bindings(); }
  } guard;
  int row_a = -1;
  int row_d = -1;
  const auto def = default_bindings();
  for (int i = 0; i < static_cast<int>(def.size()); ++i) {
    if (def[static_cast<std::size_t>(i)].k == char_key('A') && def[static_cast<std::size_t>(i)].mods == mod_none)
      row_a = i;
    if (def[static_cast<std::size_t>(i)].k == char_key('D') && def[static_cast<std::size_t>(i)].mods == mod_none)
      row_d = i;
  }
  REQUIRE(row_a >= 0);
  REQUIRE(row_d >= 0);
  REQUIRE(rebind_live(row_d, char_key('A'), mod_none));  // swap with A
  REQUIRE(live_bindings()[static_cast<std::size_t>(row_d)].k == char_key('A'));
  REQUIRE(live_bindings()[static_cast<std::size_t>(row_a)].k == char_key('D'));
  key_router r;
  r.rebuild(live_bindings());
  REQUIRE(r.on_key(down(char_key('A')), still()).command == command_id::next);
  REQUIRE(r.on_key(down(char_key('D')), still()).command == command_id::prev);
  const std::string table = describe_commands();
  REQUIRE(table.find("\tNext\tA\t") != std::string::npos);
  reset_live_bindings();
  r.rebuild(live_bindings());
  REQUIRE(r.on_key(down(char_key('A')), still()).command == command_id::prev);
}

TEST_CASE("Esc leaves the empty-window runner, before it moves focus", "[keys][dino]") {
  view_state s;  // empty canvas, runner up
  s.game = true;
  CHECK(resolve_back(s) == back_target::game);
  const auto r = key_router().on_key(down(key::escape), s);
  CHECK(r.handled);
  CHECK(r.command == command_id::back);
  CHECK(r.back == back_target::game);

  // Focus in a strip does not eat the press; the runner still goes first.
  s.focus = focus_kind::filmstrip;
  CHECK(resolve_back(s) == back_target::game);

  // Overlays above the canvas close first, as in plan/16 "Esc walks out".
  s.popup_open = true;
  CHECK(resolve_back(s) == back_target::popup);
  s.popup_open = false;
  s.fullscreen = true;
  CHECK(resolve_back(s) == back_target::fullscreen);

  // No runner, nothing to leave: still not handled, never a quit.
  view_state idle;
  CHECK(resolve_back(idle) == back_target::none);
  CHECK_FALSE(key_router().on_key(down(key::escape), idle).handled);
}
