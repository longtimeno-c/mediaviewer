// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The one key router (docs/design/16-commands.md). Runs on the UI thread, before the
// island's pre-translate. Pure: no Win32, no allocation, no I/O on keydown.
#pragma once

#include <array>
#include <cstdint>

#include "shell/commands.h"

namespace mv::shell {

enum class focus_kind : std::uint8_t {
  canvas = 0,
  command_bar = 1,
  filmstrip = 2,
  gallery = 3,
  transport = 4,
  text = 5,  // a XAML text control: keys belong to it; Esc blurs back
  // PR 9: the metadata pane or the folder tree holds focus (docs/design/16 "Pane" mode).
  // In-pane traversal belongs to XAML; Esc walks back out to the canvas.
  pane = 6,
};

enum class item_kind : std::uint8_t { none, still, clip, animation };

// Read from app state at every keydown. Nothing here is remembered between
// keys, so a slideshow that advances from a clip to a still cannot leave Space
// bound to the clip.
struct view_state {
  focus_kind focus = focus_kind::canvas;
  item_kind item = item_kind::none;
  bool gallery_open = false;
  bool slideshow = false;
  bool fullscreen = false;
  bool loupe_held = false;  // Z is down: arrows nudge the loupe
  bool popup_open = false;  // `?`, go-to or find flyout is up
  bool settings_open = false;  // the settings screen covers the canvas
  bool motion_playing = false;  // `;` is playing a Live Photo's motion (PR 7)
  bool pane_open = false;  // PR 8+
  bool crop = false;       // PR 10: crop mode on the canvas
  bool trim = false;       // PR 13: trim armed on the current clip
  bool game = false;       // the empty-window runner is up (Space on an empty view)
  // Milestone H: a search result list ("Search: ...") is open in place of a
  // folder and there is somewhere to go back to (docs/design/16 `Esc`).
  bool list_open = false;
};

[[nodiscard]] mode resolve_mode(const view_state& s) noexcept;

// docs/design/16: Esc walks out — crop, pane, fullscreen / slideshow, canvas. It never
// quits: `none` means there is nothing left to leave.
enum class back_target : std::uint8_t {
  none,
  blur_text,
  popup,
  motion,  // stop a Live Photo's motion and return to the still
  crop,
  trim,  // PR 13: disarm trim (markers kept for the clip until it changes)
  pane,
  gallery,
  settings,
  slideshow,
  fullscreen,
  canvas_focus,
  game,  // leave the empty-window runner (dino_game.h)
  result_list,  // Milestone H: back from a result list to its folder ("Back to folder")
};

[[nodiscard]] back_target resolve_back(const view_state& s) noexcept;

struct key_event {
  key k = key::none;
  std::uint8_t mods = mod_none;
  bool repeat = false;  // typematic
  bool up = false;
};

// The standard editing chords a text field owns (owner, 2026-09-27: "Cmd+A /
// Ctrl+A in the search bar"). With a text control focused the router already
// leaves every key but Esc to it; this names the chords a host must make sure
// the field actually gets. The Mac field editor gets them only through Edit
// menu items the viewer does not have (⌘A, ⌘C and ⌘Z are its own commands on
// the canvas), so the host hands them over; the Windows FakeInput reads them
// itself (and Ctrl+Y, Windows' other redo). Ctrl here is the host's primary
// modifier (⌘ on the Mac).
enum class text_edit : std::uint8_t { none, select_all, copy, cut, paste, undo, redo };

[[nodiscard]] text_edit text_edit_for(const key_event& e) noexcept;

struct route {
  command_id command = command_id::none;
  back_target back = back_target::none;  // set when command == back
  // True when the router owns the key even if there is nothing to dispatch
  // (a tap/hold's key-up after a tap, a momentary key's typematic repeat).
  // False sends the key on to the island.
  bool handled = false;
};

class key_router {
 public:
  key_router() noexcept;

  // Rebuild the O(1) index from `rows` (the live table after a remap).
  void rebuild(std::span<const binding> rows) noexcept;

  [[nodiscard]] route on_key(const key_event& e, const view_state& s) noexcept;

  // Row for (key, mods, mode), or null. O(1): an index built once.
  [[nodiscard]] const binding* lookup(key k, std::uint8_t mods, mode m) const noexcept;

  // The window lost activation, so no key-up is coming. Forgets every held
  // key and writes the releases they owe (loupe off, skim settle) into
  // `released`, for the caller to dispatch. Returns how many were written.
  [[nodiscard]] std::size_t cancel_holds(std::span<command_id> released) noexcept;

  static constexpr int kHeldSlots = 4;

 private:
  struct held {
    const binding* row = nullptr;
    key k = key::none;
    bool repeated = false;
  };

  [[nodiscard]] held* find_held(key k) noexcept;
  [[nodiscard]] held* claim_held(key k) noexcept;

  static constexpr int kSlots = kKeyCount * kModCombos * kModeCount;
  std::array<std::uint8_t, kSlots> index_{};  // 0 = unbound, else row + 1
  const binding* rows_ = nullptr;
  std::size_t row_count_ = 0;
  // Holds are per key, so holding Z and then tapping Q cannot lose Z's release.
  std::array<held, kHeldSlots> held_{};
};

}  // namespace mv::shell
