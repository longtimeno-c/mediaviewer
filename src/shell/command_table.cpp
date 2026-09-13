// SPDX-License-Identifier: GPL-2.0-or-later
// The default map (plan/16-commands.md "Default map"). FastStone / IrfanView
// muscle memory. Later slices add rows here; they do not grow a second router.
#include "shell/commands.h"

#include <iterator>

namespace mv::shell {
namespace {

using enum command_id;
using enum repeat_policy;

constexpr mode_mask kViewing = kBrowse | kVideo | kIsland;  // not slideshow
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
    row(key::enter, mod_none, kBrowse, edge, slideshow_start),
    row(C('F'), mod_none, kAllModes, edge, fullscreen),
    row(C('O'), mod_ctrl, kAllModes, edge, open),
    row(C('O'), mod_ctrl | mod_shift, kAllModes, edge, open_folder),
    row(C('W'), mod_ctrl, kAllModes, edge, close_window),

    // View. Number row is zoom; ratings never take these keys.
    row(C('0'), mod_none, kViewing, edge, fit),
    row(C('1'), mod_none, kViewing, edge, one_to_one),
    row(C('2'), mod_none, kViewing, edge, zoom_200),
    row(C('3'), mod_none, kViewing, edge, zoom_400),
    row(C('4'), mod_none, kViewing, edge, fill),
    row(C('0'), mod_ctrl, kViewing, edge, reset_view),
    row(C('+'), mod_none, kViewing, repeat, zoom_in),
    row(C('='), mod_none, kViewing, repeat, zoom_in),  // the unshifted +/= key
    row(C('-'), mod_none, kViewing, repeat, zoom_out),
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
    row(C('S'), mod_none, kViewing, edge, sticky_zoom),
    row(C('C'), mod_none, kViewing, edge, clipping),
    row(C('O'), mod_none, kViewing, edge, info_overlay),
    row(C('Z'), mod_none, kBrowse | kVideo, momentary, loupe, none, loupe_release),
    row(C('\\'), mod_none, kBrowse | kVideo, momentary, hold_previous, none,
        hold_previous_release),
    row(C('A'), mod_ctrl | mod_shift, kAllModes, edge, always_on_top),

    // Video (PR 5c). Q/E: tap steps the speed, hold skims, release settles.
    row(C('J'), mod_none, kVideo, edge, jump_back),
    row(C('K'), mod_none, kVideo, edge, pause),
    row(C('L'), mod_none, kVideo, edge, jump_forward),
    row(C(','), mod_none, kVideo, edge, frame_back),
    row(C('.'), mod_none, kVideo, edge, frame_forward),
    row(C('Q'), mod_none, kVideo, tap_hold, rate_down, skim_back, skim_settle),
    row(C('E'), mod_none, kVideo, tap_hold, rate_up, skim_forward, skim_settle),

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

    // Palette and help.
    row(C('?'), mod_none, kAllModes, edge, help),
    row(C('K'), mod_ctrl, kAllModes, edge, palette),
    row(C('P'), mod_ctrl | mod_shift, kAllModes, edge, palette),
    row(C('G'), mod_ctrl, kViewing, edge, go_to),
    row(C('E'), mod_ctrl | mod_shift, kAllModes, edge, folder_tree),
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
    {skim_back, "Skim back"},
    {skim_forward, "Skim forward"},
    {skim_settle, "Settle skim"},
    {reset_stats, "Reset frame stats"},
    {close_window, "Close window"},
    {zoom_200, "Zoom 200 %"},
    {zoom_400, "Zoom 400 %"},
    {fullscreen, "Fullscreen"},
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
    {palette, "Command palette"},
    {go_to, "Go to index…"},
    {folder_tree, "Folder tree"},
};

// Bound, but main.cpp's run_command does not handle them yet: the key falls
// through to the island. Each slice deletes its rows here as it lands, and
// this list is empty before PR 6 is proposed (6g).
constexpr command_id kPending[] = {
    // 6b
    fullscreen, fill, reset_view, cycle_background, sticky_zoom, clipping, loupe,
    loupe_release, hold_previous, hold_previous_release, always_on_top, info_overlay,
    pan_up, pan_down, pan_left, pan_right,
    // 6c
    toggle_mark, mark_all, unmark_all, copy_to, copy_to_pick, move_to, move_to_pick,
    delete_to_recycle_bin,
    // 6d
    slideshow_start, slideshow_pause, slideshow_faster, slideshow_slower, blackout, shuffle,
    // 6f
    help, palette, go_to, folder_tree,
};

}  // namespace

std::span<const binding> default_bindings() noexcept { return kBindings; }
std::span<const command_info> command_infos() noexcept { return kCommands; }
std::span<const command_id> pending_commands() noexcept { return kPending; }

const command_info* find_command(command_id id) noexcept {
  for (const auto& info : kCommands) {
    if (info.id == id) return &info;
  }
  return nullptr;
}

}  // namespace mv::shell
