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
#include <string>

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
  // PR 12 (plan/16 Rate): the numeric keypad's digits, apart from the number
  // row, which stays zoom. Appended so every value above keeps its number.
  numpad0, numpad1, numpad2, numpad3, numpad4, numpad5, numpad6, numpad7, numpad8, numpad9,
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

// A symbol from the active layout. `base` is the key with Shift not applied,
// `ignored` is the platform's "modifiers ignored" character (on macOS that
// string still includes Shift, so Shift+/ is '?' there too), and `produced`
// is the glyph the event actually inserted. Letters are the caller's job:
// they keep Shift (Shift+O). A digit keeps `base` and Shift, matching the
// Windows virtual key. Any other ASCII glyph is the shifted character with
// Shift cleared, because the table binds `?` and `+` with no Shift.
[[nodiscard]] inline key resolve_layout_symbol(char32_t base, char32_t ignored, char32_t produced,
                                               bool command_or_control,
                                               std::uint8_t* mods) noexcept {
  if (base >= '0' && base <= '9') return char_key(static_cast<char>(base));
  const char32_t glyph = (!command_or_control && produced >= 0x21 && produced <= 0x7E) ? produced
                                                                                        : ignored;
  if (mods != nullptr) *mods = static_cast<std::uint8_t>(*mods & ~mod_shift);
  if (glyph >= 0x21 && glyph <= 0x7E) return char_key(static_cast<char>(glyph));
  return key::none;
}

inline constexpr int kModCombos = 8;

// plan/16 "Modes". Derived from state at keydown (key_router.h), never kept on
// a stack that can go stale when navigation lands somewhere else. Crop and the
// panes arrive with their PRs. `island` is the filmstrip or the gallery, where
// in-pane traversal keys fall through to XAML and letters are typeahead. The
// command bar and the transport keep the mode underneath (plan/12 2026-09-13).
enum class mode : std::uint8_t {
  browse = 0,
  video = 1,      // current item is a clip or an animation
  slideshow = 2,
  island = 3,     // focused thumbnail island; visible gallery takes precedence
  loupe = 4,      // Z held on the canvas: arrows nudge the loupe point
  gallery = 5,    // visible gallery owns navigation, even with canvas focus
  runner = 6,     // empty-window game; number keys do not change image zoom
  crop = 7,       // PR 10: crop / straighten on the canvas; arrows nudge the rect
  count
};

inline constexpr int kModeCount = static_cast<int>(mode::count);

using mode_mask = std::uint8_t;
inline constexpr mode_mask kBrowse = 1 << 0;
inline constexpr mode_mask kVideo = 1 << 1;
inline constexpr mode_mask kSlideshow = 1 << 2;
inline constexpr mode_mask kIsland = 1 << 3;
inline constexpr mode_mask kLoupe = 1 << 4;
inline constexpr mode_mask kGallery = 1 << 5;
inline constexpr mode_mask kRunner = 1 << 6;
inline constexpr mode_mask kCrop = 1 << 7;  // the last bit of the mask byte
inline constexpr mode_mask kAllModes =
    kBrowse | kVideo | kSlideshow | kIsland | kLoupe | kGallery | kRunner | kCrop;

[[nodiscard]] constexpr mode_mask mask_of(mode m) noexcept {
  return static_cast<mode_mask>(1u << static_cast<unsigned>(m));
}

// What typematic repeat and key-up mean for a binding.
enum class repeat_policy : std::uint8_t {
  edge,       // down edge only; repeats fall through unhandled
  repeat,     // every down, including typematic repeat
  tap_hold,   // `command` on down (a tap skips immediately); first typematic
              // repeat makes it a hold (`hold` per repeat, `release` on key-up)
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
  palette,  // unused: Ctrl+K palette dropped; keep the id so later commands do not shift
  go_to,
  folder_tree,
  typeahead,  // `/` from the canvas: find an item by name
  reveal_in_explorer,  // Ctrl+E: open the folder and select the current file
  gallery_up,
  gallery_down,
  gallery_open_selected,
  open_settings,  // Ctrl+, and the Settings button: the settings screen
  gallery_larger,
  gallery_smaller,
  // PR 7 pairs (plan/04, plan/16)
  play_motion,  // `;`: play a Live Photo's motion once, back to the still
  open_raw,     // show the RAW half of a RAW+JPEG stop (unbound by default)
  open_jpeg,    // back to the JPEG / HEIC half (unbound by default)
  mute,         // Shift+M (plan/16 Video): toggle the clip's audio
  game_toggle_3d,  // 3 in the empty-window runner only
  // PR 9 (plan/16 View + Overlays)
  metadata_pane,  // `I`: summary card, full tree, clip stream inspector
  af_points,      // Shift+O: AF-point quads from the maker notes already read
  eyedropper,     // Shift+I: one-pixel readout under the cursor
  copy_clipboard, // Ctrl/Cmd+C: the eyedropper's readout if it is on, else the marked / current file(s)
  // PR 10 (plan/16 View + Crop mode). Appended; every id above keeps its value.
  rotate_ccw,       // `[`: −90°, a lossless file write on a JPEG
  rotate_cw,        // `]`: +90°
  flip_horizontal,  // H
  flip_vertical,    // V
  crop_mode,        // Shift+C: enter crop / straighten
  crop_commit,      // Enter in crop mode (Esc cancels through `back`)
  crop_move_left,   // arrows in crop mode move the rect
  crop_move_right,
  crop_move_up,
  crop_move_down,
  crop_narrower,    // Shift+arrows in crop mode move the bottom-right corner
  crop_wider,
  crop_shorter,
  crop_taller,
  straighten_ccw,   // `,` `.` in crop mode: ∓0.5°
  straighten_cw,
  export_image,     // Ctrl+S: bake the stack into a new file
  undo_edit,        // Ctrl+Z: pop the edit stack
  reset_edits,      // Ctrl+R: clear it — the original, exactly
  folder_up,        // Ctrl/Cmd+Up, open the enclosing folder. Appended so the ids above keep their values.
  folder_prev,      // Ctrl/Cmd+Left, open the previous sibling folder
  folder_next,      // Ctrl/Cmd+Right, open the next sibling folder
  // PR 11 (plan/16 Pane: adjust). Appended; every id above keeps its value.
  adjust_pane,         // Shift+A: show / hide the adjust pane, focus its first slider
  // Island-only, keyless: the pane's sliders post their value as the command's
  // float argument, one id per parameter (edit::adjust_param order).
  adjust_exposure,
  adjust_contrast,
  adjust_saturation,
  adjust_temperature,
  adjust_tint,
  adjust_reset,        // the pane's Reset button: every slider back to 0, one undo step
  // Milestone G (plan/18 "Commands"). Appended. Listed, routed and shown only
  // while the Import add-on is installed (set_addon_commands_available).
  open_import,      // Ctrl+Shift+I: the Import window
  import_now,       // Ctrl+Shift+F7: import the marked / current file(s), last preset
  // PR 12 (plan/16 Rate). Appended; every id above keeps its value. Numpad
  // 0-5 or Ctrl+Shift+0-5 (Cmd+Shift on a Mac, which has no keypad on a laptop).
  // 0 clears the rating. Written to the file, or its sidecar, on the I/O pool.
  set_rating_0,
  set_rating_1,
  set_rating_2,
  set_rating_3,
  set_rating_4,
  set_rating_5,
  edit_comment,  // Ctrl+I: show the metadata pane and put the keyboard in its comment field
  count
};

inline constexpr int kCommandCount = static_cast<int>(command_id::count);

// set_rating_0..5 -> 0..5, else -1.
[[nodiscard]] constexpr int rating_of_command(command_id id) noexcept {
  const int v = static_cast<int>(id) - static_cast<int>(command_id::set_rating_0);
  return v >= 0 && v <= 5 ? v : -1;
}

[[nodiscard]] constexpr bool is_reserved_notification(int id) noexcept {
  return id == 15 || id == 16 || id == 18 || id == 19 || id == 20;
}

// Ids that once were commands and are kept as holes so later ids keep their
// wire values. 76 is the dropped Ctrl+K palette (plan/12 2026-09-13): no
// command_info, no binding, never dispatched.
[[nodiscard]] constexpr bool is_retired_command(int id) noexcept {
  return id == static_cast<int>(command_id::palette);
}

// A row whose key is key::none is listed (Settings can give it one) but not
// routed. Only commands plan/16 names without a key ship that way (PR 7: Open
// RAW / Open JPEG, which plan/04 put in the dropped palette).
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
  // no key by design. A retired command may also keep a keyless entry so its
  // wire id remains named without appearing in Settings or `?`.
  bool keyless = false;
};

[[nodiscard]] std::span<const binding> default_bindings() noexcept;
// The table the router, `?` and Settings share. Starts as default_bindings;
// remaps edit it in place so those three cannot drift.
[[nodiscard]] std::span<const binding> live_bindings() noexcept;
void reset_live_bindings() noexcept;
// Change row `index` to (k, mods). A collision in overlapping modes swaps the
// two rows so nothing is left unbound. False if index is out of range.
[[nodiscard]] bool rebind_live(int index, key k, std::uint8_t mods) noexcept;
[[nodiscard]] std::span<const command_info> command_infos() noexcept;
[[nodiscard]] const command_info* find_command(command_id id) noexcept;
// Bound commands whose effect has not landed yet. Empty before PR 6 ships.
[[nodiscard]] std::span<const command_id> pending_commands() noexcept;

// Add-on commands (plan/18: "They exist only while Import is installed").
// With the add-on absent, describe_commands() does not list them and the host
// does not run them, so the key falls through as if unbound.
[[nodiscard]] bool is_addon_command(command_id id) noexcept;
void set_addon_commands_available(bool available) noexcept;
[[nodiscard]] bool addon_commands_available() noexcept;

// "Ctrl+Shift+O", "Space", "F3", "?" — what `?` and the palette show.
[[nodiscard]] std::string key_label(key k, std::uint8_t mods);

// The table for the chrome (plan/16: the palette and `?` filter the same
// static table). One UTF-8 line per binding: "id\tmodes\tname\tkeys\n", where
// modes is the mode_mask the binding is live in and keys is its label
// ("hold Z" for a momentary key, "hold Q" for a tap/hold's hold command).
// Island-only and release commands are not listed. Bindings never cross the
// C ABI; this goes to the shell's own island.
[[nodiscard]] std::string describe_commands();

// Whether the palette may run `id` on its own. A palette entry has no key-up,
// so a momentary key's command, a tap/hold's hold, and any release are shown
// in `?` but not offered to run (review note 39). The fifth column of
// describe_commands() is this, as 1 or 0.
[[nodiscard]] bool palette_runnable(command_id id) noexcept;

}  // namespace mv::shell
