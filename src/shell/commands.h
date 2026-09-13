// SPDX-License-Identifier: GPL-2.0-or-later
// The command table: host keys, modes, command ids, and the default map.
//
// plan/16-commands.md: one router, one table. Dispatch is an array index, not a
// string lookup; the palette and `?` filter this same static table in memory.
//
// Nothing here includes <windows.h>. Win32 virtual keys are translated to
// `key` at the edge in main.cpp, so the table and the router are unit-tested
// without a window, and the bindings stay in the host (D9) — nothing below
// shell/ ever sees a key.
#pragma once

#include <cstdint>
#include <span>

namespace mv::shell {

// Host key codes. 0x21..0x7E are the character the key produced (letters are
// upper-case, symbols are resolved through the layout at the edge, so `?` is
// the key '?' with no Shift). Named keys live above 0xFF.
enum class key : std::uint16_t {
  none = 0,
  space = 0x100,
  backspace,
  enter,
  escape,
  tab,
  insert,
  del,
  home,
  end,
  page_up,
  page_down,
  left,
  right,
  up,
  down,
  f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12,
  count
};

inline constexpr int kKeyCount = static_cast<int>(key::count);

[[nodiscard]] constexpr key char_key(char c) noexcept {
  if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  if (c < 0x21 || c > 0x7E) return key::none;
  return static_cast<key>(static_cast<std::uint16_t>(c));
}

enum mod : std::uint8_t {
  mod_none = 0,
  mod_ctrl = 1 << 0,
  mod_shift = 1 << 1,
  mod_alt = 1 << 2,
};

inline constexpr int kModCombos = 8;

// plan/16 "Modes". Derived from state at keydown (key_router.h), never kept on
// a stack that can go stale when navigation lands somewhere else. Crop and the
// panes arrive with their PRs; a focused island is `island`, where in-pane
// traversal keys fall through to XAML.
enum class mode : std::uint8_t {
  browse = 0,
  video = 1,      // current item is a clip or an animation
  slideshow = 2,
  island = 3,     // filmstrip, gallery, command bar or transport has focus
  loupe = 4,      // Z held on the canvas: arrows nudge the loupe point
  count
};

inline constexpr int kModeCount = static_cast<int>(mode::count);

using mode_mask = std::uint8_t;
inline constexpr mode_mask kBrowse = 1 << 0;
inline constexpr mode_mask kVideo = 1 << 1;
inline constexpr mode_mask kSlideshow = 1 << 2;
inline constexpr mode_mask kIsland = 1 << 3;
inline constexpr mode_mask kLoupe = 1 << 4;
inline constexpr mode_mask kAllModes = kBrowse | kVideo | kSlideshow | kIsland | kLoupe;

[[nodiscard]] constexpr mode_mask mask_of(mode m) noexcept {
  return static_cast<mode_mask>(1u << static_cast<unsigned>(m));
}

// What typematic repeat and key-up mean for a binding.
enum class repeat_policy : std::uint8_t {
  edge,       // down edge only; repeats fall through unhandled
  repeat,     // every down, including typematic repeat
  tap_hold,   // plan/16: down does nothing; first repeat makes it a hold
              // (`hold` per repeat, `release` on key-up); no repeat = `command`
  momentary,  // `command` on down, `release` on up (hold Z, hold `\`)
};

// One id space with the island's chrome_command (chrome_host.h). 1–19 predate
// the table and keep their values; 15, 16, 18, 19 and 20 are island → native
// notifications, not commands, and stay reserved here so the wire never
// renumbers. static_asserts in chrome_host.h pin the shared values.
enum class command_id : std::uint16_t {
  none = 0,
  open = 1,
  fit = 2,
  one_to_one = 3,
  zoom_in = 4,
  zoom_out = 5,
  zoom_preset = 6,       // island only, arg = factor
  overlay = 7,           // F3 frame-time overlay
  select_item = 8,       // island only, arg = index
  prev = 9,
  next = 10,
  open_folder = 11,
  toggle_gallery = 12,
  close_gallery = 13,    // island only; Esc reaches it through `back`
  gallery_activate = 14, // island only, arg = index
  // 15 set_settings, 16 folder_ready: notifications
  toggle_filmstrip = 17,
  // 18 video_active, 19 set_rate, 20 focus_changed: notifications
  back = 21,             // Esc walks out (plan/16)
  first,
  last,
  skip_back,
  skip_forward,
  play_pause,
  jump_back,
  pause,
  jump_forward,
  frame_back,
  frame_forward,
  rate_down,
  rate_up,
  skim_back,
  skim_forward,
  skim_settle,
  reset_stats,
  close_window,
  zoom_200,
  zoom_400,
  // 6b
  fullscreen,
  fill,
  reset_view,
  cycle_background,
  sticky_zoom,
  clipping,
  loupe,
  loupe_release,
  hold_previous,
  hold_previous_release,
  always_on_top,
  info_overlay,
  pan_up,     // ↑ when zoomed, Shift+↑
  pan_down,   // ↓ when zoomed, Shift+↓
  pan_left,   // Shift+←
  pan_right,  // Shift+→
  loupe_nudge_left,   // arrows while Z is held (plan/16 "keyboard-nudgeable")
  loupe_nudge_right,
  loupe_nudge_up,
  loupe_nudge_down,
  // 6c
  toggle_mark,
  mark_all,
  unmark_all,
  copy_to,
  copy_to_pick,
  move_to,
  move_to_pick,
  delete_to_recycle_bin,
  // 6d
  slideshow_start,
  slideshow_pause,
  slideshow_faster,
  slideshow_slower,
  blackout,
  shuffle,
  // 6f
  help,
  palette,
  go_to,
  folder_tree,
  count
};

inline constexpr int kCommandCount = static_cast<int>(command_id::count);

[[nodiscard]] constexpr bool is_reserved_notification(int id) noexcept {
  return id == 15 || id == 16 || id == 18 || id == 19 || id == 20;
}

struct binding {
  key k = key::none;
  std::uint8_t mods = mod_none;
  mode_mask modes = 0;
  repeat_policy policy = repeat_policy::edge;
  command_id command = command_id::none;
  command_id hold = command_id::none;     // tap_hold only
  command_id release = command_id::none;  // tap_hold and momentary
};

struct command_info {
  command_id id = command_id::none;
  const char* name = "";  // palette label; unique
  // Island-only commands carry an argument (an index, a zoom factor) and have
  // no key by design. Everything else must be bound.
  bool keyless = false;
};

[[nodiscard]] std::span<const binding> default_bindings() noexcept;
[[nodiscard]] std::span<const command_info> command_infos() noexcept;
[[nodiscard]] const command_info* find_command(command_id id) noexcept;
// Bound commands whose effect has not landed yet. Empty before PR 6 ships.
[[nodiscard]] std::span<const command_id> pending_commands() noexcept;

}  // namespace mv::shell
