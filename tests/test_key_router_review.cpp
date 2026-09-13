// SPDX-License-Identifier: GPL-2.0-or-later
// Reviewer tests for the PR 6 key router: the cases a table-driven router gets
// wrong when two keys are held at once, and an exhaustive sweep that proves no
// (key, mods, mode, edge) input can produce an out-of-range or half-bound route.
#include <catch2/catch_test_macros.hpp>

#include "shell/commands.h"
#include "shell/key_router.h"

using namespace mv::shell;

namespace {

key_event down(key k, std::uint8_t mods = mod_none) { return {k, mods, false, false}; }
key_event rep(key k, std::uint8_t mods = mod_none) { return {k, mods, true, false}; }
key_event up(key k) { return {k, mod_none, false, true}; }

view_state with(item_kind item) {
  view_state s;
  s.item = item;
  return s;
}

}  // namespace

TEST_CASE("table rows are well-formed", "[shell][commands][review]") {
  const auto rows = default_bindings();
  REQUIRE(rows.size() < 255);  // the router's index is uint8 row + 1
  for (const auto& b : rows) {
    INFO("key " << static_cast<int>(b.k) << " command " << static_cast<int>(b.command));
    REQUIRE(static_cast<int>(b.k) > 0);
    REQUIRE(static_cast<int>(b.k) < kKeyCount);
    REQUIRE(b.mods < kModCombos);
    REQUIRE((b.modes & ~kAllModes) == 0);
    REQUIRE(static_cast<int>(b.command) < kCommandCount);
    REQUIRE(static_cast<int>(b.hold) < kCommandCount);
    REQUIRE(static_cast<int>(b.release) < kCommandCount);
    // Holds need their key-up. A focused island eats key-ups the router never
    // sees, so a hold row that is live in island mode can stick.
    if (b.policy == repeat_policy::tap_hold || b.policy == repeat_policy::momentary) {
      REQUIRE((b.modes & kIsland) == 0);
    }
    if (b.policy != repeat_policy::tap_hold) REQUIRE(b.hold == command_id::none);
    if (b.policy == repeat_policy::edge || b.policy == repeat_policy::repeat) {
      REQUIRE(b.release == command_id::none);
    }
    // A key the router intercepts before lookup must not also carry a second
    // meaning in the table, or `?` lists a binding that never runs.
    if (b.k == key::escape) REQUIRE(b.command == command_id::back);
  }
}

TEST_CASE("exhaustive sweep: every input routes to nothing or a real command",
          "[shell][router][review]") {
  const item_kind items[] = {item_kind::none, item_kind::still, item_kind::clip,
                             item_kind::animation};
  const focus_kind focuses[] = {focus_kind::canvas, focus_kind::command_bar,
                                focus_kind::filmstrip, focus_kind::gallery,
                                focus_kind::transport, focus_kind::text};
  std::size_t routed = 0;
  for (const auto item : items) {
    for (const auto focus : focuses) {
      for (int flags = 0; flags < 4; ++flags) {
        view_state s = with(item);
        s.focus = focus;
        s.slideshow = (flags & 1) != 0;
        s.gallery_open = (flags & 2) != 0;
        for (int k = 1; k < kKeyCount; ++k) {
          for (int mods = 0; mods < kModCombos; ++mods) {
            key_router r;  // fresh: no hold leaks between cells
            const auto kk = static_cast<key>(k);
            const auto m8 = static_cast<std::uint8_t>(mods);
            for (const auto& e : {down(kk, m8), rep(kk, m8), up(kk)}) {
              const auto got = r.on_key(e, s);
              REQUIRE(static_cast<int>(got.command) < kCommandCount);
              if (!got.handled) REQUIRE(got.command == command_id::none);
              if (got.command == command_id::back) {
                REQUIRE(got.back != back_target::none);
              } else {
                REQUIRE(got.back == back_target::none);
              }
              if (focus == focus_kind::text && kk != key::escape) {
                REQUIRE_FALSE(got.handled);
              }
              if (got.handled) ++routed;
            }
            // Whatever that left held, a release of an unrelated key is not ours.
            const key other = kk == key::f12 ? key::f11 : key::f12;
            REQUIRE_FALSE(r.on_key(up(other), s).handled);
          }
        }
      }
    }
  }
  REQUIRE(routed > 0);
}

TEST_CASE("a second hold does not swallow the first hold's release", "[shell][router][review]") {
  key_router r;
  const auto s = with(item_kind::clip);
  const key z = char_key('Z');
  const key q = char_key('Q');

  REQUIRE(r.on_key(down(z), s).command == command_id::loupe);
  REQUIRE(r.on_key(down(q), s).command == command_id::skim_back);
  REQUIRE(r.on_key(up(q), s).command == command_id::none);
  // The loupe was never let go of; its key-up must still turn it off.
  REQUIRE(r.on_key(up(z), s).command == command_id::loupe_release);
}

TEST_CASE("overlapping Q and E holds each settle", "[shell][router][review]") {
  key_router r;
  const auto s = with(item_kind::clip);
  const key q = char_key('Q');
  const key e = char_key('E');

  REQUIRE(r.on_key(down(q), s).command == command_id::skim_back);
  REQUIRE(r.on_key(rep(q), s).command == command_id::skim_back);
  REQUIRE(r.on_key(down(e), s).command == command_id::skim_forward);
  const auto q_up = r.on_key(up(q), s);
  // Letting go of Q after a hold is a settle, even with E now down.
  REQUIRE(q_up.command == command_id::skim_settle);
  REQUIRE(r.on_key(up(e), s).command == command_id::none);  // E was a tap (no repeat)
}

TEST_CASE("momentary held across a mode change still releases", "[shell][router][review]") {
  key_router r;
  const key z = char_key('Z');
  REQUIRE(r.on_key(down(z), with(item_kind::still)).command == command_id::loupe);
  // Navigation lands on a clip while Z is held (key-repeat of A, or the
  // slideshow timer). The release belongs to the row that started the hold.
  REQUIRE(r.on_key(up(z), with(item_kind::clip)).command == command_id::loupe_release);
  view_state show = with(item_kind::still);
  REQUIRE(r.on_key(down(z), show).command == command_id::loupe);
  show.slideshow = true;
  REQUIRE(r.on_key(up(z), show).command == command_id::loupe_release);
}

TEST_CASE("walk keys still repeat while a momentary key is held", "[shell][router][review]") {
  key_router r;
  const auto s = with(item_kind::still);
  const key bs = char_key('\\');
  REQUIRE(r.on_key(down(bs), s).command == command_id::hold_previous);
  REQUIRE(r.on_key(rep(key::right), s).command == command_id::next);
  REQUIRE(r.on_key(rep(bs), s).handled);
  REQUIRE(r.on_key(up(bs), s).command == command_id::hold_previous_release);
}

TEST_CASE("modifier-only and unknown keys never route", "[shell][router][review]") {
  key_router r;
  const auto s = with(item_kind::still);
  REQUIRE_FALSE(r.on_key(down(key::none), s).handled);
  REQUIRE_FALSE(r.on_key(down(key::count), s).handled);
  REQUIRE_FALSE(r.on_key(down(static_cast<key>(0x7F)), s).handled);
  REQUIRE_FALSE(r.on_key(down(char_key('D'), mod_alt), s).handled);  // Alt+letter is the menu
  REQUIRE_FALSE(r.on_key(down(key::f10), s).handled);                // F10 is DefWindowProc's
}
