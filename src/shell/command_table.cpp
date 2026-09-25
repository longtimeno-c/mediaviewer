// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The default map (plan/16-commands.md "Default map"). FastStone / IrfanView
// muscle memory. Later slices add rows here; they do not grow a second router.
#include "shell/commands.h"

#include <atomic>
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

// Crop mode (PR 10) owns its keys: walking the folder mid-crop would throw
// the crop away, so A/D stop there; Esc, `?`, F3, fullscreen still work.
constexpr mode_mask kNotCrop = kAllModes & static_cast<mode_mask>(~kCrop);

constexpr binding kBindings[] = {
    // Browse. A/D walk the folder in every mode, including on a clip and with
    // the filmstrip focused (the strip only handles its own arrows).
    row(key::left, mod_none, kWalk, repeat, prev),
    row(key::right, mod_none, kWalk, repeat, next),
    row(C('A'), mod_none, kNotCrop, repeat, prev),
    row(C('D'), mod_none, kNotCrop, repeat, next),
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
    row(C('O'), mod_ctrl, kNotCrop, edge, open),
    row(C('O'), mod_ctrl | mod_shift, kNotCrop, edge, open_folder),
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
    row(C('G'), mod_none, kNotCrop, edge, toggle_gallery),
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
    row(C(','), mod_ctrl, kNotCrop, edge, open_settings),
    row(C('G'), mod_ctrl, kViewing, edge, go_to),
    row(C('E'), mod_ctrl | mod_shift, kNotCrop, edge, folder_tree),
    // plan/16 typeahead (plan/12 2026-09-13): `/` opens find from the canvas;
    // with the filmstrip or gallery focused, plain typing jumps by name.
    // In the gallery, `/` with the folder row active finds a folder tile.
    row(C('/'), mod_none, kBrowse | kVideo | kGallery, edge, typeahead),
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
    // plan/16 Video: Shift+M is mute (M itself stays free for a Move preset).
    row(C('M'), mod_shift, kVideo, edge, mute),
    // Append: Settings persists the existing row indices.
    row(key::space, mod_none, kRunner, edge, next),
    row(C('3'), mod_none, kRunner, edge, game_toggle_3d),
    // PR 9. Appended, like every row before it, so saved Settings indices keep
    // their meaning. plan/16 gives `I` the metadata pane; the two overlays are
    // Shift twins of the keys they sit beside (`O` info, `I` pane) because
    // every other letter on the canvas already has a job.
    row(C('I'), mod_none, kViewing, edge, metadata_pane),
    row(C('O'), mod_shift, kViewing, edge, af_points),
    row(C('I'), mod_shift, kViewing, edge, eyedropper),
    // Ctrl+C (plan/16): with the eyedropper on it copies the colour; otherwise it
    // copies the marked (or current / gallery-selected) files.
    row(C('C'), mod_ctrl, kViewing, edge, copy_clipboard),
    // PR 10 (plan/16 View, Crop mode). Appended. `[` `]` H V act on a still in
    // browse (a clip takes `[` `]` for trim in PR 13); on a JPEG with nothing
    // else in its stack they rewrite the file losslessly, no pane needed.
    row(C('['), mod_none, kBrowse | kCrop, edge, rotate_ccw),
    row(C(']'), mod_none, kBrowse | kCrop, edge, rotate_cw),
    row(C('H'), mod_none, kBrowse | kCrop, edge, flip_horizontal),
    row(C('V'), mod_none, kBrowse | kCrop, edge, flip_vertical),
    // Shift+C: C is clipping; the Shift twin, as PR 9 did for O and I.
    row(C('C'), mod_shift, kBrowse, edge, crop_mode),
    row(key::enter, mod_none, kCrop, edge, crop_commit),
    row(key::left, mod_none, kCrop, repeat, crop_move_left),
    row(key::right, mod_none, kCrop, repeat, crop_move_right),
    row(key::up, mod_none, kCrop, repeat, crop_move_up),
    row(key::down, mod_none, kCrop, repeat, crop_move_down),
    row(key::left, mod_shift, kCrop, repeat, crop_narrower),
    row(key::right, mod_shift, kCrop, repeat, crop_wider),
    row(key::up, mod_shift, kCrop,repeat, crop_shorter),
    row(key::down, mod_shift, kCrop, repeat, crop_taller),
    row(C(','), mod_none, kCrop, repeat, straighten_ccw),
    row(C('.'), mod_none, kCrop, repeat, straighten_cw),
    row(C('S'), mod_ctrl, kBrowse, edge, export_image),
    row(C('Z'), mod_ctrl, kBrowse | kCrop, edge, undo_edit),
    row(C('R'), mod_ctrl, kBrowse | kCrop, edge, reset_edits),
    // Up one folder (Finder's Cmd+Up). Appended so the rows above keep their
    // Settings indices. Only meaningful with a folder open that has a parent;
    // the host answers "not handled" otherwise.
    row(key::up, mod_ctrl, kViewing, edge, folder_up),
    // Previous / next folder beside the one open. Browse and video only: in
    // the gallery, Left / Right already move among the tiles.
    row(key::left, mod_ctrl, kBrowse | kVideo, edge, folder_prev),
    row(key::right, mod_ctrl, kBrowse | kVideo, edge, folder_next),
    // PR 11. plan/16 leaves `E` to the clip transport (Q / E) and asks PR 11
    // for another key: the Shift twin of `A`djust, as PR 9 / 10 did for O, I
    // and C. Stills only; the pane's own close button sends the same id.
    row(C('A'), mod_shift, kBrowse, edge, adjust_pane),
    // Milestone G: only while the Import add-on is installed (plan/18).
    row(C('I'), mod_ctrl | mod_shift, kViewing, edge, open_import),
    row(key::f7, mod_ctrl | mod_shift, kViewing, edge, import_now),
    // PR 12 (plan/16 Rate). Appended. The keypad digits are their own keys;
    // the number row stays zoom (0 fit, 1 100 %), so a laptop with no keypad
    // rates on Ctrl+Shift+digit (Cmd+Shift on a Mac). Not in a slideshow or
    // crop mode: a rating is a write, and neither is a place to make one by
    // accident. Only on the item on screen; a batch is v1.1.
    row(key::numpad0, mod_none, kViewing, edge, set_rating_0),
    row(key::numpad1, mod_none, kViewing, edge, set_rating_1),
    row(key::numpad2, mod_none, kViewing, edge, set_rating_2),
    row(key::numpad3, mod_none, kViewing, edge, set_rating_3),
    row(key::numpad4, mod_none, kViewing, edge, set_rating_4),
    row(key::numpad5, mod_none, kViewing, edge, set_rating_5),
    row(C('0'), mod_ctrl | mod_shift, kViewing, edge, set_rating_0),
    row(C('1'), mod_ctrl | mod_shift, kViewing, edge, set_rating_1),
    row(C('2'), mod_ctrl | mod_shift, kViewing, edge, set_rating_2),
    row(C('3'), mod_ctrl | mod_shift, kViewing, edge, set_rating_3),
    row(C('4'), mod_ctrl | mod_shift, kViewing, edge, set_rating_4),
    row(C('5'), mod_ctrl | mod_shift, kViewing, edge, set_rating_5),
    // plan/16 gives the comment no key, only "user comment in the pane" and a
    // rule that a focused text control owns the keyboard. Ctrl+I is `I`
    // (the pane) with the edit modifier, and puts the keyboard in the field so
    // the comment is reachable without the mouse.
    row(C('I'), mod_ctrl, kViewing, edge, edit_comment),
    // PR 13 (plan/16 "Video and trim", plan/08). Appended. Trim mode layers
    // over video (key_router.cpp), so Space, J K L, Q E and , . keep driving
    // the clip; these rows are only what trim adds or takes over. `[` `]` are
    // the markers here and nothing on a clip otherwise. Ctrl+Left / Right are
    // the keyframe walk plan/16 names, which in video stay folder nav.
    row(C('T'), mod_ctrl, kVideo | kTrim, edge, trim_mode),
    row(C('['), mod_none, kTrim, edge, trim_in),
    row(C(']'), mod_none, kTrim, edge, trim_out),
    row(key::backspace, mod_none, kTrim, edge, trim_clear),
    // The Mac's delete key is `del` (it has no Backspace), and in trim mode
    // Delete falling through to video would move the clip being trimmed to
    // the Trash. Both keys clear the markers here instead.
    row(key::del, mod_none, kTrim, edge, trim_clear),
    row(C('P'), mod_none, kTrim, edge, trim_preview),
    row(key::enter, mod_none, kTrim, edge, trim_keyframe),
    row(key::enter, mod_shift, kTrim, edge, trim_reencode),
    row(key::left, mod_ctrl, kTrim, repeat, keyframe_prev),
    row(key::right, mod_ctrl, kTrim, repeat, keyframe_next),
    row(C('J'), mod_ctrl, kViewing | kTrim, edge, jobs_pane),
    // PR 14. Ctrl+S is Export on a still; on a clip it opens the clip tools.
    row(C('S'), mod_ctrl, kVideo | kTrim, edge, clip_tools),
    row(C('B'), mod_ctrl, kVideo | kTrim, edge, clip_split),
    row(C('X'), mod_ctrl, kTrim, edge, trim_remove_middle),
    // PR 15 (plan/16 View): the keyboard twins of drag-out. Appended. Ctrl+C
    // copies the file(s); the Shift twin their path(s); Ctrl+Alt+C what the
    // canvas shows with the edits baked (stills: a clip's frame is PR 14's
    // frame export). Ctrl+Shift+S is Share; Ctrl+S stays Export / clip tools.
    row(C('C'), mod_ctrl | mod_shift, kViewing, edge, copy_path),
    row(C('C'), mod_ctrl | mod_alt, kBrowse, edge, copy_flattened),
    row(C('S'), mod_ctrl | mod_shift, kViewing, edge, share),
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
    // The island TextBox fail-fast retired Ctrl+K, but this id deliberately
    // remains in the wire enum so every later command keeps its value.
    {palette, "Command palette (retired)", true},
    {go_to, "Go to index…"},
    {folder_tree, "Folder tree"},
    {typeahead, "Find by name…"},
    {reveal_in_explorer, "Show in Explorer"},
    {open_settings, "Settings"},
    {play_motion, "Play Live Photo motion"},
    {open_raw, "Open RAW of pair"},
    {mute, "Mute"},
    {open_jpeg, "Open JPEG of pair"},
    {game_toggle_3d, "Runner: toggle 3D view"},
    {metadata_pane, "Metadata pane"},
    {af_points, "AF points"},
    {eyedropper, "Eyedropper"},
    {copy_clipboard, "Copy"},
    {rotate_ccw, "Rotate left"},
    {rotate_cw, "Rotate right"},
    {flip_horizontal, "Flip horizontal"},
    {flip_vertical, "Flip vertical"},
    {crop_mode, "Crop / straighten"},
    {crop_commit, "Apply crop"},
    {crop_move_left, "Crop: move left"},
    {crop_move_right, "Crop: move right"},
    {crop_move_up, "Crop: move up"},
    {crop_move_down, "Crop: move down"},
    {crop_narrower, "Crop: narrower"},
    {crop_wider, "Crop: wider"},
    {crop_shorter, "Crop: shorter"},
    {crop_taller, "Crop: taller"},
    {straighten_ccw, "Straighten left"},
    {straighten_cw, "Straighten right"},
    {export_image, "Export…"},
    {undo_edit, "Undo edit"},
    {reset_edits, "Reset edits"},
    {folder_up, "Up one folder"},
    {folder_prev, "Previous folder"},
    {folder_next, "Next folder"},
    {adjust_pane, "Adjust pane"},
    {adjust_exposure, "Adjust: exposure", true},
    {adjust_contrast, "Adjust: contrast", true},
    {adjust_saturation, "Adjust: saturation", true},
    {adjust_temperature, "Adjust: temperature", true},
    {adjust_tint, "Adjust: tint", true},
    {adjust_reset, "Adjust: reset", true},
    {open_import, "Import…"},
    {import_now, "Import marked now"},
    {set_rating_0, "Rating: none"},
    {set_rating_1, "Rating: 1 star"},
    {set_rating_2, "Rating: 2 stars"},
    {set_rating_3, "Rating: 3 stars"},
    {set_rating_4, "Rating: 4 stars"},
    {set_rating_5, "Rating: 5 stars"},
    {edit_comment, "Edit comment"},
    {trim_mode, "Trim clip"},
    {trim_in, "Trim: in marker"},
    {trim_out, "Trim: out marker"},
    {trim_clear, "Trim: clear markers"},
    {trim_preview, "Trim: preview the cut"},
    {trim_keyframe, "Trim: save (keyframe, instant)"},
    {trim_reencode, "Trim: save (re-encode, frame-accurate, slower)"},
    {keyframe_prev, "Previous keyframe"},
    {keyframe_next, "Next keyframe"},
    {jobs_pane, "Jobs"},
    {clip_tools, "Clip tools…"},
    {clip_split, "Split clip at playhead"},
    {trim_remove_middle, "Trim: remove in–out"},
    {copy_path, "Copy path"},
    {copy_flattened, "Copy edited image"},
    {share, "Share…"},
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
    case key::numpad0: return "Numpad 0";
    case key::numpad1: return "Numpad 1";
    case key::numpad2: return "Numpad 2";
    case key::numpad3: return "Numpad 3";
    case key::numpad4: return "Numpad 4";
    case key::numpad5: return "Numpad 5";
    case key::numpad6: return "Numpad 6";
    case key::numpad7: return "Numpad 7";
    case key::numpad8: return "Numpad 8";
    case key::numpad9: return "Numpad 9";
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

namespace {
std::atomic<bool> g_addon_commands{false};
}  // namespace

bool is_addon_command(command_id id) noexcept { return id == open_import || id == import_now; }
void set_addon_commands_available(bool available) noexcept { g_addon_commands = available; }
bool addon_commands_available() noexcept { return g_addon_commands.load(); }

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
    if (is_addon_command(id) && !addon_commands_available()) return;
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
