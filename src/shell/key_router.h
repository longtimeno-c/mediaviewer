// SPDX-License-Identifier: GPL-2.0-or-later
// The one key router (plan/16-commands.md). Runs on the UI thread, before the
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
  bool pane_open = false;  // PR 8+
  bool crop = false;       // PR 9
};

[[nodiscard]] mode resolve_mode(const view_state& s) noexcept;

// plan/16: Esc walks out — crop, pane, fullscreen / slideshow, canvas. It never
// quits: `none` means there is nothing left to leave.
enum class back_target : std::uint8_t {
  none,
  blur_text,
  crop,
  pane,
  gallery,
  slideshow,
  fullscreen,
  canvas_focus,
};

[[nodiscard]] back_target resolve_back(const view_state& s) noexcept;

struct key_event {
  key k = key::none;
  std::uint8_t mods = mod_none;
  bool repeat = false;  // typematic
  bool up = false;
};

struct route {
  command_id command = command_id::none;
  back_target back = back_target::none;  // set when command == back
  // True when the router owns the key even if there is nothing to dispatch
  // (the down edge of a tap/hold). False sends the key on to the island.
  bool handled = false;
};

class key_router {
 public:
  key_router() noexcept;

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
  // Holds are per key, so holding Z and then tapping Q cannot lose Z's release.
  std::array<held, kHeldSlots> held_{};
};

}  // namespace mv::shell
