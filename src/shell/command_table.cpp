// SPDX-License-Identifier: GPL-2.0-or-later
// The default map (plan/16-commands.md "Default map"). FastStone / IrfanView
// muscle memory. Later slices add rows here; they do not grow a second router.
#include "shell/commands.h"

#include <iterator>
#include <vector>

namespace mv::shell {
namespace {

using enum command_id;
using enum repeat_policy;

constexpr mode_mask kViewing = kBrowse | kVideo | kIsland | kGallery;  // not slideshow
constexpr mode_mask kImageZoom = kBrowse | kVideo | kIsland;
// mode::loupe layers over browse / video: the router falls back to them for any
// key without a kLoupe row, so only the arrows and Z / \ are bound there.
constexpr mode_mask kWalk = kBrowse | kVideo | kSlideshow;  // arrows: island traverses itself

constexpr binding row(key k, std::uint8_t mods, mode_mask modes, repeat_policy policy,
                      command_id command, command_id hold = none,
                      command_id release = none) noexcept {
  return binding{k, mods, modes, policy, command, hold, release};
}

constexpr key C(char c) noexcept { return char_key(c); }

constexpr binding kBindings[] = {
    // Browse. A/D walk the folder in every mode, including on a clip and with
    // the filmstrip focused (the strip only handles its own arrows).
    row(key::left, mod_none, kWalk, repeat, prev),
    row(key::right, mod_none, kWalk, repeat, next),
    row(C('A'), mod_none, kAllModes, repeat, prev),
    row(C('D'), mod_none, kAllModes, repeat, next),
    row(key::backspace, mod_none, kWalk, repeat, prev),
    // Space is next on a still, play/pause on a clip or animation, pause in a
    // slideshow. It is never the lab sweep.
    row(key::space, mod_none, kBrowse, repeat, next),
    row(key::space, mod_none, kVideo, edge, play_pause),
    row(key::space, mod_none, kSlideshow, edge, slideshow_pause),
    row(key::home, mod_none, kWalk, edge, first),
    row(key::end, mod_none, kWalk, edge, last),
    row(key::page_up, mod_none, kWalk, repeat, skip_back),
    row(key::page_down, mod_none, kWalk, repeat, skip_forward),
    row(key::escape, mod_none, kAllModes, edge, back),
    row(key::f5, mod_none, kBrowse | kVideo, edge, slideshow_start),
    row(C('F'), mod_none, kAllModes, edge, fullscreen),
    row(key::f11, mod_none, kAllModes, edge, fullscreen),
    row(key::enter, mod_none, kGallery, edge, gallery_open_selected),
    row(C('W'), mod_none, kGallery, repeat, gallery_up),
    row(C('S'), mod_none, kGallery, repeat, gallery_down),
    row(key::up, mod_none, kGallery, repeat, gallery_up),
    row(key::down, mod_none, kGallery, repeat, gallery_down),
    row(key::left, mod_none, kGallery, repeat, prev),
    row(key::right, mod_none, kGallery, repeat, next),
    row(C('O'), mod_ctrl, kAllModes, edge, open),
    row(C('O'), mod_ctrl | mod_shift, kAllModes, edge, open_folder),
    row(C('E'), mod_ctrl, kAllModes, edge, reveal_in_explorer),
    row(C('W'), mod_ctrl, kAllModes, edge, close_window),

    // View. Number row is zoom; ratings never take these keys.
    row(C('0'), mod_none, kViewing, edge, fit),
    row(C('1'), mod_none, kViewing, edge, one_to_one),
    row(C('2'), mod_none, kViewing, edge, zoom_200),
    row(C('3'), mod_none, kViewing, edge, zoom_400),
    row(C('4'), mod_none, kViewing, edge, fill),
    row(C('0'), mod_ctrl, kViewing, edge, reset_view),
    row(C('+'), mod_none, kImageZoom, repeat, zoom_in),
    row(C('='), mod_none, kImageZoom, repeat, zoom_in),  // the unshifted +/= key
    row(C('-'), mod_none, kImageZoom, repeat, zoom_out),
    // plan/16 Browse: when zoomed ↑ ↓ pan and ← → still navigate; Shift+arrows
    // pan in all four directions.
    row(key::up, mod_none, kBrowse | kVideo, repeat, pan_up),
    row(key::down, mod_none, kBrowse | kVideo, repeat, pan_down),
    row(key::up, mod_shift, kBrowse | kVideo, repeat, pan_up),
    row(key::down, mod_shift, kBrowse | kVideo, repeat, pan_down),
    row(key::left, mod_shift, kBrowse | kVideo, repeat, pan_left),
    row(key::right, mod_shift, kBrowse | kVideo, repeat, pan_right),
    row(key::f3, mod_none, kAllModes, edge, overlay),
    row(C('R'), mod_none, kViewing, edge, reset_stats),
    row(C('G'), mod_none, kAllModes, edge, toggle_gallery),
    row(C('T'), mod_none, kAllModes, edge, toggle_filmstrip),
    row(C('B'), mod_none, kViewing, edge, cycle_background),
    row(C('S'), mod_none, kBrowse | kVideo | kIsland, edge, sticky_zoom),
    row(C('C'), mod_none, kViewing, edge, clipping),
    row(C('O'), mod_none, kViewing, edge, info_overlay),
    row(C('Z'), mod_none, kBrowse | kVideo | kLoupe, momentary, loupe, none, loupe_release),
    row(C('\\'), mod_none, kBrowse | kVideo | kLoupe, momentary, hold_previous, none,
        hold_previous_release),
    // While Z is held the arrows move the loupe point, so it works with no
    // cursor at all. A / D still walk the folder.
    row(key::left, mod_none, kLoupe, repeat, loupe_nudge_left),
    row(key::right, mod_none, kLoupe, repeat, loupe_nudge_right),
    row(key::up, mod_none, kLoupe, repeat, loupe_nudge_up),
    row(key::down, mod_none, kLoupe, repeat, loupe_nudge_down),
    row(C('A'), mod_ctrl | mod_shift, kAllModes, edge, always_on_top),

    // Video (PR 5c). Q/E: tap skips ±2 s, hold skims, release settles.
    // Speed stays on the command-bar dropdown (one owner; native pushes it).
    row(C('J'), mod_none, kVideo, edge, jump_back),
    row(C('K'), mod_none, kVideo, edge, pause),
    row(C('L'), mod_none, kVideo, edge, jump_forward),
    row(C(','), mod_none, kVideo, edge, frame_back),
    row(C('.'), mod_none, kVideo, edge, frame_forward),
    row(C('Q'), mod_none, kVideo, tap_hold, skim_back, skim_back, skim_settle),
    row(C('E'), mod_none, kVideo, tap_hold, skim_forward, skim_forward, skim_settle),
    row(C('Q'), mod_shift, kVideo, edge, rate_down),
    row(C('E'), mod_shift, kVideo, edge, rate_up),

    // Marks, copy, move.
    row(key::insert, mod_none, kBrowse | kVideo, edge, toggle_mark),
    row(key::space, mod_shift, kBrowse | kVideo, edge, toggle_mark),
    row(C('A'), mod_ctrl, kViewing, edge, mark_all),
    row(C('D'), mod_ctrl, kViewing, edge, unmark_all),
    row(key::f7, mod_none, kViewing, edge, copy_to),
    row(key::f7, mod_shift, kViewing, edge, copy_to_pick),
    row(key::f8, mod_none, kViewing, edge, move_to),
    row(key::f8, mod_shift, kViewing, edge, move_to_pick),
    row(key::del, mod_none, kBrowse | kVideo, edge, delete_to_recycle_bin),

    // Slideshow.
    row(C('+'), mod_none, kSlideshow, repeat, slideshow_faster),
    row(C('='), mod_none, kSlideshow, repeat, slideshow_faster),
    row(C('-'), mod_none, kSlideshow, repeat, slideshow_slower),
    row(C('.'), mod_none, kSlideshow, edge, blackout),
    row(C('R'), mod_none, kSlideshow, edge, shuffle),

    // Help and find. The Ctrl+K palette was dropped (plan/12 2026-09-13): a
    // TextBox in the island flyout fail-fasts, and bound keys never reach it.
    row(C('?'), mod_none, kAllModes, edge, help),
    row(C(','), mod_ctrl, kAllModes, edge, open_settings),
    row(C('G'), mod_ctrl, kViewing, edge, go_to),
    row(C('E'), mod_ctrl | mod_shift, kAllModes, edge, folder_tree),
    // plan/16 typeahead (plan/12 2026-09-13): `/` opens find from the canvas;
    // with the filmstrip or gallery focused, plain typing jumps by name.
    row(C('/'), mod_none, kBrowse | kVideo, edge, typeahead),
    // Append rows so existing Settings row indices keep their meaning.
    row(C('+'), mod_none, kGallery, repeat, gallery_larger),
    row(C('='), mod_none, kGallery, repeat, gallery_larger),
    row(C('-'), mod_none, kGallery, repeat, gallery_smaller),
    // PR 7. `;` plays a Live Photo's motion once (plan/16 View). Edge only:
    // hold-to-play on a repeating key is forbidden (plan/04). Video mode too,
    // because the motion playing *is* a clip on screen and `;` again stops it.
    row(C(';'), mod_none, kBrowse | kVideo, edge, play_motion),
    // plan/04: "Open RAW" / "Open JPEG" stay reachable so a paired file is never
    // trapped. The palette that was to hold them is gone (plan/12 2026-09-13)
    // and plan/16 gives no key, so they are listed in Settings, unbound.
    row(key::none, mod_none, kBrowse | kVideo, edge, open_raw),
    row(key::none, mod_none, kBrowse | kVideo, edge, open_jpeg),
};

// The router's index stores row + 1 in a byte.
static_assert(std::size(kBindings) < 255, "key_router index is uint8_t");

constexpr command_info kCommands[] = {
    {open, "Open media…"},
    {fit, "Fit"},
    {one_to_one, "Zoom 100 %"},
    {zoom_in, "Zoom in"},
    {zoom_out, "Zoom out"},
    {zoom_preset, "Zoom preset", true},
    {overlay, "Frame-time overlay"},
    {select_item, "Select item", true},
    {prev, "Previous"},
    {next, "Next"},
    {open_folder, "Open folder…"},
    {toggle_gallery, "Gallery"},
    {close_gallery, "Close gallery", true},
    {gallery_activate, "Open from gallery", true},
    {toggle_filmstrip, "Filmstrip"},
    {back, "Back"},
    {first, "First"},
    {last, "Last"},
    {skip_back, "Back ten"},
    {skip_forward, "Forward ten"},
    {play_pause, "Play / pause"},
    {jump_back, "Back 10 s"},
    {pause, "Pause"},
    {jump_forward, "Forward 10 s"},
    {frame_back, "Previous frame"},
    {frame_forward, "Next frame"},
    {rate_down, "Slower"},
    {rate_up, "Faster"},
    {skim_back, "Skip back 2 s"},
    {skim_forward, "Skip forward 2 s"},
    {skim_settle, "Settle skim"},
    {reset_stats, "Reset frame stats"},
    {close_window, "Close window"},
    {zoom_200, "Zoom 200 %"},
    {zoom_400, "Zoom 400 %"},
    {fullscreen, "Fullscreen"},
    {gallery_up, "Gallery: previous row"},
    {gallery_down, "Gallery: next row"},
    {gallery_open_selected, "Gallery: open selected image"},
    {gallery_larger, "Gallery: larger thumbnails"},
    {gallery_smaller, "Gallery: smaller thumbnails"},
    {fill, "Fill"},
    {reset_view, "Reset view"},
    {cycle_background, "Canvas background"},
    {sticky_zoom, "Sticky zoom"},
    {clipping, "Clipping"},
    {loupe, "Loupe"},
    {loupe_release, "Loupe off"},
    {hold_previous, "Hold previous"},
    {hold_previous_release, "Release previous"},
    {always_on_top, "Always on top"},
    {info_overlay, "Info overlay"},
    {pan_up, "Pan up"},
    {pan_down, "Pan down"},
    {pan_left, "Pan left"},
    {pan_right, "Pan right"},
    {loupe_nudge_left, "Loupe left"},
    {loupe_nudge_right, "Loupe right"},
    {loupe_nudge_up, "Loupe up"},
    {loupe_nudge_down, "Loupe down"},
    {toggle_mark, "Toggle mark"},
    {mark_all, "Mark all"},
    {unmark_all, "Unmark all"},
    {copy_to, "Copy to last folder"},
    {copy_to_pick, "Copy to…"},
    {move_to, "Move to last folder"},
    {move_to_pick, "Move to…"},
    {delete_to_recycle_bin, "Delete to Recycle Bin"},
    {slideshow_start, "Slideshow"},
    {slideshow_pause, "Pause slideshow"},
    {slideshow_faster, "Shorter interval"},
    {slideshow_slower, "Longer interval"},
    {blackout, "Blackout"},
    {shuffle, "Shuffle"},
    {help, "Keyboard shortcuts"},
    {go_to, "Go to index…"},
    {folder_tree, "Folder tree"},
    {typeahead, "Find by name…"},
    {reveal_in_explorer, "Show in Explorer"},
    {open_settings, "Settings"},
    {play_motion, "Play Live Photo motion"},
    {open_raw, "Open RAW of pair"},
    {open_jpeg, "Open JPEG of pair"},
};

const char* named_key(key k) noexcept {
  switch (k) {
    case key::space: return "Space";
    case key::backspace: return "Backspace";
    case key::enter: return "Enter";
    case key::escape: return "Esc";
    case key::tab: return "Tab";
    case key::insert: return "Insert";
    case key::del: return "Delete";
    case key::home: return "Home";
    case key::end: return "End";
    case key::page_up: return "PageUp";
    case key::page_down: return "PageDown";
    case key::left: return "Left";
    case key::right: return "Right";
    case key::up: return "Up";
    case key::down: return "Down";
    case key::f1: return "F1";
    case key::f2: return "F2";
    case key::f3: return "F3";
    case key::f4: return "F4";
    case key::f5: return "F5";
    case key::f6: return "F6";
    case key::f7: return "F7";
    case key::f8: return "F8";
    case key::f9: return "F9";
    case key::f10: return "F10";
    case key::f11: return "F11";
    case key::f12: return "F12";
    default: return nullptr;
  }
}

}  // namespace

std::span<const binding> default_bindings() noexcept { return kBindings; }

namespace {

std::vector<binding>& live_store() {
  static std::vector<binding> live{std::begin(kBindings), std::end(kBindings)};
  return live;
}

}  // namespace

std::span<const binding> live_bindings() noexcept { return live_store(); }

void reset_live_bindings() noexcept {
  live_store().assign(std::begin(kBindings), std::end(kBindings));
}

bool rebind_live(int index, key k, std::uint8_t mods) noexcept {
  auto& live = live_store();
  if (index < 0 || static_cast<std::size_t>(index) >= live.size()) return false;
  if (static_cast<int>(k) <= 0 || static_cast<int>(k) >= kKeyCount) return false;
  if (mods >= kModCombos) return false;
  binding& target = live[static_cast<std::size_t>(index)];
  if (target.k == k && target.mods == mods) return true;
  // Giving an unbound row a key another row holds swaps as usual, so that
  // other row becomes the unbound one — visible in Settings, never silent.
  for (std::size_t i = 0; i < live.size(); ++i) {
    if (static_cast<int>(i) == index) continue;
    binding& other = live[i];
    if (other.k == k && other.mods == mods && (other.modes & target.modes) != 0) {
      const key old_k = target.k;
      const std::uint8_t old_mods = target.mods;
      target.k = k;
      target.mods = mods;
      other.k = old_k;
      other.mods = old_mods;
      return true;
    }
  }
  target.k = k;
  target.mods = mods;
  return true;
}

std::span<const command_info> command_infos() noexcept { return kCommands; }

// Every bound command is handled by run_command as of PR 6 (6f emptied this;
// the folder tree's command is handled as a documented slip, plan/12).
std::span<const command_id> pending_commands() noexcept { return {}; }

std::string key_label(key k, std::uint8_t mods) {
  std::string out;
  if (mods & mod_ctrl) out += "Ctrl+";
  if (mods & mod_shift) out += "Shift+";
  if (mods & mod_alt) out += "Alt+";
  if (const char* name = named_key(k)) {
    out += name;
  } else {
    const auto v = static_cast<std::uint16_t>(k);
    if (v >= 0x21 && v <= 0x7E) out.push_back(static_cast<char>(v));
  }
  return out;
}

bool palette_runnable(command_id id) noexcept {
  if (id == none) return false;
  for (const binding& b : live_bindings()) {
    if (b.policy == momentary && (b.command == id || b.release == id)) return false;
    // A tap/hold's hold is not runnable unless it is also the tap (Q/E skip
    // on down and on repeat). The release always needs a key-up.
    if (b.policy == tap_hold && b.release == id) return false;
    if (b.policy == tap_hold && b.hold == id && b.command != id) return false;
  }
  return true;
}

std::string describe_commands() {
  std::string out;
  out.reserve(4096);
  const auto line = [&out](command_id id, mode_mask modes, std::string keys, std::size_t row) {
    const command_info* info = find_command(id);
    if (!info || info->keyless || id == back) return;
    out += std::to_string(static_cast<int>(id));
    out += '\t';
    out += std::to_string(static_cast<int>(modes));
    out += '\t';
    out += info->name;
    out += '\t';
    out += keys;
    out += '\t';
    out += palette_runnable(id) ? '1' : '0';
    out += '\t';
    out += std::to_string(row);
    out += '\n';
  };
  const auto rows = live_bindings();
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const binding& b = rows[i];
    const std::string label = key_label(b.k, b.mods);
    if (b.policy == momentary) {
      line(b.command, b.modes, "hold " + label, i);
    } else if (b.policy == tap_hold) {
      line(b.command, b.modes, label, i);
      line(b.hold, b.modes, "hold " + label, i);
    } else {
      line(b.command, b.modes, label, i);
    }
  }
  return out;
}

const command_info* find_command(command_id id) noexcept {
  for (const auto& info : kCommands) {
    if (info.id == id) return &info;
  }
  return nullptr;
}

}  // namespace mv::shell
