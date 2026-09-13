// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/key_router.h"

namespace mv::shell {
namespace {

constexpr int slot(key k, std::uint8_t mods, mode m) noexcept {
  return (static_cast<int>(k) * kModCombos + (mods & (kModCombos - 1))) * kModeCount +
         static_cast<int>(m);
}

}  // namespace

mode resolve_mode(const view_state& s) noexcept {
  if (s.focus != focus_kind::canvas) return mode::island;
  if (s.slideshow) return mode::slideshow;
  if (s.item == item_kind::clip || s.item == item_kind::animation) return mode::video;
  return mode::browse;
}

back_target resolve_back(const view_state& s) noexcept {
  if (s.focus == focus_kind::text) return back_target::blur_text;
  // 6f: an open `?` / palette flyout must close before anything below. It
  // needs its own view_state bit; today a flyout closes on focus loss, which
  // is what canvas_focus causes.
  if (s.crop) return back_target::crop;
  if (s.pane_open) return back_target::pane;
  // The gallery covers the canvas like an overlay, so it goes before the
  // window-level states (plan/16 "Esc walks out").
  if (s.gallery_open) return back_target::gallery;
  if (s.slideshow) return back_target::slideshow;
  if (s.fullscreen) return back_target::fullscreen;
  if (s.focus != focus_kind::canvas) return back_target::canvas_focus;
  return back_target::none;
}

key_router::key_router() noexcept {
  const auto rows = default_bindings();
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
  const auto cell = index_[static_cast<std::size_t>(slot(k, mods, m))];
  return cell == 0 ? nullptr : &default_bindings()[cell - 1u];
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
    return {repeated ? b->release : b->command, back_target::none, true};
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

  const binding* b = lookup(e.k, e.mods, resolve_mode(s));
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
      // The down edge of a tap does nothing: it is not yet known to be one.
      // A repeat with no recorded down (focus arrived mid-hold) starts the
      // hold from here.
      *h = held{b, e.k, e.repeat};
      if (!e.repeat) return {command_id::none, back_target::none, true};
      return {b->hold, back_target::none, true};
    }
  }
  return {};
}

}  // namespace mv::shell
