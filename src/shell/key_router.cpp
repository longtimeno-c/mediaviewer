// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/key_router.h"

#include <span>

namespace mv::shell {
namespace {

constexpr int slot(key k, std::uint8_t mods, mode m) noexcept {
  return (static_cast<int>(k) * kModCombos + (mods & (kModCombos - 1))) * kModeCount +
         static_cast<int>(m);
}

}  // namespace

mode resolve_mode(const view_state& s) noexcept {
  if (s.gallery_open && !s.popup_open) return mode::gallery;
  // Island mode is in-pane traversal and typeahead (plan/16, plan/12 2026-09-13):
  // the filmstrip and the gallery. The command bar, the transport, and a `?`
  // flyout's popup HWND are not a mode — A/D still walk the folder, Q/E still
  // skip a clip. Treating them as island was why those keys died until the
  // window was deactivated and the canvas HWND took focus again.
  if (s.focus == focus_kind::filmstrip || s.focus == focus_kind::gallery) {
    return mode::island;
  }
  // Crop is a canvas mode (plan/16): it only exists on a still, and the
  // host leaves it when the item changes.
  if (s.crop && s.item == item_kind::still) return mode::crop;
  if (s.game && s.item == item_kind::none && !s.popup_open) return mode::runner;
  if (s.loupe_held) return mode::loupe;
  if (s.slideshow) return mode::slideshow;
  if (s.item == item_kind::clip || s.item == item_kind::animation) return mode::video;
  return mode::browse;
}

back_target resolve_back(const view_state& s) noexcept {
  if (s.focus == focus_kind::text) return back_target::blur_text;
  // A `?` / go-to / find flyout closes before anything under it.
  if (s.popup_open) return back_target::popup;
  if (s.settings_open) return back_target::settings;
  // A Live Photo's motion is a moment on top of the still, not a mode: Esc
  // ends it before it touches gallery, slideshow or fullscreen.
  if (s.motion_playing) return back_target::motion;
  // 6f: an open `?` / go-to / find flyout must close before anything below. It
  // needs its own view_state bit; today a flyout closes on focus loss, which
  // is what canvas_focus causes.
  if (s.crop) return back_target::crop;
  if (s.pane_open) return back_target::pane;
  // The gallery covers the canvas like an overlay, so it goes before the
  // window-level states (plan/16 "Esc walks out").
  if (s.gallery_open) return back_target::gallery;
  if (s.slideshow) return back_target::slideshow;
  if (s.fullscreen) return back_target::fullscreen;
  // The runner sits on an empty canvas, under every overlay above; Esc leaves
  // it before it moves focus, so one press is enough wherever focus is.
  if (s.game) return back_target::game;
  if (s.focus != focus_kind::canvas) return back_target::canvas_focus;
  return back_target::none;
}

key_router::key_router() noexcept { rebuild(live_bindings()); }

void key_router::rebuild(std::span<const binding> rows) noexcept {
  index_.fill(0);
  rows_ = rows.data();
  row_count_ = rows.size();
  held_ = {};
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const binding& b = rows[i];
    if (static_cast<int>(b.k) <= 0 || static_cast<int>(b.k) >= kKeyCount) continue;
    for (int m = 0; m < kModeCount; ++m) {
      if ((b.modes & (1 << m)) == 0) continue;
      auto& cell = index_[static_cast<std::size_t>(slot(b.k, b.mods, static_cast<mode>(m)))];
      // First row wins; duplicates are a table bug the tests reject. The table
      // size is static_asserted below 255 in command_table.cpp.
      if (cell == 0) cell = static_cast<std::uint8_t>(i + 1);
    }
  }
}

const binding* key_router::lookup(key k, std::uint8_t mods, mode m) const noexcept {
  if (static_cast<int>(k) <= 0 || static_cast<int>(k) >= kKeyCount) return nullptr;
  if (mods >= kModCombos || m >= mode::count) return nullptr;
  if (!rows_ || row_count_ == 0) return nullptr;
  const auto cell = index_[static_cast<std::size_t>(slot(k, mods, m))];
  if (cell == 0 || cell > row_count_) return nullptr;
  return &rows_[cell - 1u];
}

key_router::held* key_router::find_held(key k) noexcept {
  for (auto& h : held_) {
    if (h.row && h.k == k) return &h;
  }
  return nullptr;
}

key_router::held* key_router::claim_held(key k) noexcept {
  if (auto* existing = find_held(k)) return existing;
  for (auto& h : held_) {
    if (!h.row) return &h;
  }
  return nullptr;
}

std::size_t key_router::cancel_holds(std::span<command_id> released) noexcept {
  std::size_t n = 0;
  for (auto& h : held_) {
    if (!h.row) continue;
    // A momentary key always owes its release. A tap/hold owes one only once
    // it became a hold; an unfinished tap is dropped rather than fired.
    const bool owed = h.row->policy == repeat_policy::momentary || h.repeated;
    if (owed && n < released.size()) released[n++] = h.row->release;
    h = held{};
  }
  return n;
}

route key_router::on_key(const key_event& e, const view_state& s) noexcept {
  if (e.up) {
    held* h = find_held(e.k);
    if (!h) return {};
    const binding* b = h->row;
    const bool repeated = h->repeated;
    *h = held{};
    if (b->policy == repeat_policy::momentary) return {b->release, back_target::none, true};
    // Tap already ran on down. A hold owes its settle; a tap does not.
    return {repeated ? b->release : command_id::none, back_target::none, true};
  }

  // Settings owns keyboard input all the way through XAML dispatch. Its
  // PreviewKeyDown captures bindings; Esc cancels capture before closing it.
  // ContentPreTranslateMessage returning false does not mean XAML declined it.
  if (s.settings_open) return {};

  // A focused pane owns its keys (arrows walk it, Enter opens); Esc returns to the
  // canvas (plan/16 "Pane": in-pane traversal, Esc returns).
  if (s.focus == focus_kind::pane) {
    if (e.k == key::escape && e.mods == mod_none && !e.repeat) {
      return {command_id::back, back_target::canvas_focus, true};
    }
    return {};
  }

  // A text control owns every key except the one that leaves it.
  if (s.focus == focus_kind::text) {
    if (e.k == key::escape && e.mods == mod_none && !e.repeat) {
      return {command_id::back, back_target::blur_text, true};
    }
    return {};
  }

  if (e.k == key::escape && e.mods == mod_none) {
    // A held Esc must not walk out of three levels at once.
    if (e.repeat) return {};
    const back_target target = resolve_back(s);
    // Nothing to leave: not handled, and certainly not a quit. The key goes
    // on to whatever is focused (a flyout closes itself on Esc).
    if (target == back_target::none) return {};
    return {command_id::back, target, true};
  }

  const mode m = resolve_mode(s);
  const binding* b = lookup(e.k, e.mods, m);
  // Gallery navigation is available immediately after G, regardless of which
  // HWND still has focus. Other letters remain available for typeahead.
  if (m == mode::gallery && (e.mods & (mod_ctrl | mod_alt)) == 0 &&
      static_cast<std::uint16_t>(e.k) >= 0x21 &&
      static_cast<std::uint16_t>(e.k) <= 0x7E) {
    // Check the resolved command, not its default key: Settings may remap it.
    if (!b) return {};
    switch (b->command) {
      case command_id::gallery_up:
      case command_id::gallery_down:
      case command_id::gallery_open_selected:
      case command_id::gallery_larger:
      case command_id::gallery_smaller:
      case command_id::prev:
      case command_id::next:
      case command_id::toggle_gallery:
      case command_id::fullscreen:
      case command_id::help:
        break;
      default: return {};
    }
  }
  // Review note 38 / plan/12 2026-09-13: with the strip or the gallery focused,
  // a character typed without Ctrl or Alt is typeahead, not a command — so
  // letter bindings added later cannot eat it either. Named keys (F3, arrows)
  // and chords (Ctrl+O) still route. Command-bar and transport focus is not
  // this case; those keys belong to the mode underneath.
  if (const auto raw = static_cast<std::uint16_t>(e.k);
      m == mode::island && (e.mods & (mod_ctrl | mod_alt)) == 0 && raw >= 0x21 && raw <= 0x7E) {
    return {};
  }
  if (!b && m == mode::loupe) {
    // The loupe layers over the mode underneath: only the arrows (and Z / \)
    // mean something else while Z is held. Space, marks, Delete and transport
    // keep working mid-cull.
    view_state under = s;
    under.loupe_held = false;
    b = lookup(e.k, e.mods, resolve_mode(under));
  }
  if (!b) return {};

  switch (b->policy) {
    case repeat_policy::edge:
      if (e.repeat) return {};
      return {b->command, back_target::none, true};
    case repeat_policy::repeat:
      return {b->command, back_target::none, true};
    case repeat_policy::momentary: {
      if (e.repeat && find_held(e.k)) return {command_id::none, back_target::none, true};
      held* h = claim_held(e.k);
      if (!h) return {};  // four keys already held; a fifth hold is not tracked
      *h = held{b, e.k, false};
      return {b->command, back_target::none, true};
    }
    case repeat_policy::tap_hold: {
      held* h = find_held(e.k);
      if (h && h->row == b && e.repeat) {
        h->repeated = true;
        return {b->hold, back_target::none, true};
      }
      h = claim_held(e.k);
      if (!h) return {};
      // Tap fires on the down edge so Q/E skip without waiting for key-up.
      // The first typematic repeat makes it a hold; a repeat with no recorded
      // down (focus arrived mid-hold) starts the hold from here.
      *h = held{b, e.k, e.repeat};
      if (!e.repeat) return {b->command, back_target::none, true};
      return {b->hold, back_target::none, true};
    }
  }
  return {};
}

}  // namespace mv::shell
