// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Loads the C# WinUI chrome and attaches a DesktopWindowXamlSource island.
//
// docs/design/10 PR 3, D1 amendment: the Win32 window and D3D11 swapchain stay the
// app; chrome is hosted inside them. C++ never talks to WinUI types — it loads
// MediaViewer.Chrome.dll through hostfxr and calls a handful of entry points.
// Canvas mouse-move does not cross this boundary (docs/design/02).
#pragma once

#include <windows.h>

#include <cstdint>
#include <iterator>
#include <string>

#include "core/result.h"
#include "shell/adjust_pane.h"
#include "shell/commands.h"
#include "shell/key_router.h"

namespace mv::shell {

// Commands the island posts back. The C# side uses the same integers.
enum chrome_command : int {
  chrome_cmd_open = 1,         // media picker: photos and clips
  chrome_cmd_fit = 2,
  chrome_cmd_one_to_one = 3,
  chrome_cmd_zoom_in = 4,
  chrome_cmd_zoom_out = 5,
  chrome_cmd_zoom_preset = 6,  // arg is the zoom factor (0.5, 1, 2, 4)
  chrome_cmd_overlay = 7,
  chrome_cmd_select_item = 8,  // arg is the folder index
  chrome_cmd_prev = 9,
  chrome_cmd_next = 10,
  chrome_cmd_open_folder = 11,     // folder picker
  chrome_cmd_toggle_gallery = 12,
  chrome_cmd_close_gallery = 13,
  chrome_cmd_gallery_activate = 14,  // arg is the folder index: select and leave the gallery
  chrome_cmd_set_settings = 15,      // arg is a view_settings flag word
  chrome_cmd_folder_ready = 16,      // arg is the item count the island just listed
  chrome_cmd_toggle_filmstrip = 17,
  chrome_cmd_video_active = 18,      // arg != 0 while a clip is open: show the transport
  chrome_cmd_set_rate = 19,          // arg is the playback rate the dropdown picked
  chrome_cmd_focus_changed = 20,     // arg is a focus_kind (key_router.h): which island, or text
  // Island notifications added after the command table filled the low ids
  // live above every command id, so the two ranges never meet.
  chrome_cmd_popup = 1000,           // arg != 0 while a `?` / go-to / find flyout is up
  chrome_cmd_rebind = 1001,          // packed: row | (key << 8) | (mods << 20)
  chrome_cmd_reset_keys = 1002,      // restore the default map
  // PR 8 updater. Chrome, not a command: no key, no row in `?` or Settings
  // (docs/design/16 — an update affordance is chrome). arg 0: the user clicked
  // "Update ready — restart"; arg 1: Update.exe is armed, close now; arg 2:
  // an add-on update waits on a restart (through Update.exe when an app
  // update is staged too, else MediaViewer starts again after it exits).
  chrome_cmd_update_restart = 1003,
  // PR 9. tree_open: the user chose a folder in the tree; native pulls the path
  // with take_tree_path (the callback carries only a float). set_sort: arg is the
  // packed sort order (io/sort_order.h pack_sort).
  chrome_cmd_tree_open = 1004,
  chrome_cmd_set_sort = 1005,
  // PR 10. The export dialog was confirmed; arg is the packed choice
  // (edit_session.h pack_export). Cancel sends nothing.
  chrome_cmd_export = 1006,
  // PR 26 folder tiles. Chrome, not keyed commands: the gallery posts these.
  chrome_cmd_open_subfolder = 1007,   // arg is the child-folder index
  chrome_cmd_open_crumb = 1008,       // arg is the breadcrumb index
  chrome_cmd_gallery_columns = 1009,  // arg is cells per row
  // Milestone G. arg 1: the Import add-on is installed and loaded, so its
  // commands exist (docs/design/18 "Commands"); arg 0: it is not.
  chrome_cmd_addon_state = 1010,
  // Milestone G. Open a file in the viewer (Import's Enter); native pulls the
  // path with take_tree_path, as for the tree.
  chrome_cmd_open_path = 1011,
  // PR 12. The metadata pane's comment field was committed (Return or leaving
  // it); native pulls the text with take_parked_text. Revert puts the fields
  // back to the session's first snapshot. The stars post set_rating_0..5.
  chrome_cmd_meta_comment = 1012,
  chrome_cmd_meta_revert = 1013,
  // PR 14. The clip tools flyout was confirmed; arg is the packed choice
  // (shell/trim_state.h pack_clip_choice). Cancel sends nothing.
  chrome_cmd_clip_tool = 1014,
  // PR 13. The island drained MV_COMPLETION_CLIP_INDEX (the island owns the
  // drain); arg is the request id, which native reads with mv_clip_index_get.
  chrome_cmd_clip_index = 1015,
  // Appearance only: WinUI sends the system window colour as an exact
  // 24-bit integer carried by float (all integers up to 2^24 are exact).
  chrome_cmd_home_colour = 1016,
  // Issue #38: arg != 0 while the transport is held open by something the
  // host cannot see from the canvas -- a scrub drag, the More flyout, a
  // dropdown. Release restarts the idle clock.
  chrome_cmd_transport_hold = 1017,
  // The transport row's natural width in DIPs (it grows while trim is armed).
  // Native sizes the bar to it, so the island hugs its controls instead of
  // being a long empty box around them.
  chrome_cmd_transport_width = 1018,
  // PR 29 (docs/design/20): the Edit workspace (moved up one again, 2026-09-27, when
  // main's transport_width took 1018). edit_tab: arg is the shell::edit_tab
  // the strip's tab row picked. edit_action: arg is a chrome_edit_action.
  chrome_cmd_edit_tab = 1019,
  chrome_cmd_edit_action = 1020,
  // PR 29 (owner, 2026-09-26): every tag editable. meta_tags: native pulls
  // the parked edits with take_parked_text, one per line: "S\tkey\tvalue"
  // sets, "R\tkey" removes. meta_date: the parked "YYYY-MM-DD HH:MM:SS" goes
  // into every capture-time tag; arg 1 removes them all (nothing parked).
  chrome_cmd_meta_tags = 1021,
  chrome_cmd_meta_date = 1022,
  // PR 30 (docs/design/21): the Video Editor window's timeline. editor_seek: arg is
  // the program time in milliseconds (exact to 4.6 h in a float), and selects
  // the piece there; editor_action: arg is a chrome_editor_action.
  chrome_cmd_editor_seek = 1023,
  chrome_cmd_editor_action = 1024,
  // Drag-out (the Mac's FolderStore.dragFiles). drag_items: a gallery or
  // filmstrip cell started a drag; arg is its folder index. Native answers at
  // once, inside the call, with set_drag_paths: every marked item in listing
  // order, pairs expanded, when that cell is marked; else nothing, and the
  // cell drags itself (and its pair).
  // drag_ended: that drag finished (dropped or cancelled). While one of our
  // own drags is in flight, our own drop targets refuse it.
  chrome_cmd_drag_items = 1025,
  chrome_cmd_drag_ended = 1026,
  // Open > Recent folders (the Mac's File > Open Recent): arg is the row in
  // the list set_recent_folders last pushed. A folder that is gone beeps and
  // leaves the list, as a welcome-card click does.
  chrome_cmd_open_recent = 1027,
  // A piece's edge dragged on the timeline. trim_grab: arg is index * 2 +
  // edge (0 in, 1 out), or -1 to let go; trim_to: arg is the edge's new
  // source time in milliseconds (exact to 4.6 h in a float). One undo a drag.
  chrome_cmd_editor_trim_grab = 1028,
  chrome_cmd_editor_trim_to = 1029,
  // A document's "Open in <app>" (docs/design/20): arg is the row of the apps
  // set_edit_view last listed, or -1 for the default.
  chrome_cmd_open_in_app = 1030,
  // Issue #214: the transport's More flyout. Native owns volume, mute, the
  // audio track and the A-B loop, as it owns the rate, so the flyout and the
  // keys (Up / Down, Shift+M, trim's P) cannot drift; native pushes the result
  // back with apply_audio. video_volume: arg is 0..1. video_muted: arg != 0
  // mutes. video_track: arg is the audio track index. video_loop: arg is a
  // chrome_loop_action.
  chrome_cmd_video_volume = 1031,
  chrome_cmd_video_muted = 1032,
  chrome_cmd_video_track = 1033,
  chrome_cmd_video_loop = 1034,
};

// chrome_cmd_video_loop's argument. The C# side mirrors it.
enum class chrome_loop_action : std::int32_t {
  set_a = 0,  // remember the playhead as A
  set_b = 1,  // loop A..playhead (A and B are normalised by the player)
  clear = 2,
};

// chrome_cmd_editor_action's argument; 1-6 are mv_chrome_editor_edit's codes
// on the Mac bridge. The C# side mirrors it.
enum class chrome_editor_action : std::int32_t {
  split = 1,         // at the playhead
  remove = 2,        // the selected piece
  set_in = 3,        // cut everything before the playhead
  set_out = 4,       // cut everything after it
  undo = 5,
  redo = 6,
  toggle_play = 7,
  step_back = 8,     // a frame
  step_forward = 9,
  export_keyframe = 10,  // keep_ranges, cut on keyframes (instant)
  export_exact = 11,     // keep_ranges, re-encoded (frame-accurate)
  close = 12,        // asks first when the edit has not been exported
  show = 13,         // the viewer's "Editing in the Video Editor" card: raise the window
  mark_in = 14,      // I: marks, cuts nothing (the Mac bridge's codes too)
  mark_out = 15,     // O
  clear_marks = 16,  // X
};

// chrome_cmd_edit_action's argument. The C# side mirrors it.
enum class chrome_edit_action : std::int32_t {
  cancel_crop = 0,   // the Crop pane's Cancel: drop the draft
  original_off = 1,  // the strip's Original toggle
  original_on = 2,
  save_copy = 3,     // applies a crop draft, then the PR 10 export dialog
};

static_assert(chrome_cmd_popup >= kCommandCount);

// Which flyout ShowPopup opens (0 closes whatever is up). The C# side mirrors it.
enum class chrome_popup : std::int32_t {
  close = 0,
  help = 1,
  palette = 2,  // unused; keep the value so go_to / find / settings stay put
  go_to = 3,
  find = 4,
  settings = 5,
  export_image = 6,  // PR 10; mode_mask carries the last packed choice to preselect
  clip_tools = 7,    // PR 14; mode_mask carries clip flags (trim_state.h kClipFlag*)
};

struct chrome_popup_args {
  std::int32_t kind;       // chrome_popup
  std::int32_t mode_mask;  // `?` lists the bindings live in these modes
};

static_assert(sizeof(chrome_popup_args) == 8, "keep in sync with ChromePopupArgs");

// File search (docs/design/16 "File search", 2026-09-28): find by name over the
// gallery, base app, no index. The island answers whether it took the key;
// if not, native does what it did without it. Mirrored by
// IslandHost.FileSearch.cs (entry GallerySearch).
enum class gallery_search_action : std::int32_t {
  open = 0,    // Ctrl+F without the Local search command: see gallery_search_answer
  escape = 1,  // Esc while a text control has focus: clear the field, then close it
  step = 2,    // Left / Right (arg -1 / +1): walk the name filter's matches
};
enum class gallery_search_answer : std::int32_t {
  took = 0,           // done: the field has the keyboard, or Local search's panel opens
  declined = 1,       // not taken (also: no chrome entry, nothing listed)
  needs_gallery = 2,  // open: show the gallery; the field opens once it is shown
};

struct chrome_table_args {
  std::uint64_t utf8;  // describe_commands() text, valid for the call only
  std::int32_t length;
  std::int32_t reserved;
};

static_assert(sizeof(chrome_table_args) == 16, "keep in sync with ChromeTableArgs");

// Commands and island notifications share one id space (docs/design/16). These pin
// the values the island already sends; commands.h reserves the notifications.
static_assert(chrome_cmd_open == static_cast<int>(command_id::open));
static_assert(chrome_cmd_fit == static_cast<int>(command_id::fit));
static_assert(chrome_cmd_one_to_one == static_cast<int>(command_id::one_to_one));
static_assert(chrome_cmd_zoom_in == static_cast<int>(command_id::zoom_in));
static_assert(chrome_cmd_zoom_out == static_cast<int>(command_id::zoom_out));
static_assert(chrome_cmd_zoom_preset == static_cast<int>(command_id::zoom_preset));
static_assert(chrome_cmd_overlay == static_cast<int>(command_id::overlay));
static_assert(chrome_cmd_select_item == static_cast<int>(command_id::select_item));
static_assert(chrome_cmd_prev == static_cast<int>(command_id::prev));
static_assert(chrome_cmd_next == static_cast<int>(command_id::next));
static_assert(chrome_cmd_open_folder == static_cast<int>(command_id::open_folder));
static_assert(chrome_cmd_toggle_gallery == static_cast<int>(command_id::toggle_gallery));
static_assert(chrome_cmd_close_gallery == static_cast<int>(command_id::close_gallery));
static_assert(chrome_cmd_gallery_activate == static_cast<int>(command_id::gallery_activate));
static_assert(chrome_cmd_toggle_filmstrip == static_cast<int>(command_id::toggle_filmstrip));
// The panes' close buttons and the View menu send these keyed commands.
static_assert(static_cast<int>(command_id::folder_tree) == 78);
static_assert(static_cast<int>(command_id::metadata_pane) == 92);
static_assert(static_cast<int>(command_id::folder_up) == 115);
// PR 11: the adjust pane's close button and its sliders (arg = the value).
static_assert(static_cast<int>(command_id::adjust_pane) == 118);
static_assert(static_cast<int>(command_id::adjust_exposure) == 119);
static_assert(static_cast<int>(command_id::adjust_tint) == 123);
static_assert(static_cast<int>(command_id::adjust_reset) == 124);
static_assert(static_cast<int>(command_id::set_rating_0) == 127);
static_assert(static_cast<int>(command_id::set_rating_5) == 132);
static_assert(chrome_cmd_tree_open >= kCommandCount && chrome_cmd_set_sort >= kCommandCount);
static_assert(chrome_cmd_export >= kCommandCount);
static_assert(chrome_cmd_open_subfolder >= kCommandCount && chrome_cmd_gallery_columns >= kCommandCount);
static_assert(chrome_cmd_addon_state >= kCommandCount);
static_assert(chrome_cmd_open_path >= kCommandCount);
static_assert(chrome_cmd_meta_comment >= kCommandCount && chrome_cmd_meta_revert >= kCommandCount);
static_assert(chrome_cmd_clip_tool >= kCommandCount && chrome_cmd_clip_index >= kCommandCount);
// PR 13 / 14: the jobs pane's close button and the trim chips send these.
static_assert(static_cast<int>(command_id::trim_mode) == 134);
static_assert(static_cast<int>(command_id::trim_keyframe) == 139);
static_assert(static_cast<int>(command_id::trim_reencode) == 140);
static_assert(static_cast<int>(command_id::jobs_pane) == 143);
static_assert(static_cast<int>(command_id::clip_tools) == 144);
// PR 29: the Edit workspace's buttons send these (the Mac bridge pins 150 too; PR 15 took 147-149).
static_assert(static_cast<int>(command_id::edit_workspace) == 150);
static_assert(static_cast<int>(command_id::rotate_ccw) == 96 && static_cast<int>(command_id::rotate_cw) == 97);
static_assert(static_cast<int>(command_id::flip_horizontal) == 98 &&
              static_cast<int>(command_id::flip_vertical) == 99);
static_assert(static_cast<int>(command_id::crop_mode) == 100 && static_cast<int>(command_id::crop_commit) == 101);
static_assert(static_cast<int>(command_id::undo_edit) == 113 && static_cast<int>(command_id::reset_edits) == 114);
static_assert(static_cast<int>(command_id::trim_in) == 135 && static_cast<int>(command_id::trim_out) == 136);
static_assert(static_cast<int>(command_id::trim_clear) == 137 && static_cast<int>(command_id::trim_preview) == 138);
static_assert(static_cast<int>(command_id::clip_split) == 145 &&
              static_cast<int>(command_id::trim_remove_middle) == 146);
static_assert(static_cast<int>(command_id::crop_aspect_set) == 155);
static_assert(static_cast<int>(command_id::crop_straighten_set) == 156);
// docs/design/07 "Print": the Open flyout's Print… (IslandHost.cs Command.Print).
static_assert(static_cast<int>(command_id::print) == 172);
static_assert(chrome_cmd_edit_tab >= kCommandCount && chrome_cmd_edit_action >= kCommandCount);
static_assert(chrome_cmd_meta_tags >= kCommandCount && chrome_cmd_meta_date >= kCommandCount);
static_assert(chrome_cmd_editor_seek >= kCommandCount && chrome_cmd_editor_action >= kCommandCount);
static_assert(chrome_cmd_drag_items >= kCommandCount && chrome_cmd_drag_ended >= kCommandCount);
static_assert(chrome_cmd_open_recent >= kCommandCount);
static_assert(chrome_cmd_editor_trim_grab >= kCommandCount && chrome_cmd_editor_trim_to >= kCommandCount);
static_assert(chrome_cmd_open_in_app >= kCommandCount);
static_assert(chrome_cmd_video_volume >= kCommandCount && chrome_cmd_video_loop >= kCommandCount);
static_assert(is_reserved_notification(chrome_cmd_set_settings));
static_assert(is_reserved_notification(chrome_cmd_folder_ready));
static_assert(is_reserved_notification(chrome_cmd_video_active));
static_assert(is_reserved_notification(chrome_cmd_set_rate));
static_assert(is_reserved_notification(chrome_cmd_focus_changed));

// The island's Command constants, hashed in declaration order. IslandHost.Probe
// computes the same over its own constants; a drift on either side fails
// tests/test_chrome_host.cpp instead of a menu item that runs the wrong thing.
[[nodiscard]] constexpr std::int32_t chrome_command_checksum() noexcept {
  constexpr int ids[] = {
      chrome_cmd_open, chrome_cmd_fit, chrome_cmd_one_to_one, chrome_cmd_zoom_in,
      chrome_cmd_zoom_out, chrome_cmd_zoom_preset, chrome_cmd_overlay, chrome_cmd_select_item,
      chrome_cmd_prev, chrome_cmd_next, chrome_cmd_open_folder, chrome_cmd_toggle_gallery,
      chrome_cmd_close_gallery, chrome_cmd_gallery_activate, chrome_cmd_set_settings,
      chrome_cmd_folder_ready, chrome_cmd_toggle_filmstrip, chrome_cmd_video_active,
      chrome_cmd_set_rate, chrome_cmd_focus_changed, chrome_cmd_popup, chrome_cmd_rebind,
      chrome_cmd_reset_keys, chrome_cmd_update_restart, chrome_cmd_tree_open,
      chrome_cmd_set_sort, chrome_cmd_export, chrome_cmd_open_subfolder, chrome_cmd_open_crumb,
      chrome_cmd_gallery_columns,
      chrome_cmd_addon_state, chrome_cmd_open_path, chrome_cmd_meta_comment,
      chrome_cmd_meta_revert, chrome_cmd_clip_tool, chrome_cmd_clip_index,
      chrome_cmd_edit_tab, chrome_cmd_edit_action, chrome_cmd_meta_tags, chrome_cmd_meta_date,
      chrome_cmd_editor_seek, chrome_cmd_editor_action,
      chrome_cmd_drag_items, chrome_cmd_drag_ended, chrome_cmd_open_recent,
      chrome_cmd_editor_trim_grab, chrome_cmd_editor_trim_to, chrome_cmd_open_in_app,
      chrome_cmd_video_volume, chrome_cmd_video_muted, chrome_cmd_video_track,
      chrome_cmd_video_loop};
  std::uint32_t h = 17;
  for (const int id : ids) h = h * 31u + static_cast<std::uint32_t>(id);
  return static_cast<std::int32_t>(h);
}

using chrome_command_fn = void (*)(void* context, int command, float arg);

// Blittable attach payload. Layout is asserted against the C# struct; a silent
// drift here is a function-pointer read as a HWND.
struct chrome_attach_args {
  std::uint64_t parent_hwnd;
  std::uint64_t context;
  std::uint64_t on_command;
  std::int32_t  client_width;
  std::int32_t  client_height;
  std::int32_t  dpi;
  std::int32_t  reserved;
};

static_assert(sizeof(chrome_attach_args) == 40, "keep in sync with ChromeAttachArgs");

struct chrome_resize_args {
  std::int32_t width;
  std::int32_t height;
  std::int32_t dpi;
  std::int32_t y;  // client-pixel top of the island (0 for the command bar)
};

static_assert(sizeof(chrome_resize_args) == 16, "keep in sync with ChromeResizeArgs");

struct chrome_filmstrip_args {
  std::uint64_t parent_hwnd;
  std::uint64_t context;
  std::uint64_t on_command;
  std::uint64_t session;  // borrowed mv_session_t
  std::int32_t  client_width;
  std::int32_t  client_height;
  std::int32_t  dpi;
  std::int32_t  reserved;
};

static_assert(sizeof(chrome_filmstrip_args) == 48, "keep in sync with ChromeFilmstripArgs");

// Show/hide plus the geometry for the state being entered. Native owns the
// layout maths for both states so the island never has to guess where
// "offscreen" is: hiding moves the bridge below the client area, which is the
// one place a stray child window cannot eat a click meant for the canvas.
struct chrome_show_args {
  std::int32_t visible;
  std::int32_t width;
  std::int32_t height;
  std::int32_t y;
};

static_assert(sizeof(chrome_show_args) == 16, "keep in sync with ChromeShowArgs");

// PR 9 panes. Native owns the geometry: x/y/width/height are client pixels, and a
// hidden pane is parked below the client area (the gallery's rule).
struct chrome_panel_args {
  std::int32_t visible;
  std::int32_t x;
  std::int32_t y;
  std::int32_t width;
  std::int32_t height;
  std::int32_t focus;  // non-zero: move keyboard focus into the pane (I, Ctrl+Shift+E)
};

static_assert(sizeof(chrome_panel_args) == 24, "keep in sync with ChromePanelArgs");

// PR 13: trim mode on the transport's scrub bar (shell/trim_state.h). Times in
// ns on the player's timeline; -1 = unset. `cut_*` is what Path 1 will write
// (the keyframe-snapped range). Pointers are valid for the call only.
struct chrome_trim_args {
  std::int32_t armed;
  std::int32_t index_ready;
  std::int64_t duration_ns;
  std::int64_t in_ns;
  std::int64_t out_ns;
  std::int64_t cut_in_ns;
  std::int64_t cut_out_ns;
  std::uint64_t keyframes;  // const int64_t*
  std::int32_t keyframe_count;
  std::int32_t label_len;
  std::uint64_t label_utf8;  // trim_state::label()
  std::int32_t previewing;
  std::int32_t reserved;
};

static_assert(sizeof(chrome_trim_args) == 80, "keep in sync with ChromeTrimArgs");

// The metadata pane's three tables (meta/tables.h), valid for the call only.
struct chrome_meta_args {
  std::uint64_t summary;
  std::uint64_t properties;
  std::uint64_t streams;
  std::int32_t summary_len;
  std::int32_t properties_len;
  std::int32_t streams_len;
  std::int32_t loading;
};

static_assert(sizeof(chrome_meta_args) == 40, "keep in sync with ChromeMetaArgs");

// PR 12: what the pane's rating, comment and Revert show. A change still in the
// write queue already counts, so a key or a click shows at once.
struct chrome_meta_edit_args {
  std::uint64_t comment;     // UTF-8, valid for the call only
  std::int32_t comment_len;
  std::int32_t rating;       // -1 rejected, 0 none, 1..5
  std::int32_t flags;        // kMetaEdit*
  std::int32_t reserved;
};

static_assert(sizeof(chrome_meta_edit_args) == 24, "keep in sync with ChromeMetaEditArgs");

// PR 29 (docs/design/20): what the Edit workspace's strip, its Crop / Trim pane and
// the command bar's Edit button show -- the Mac bridge's mv_edit_view plus the
// item's name and the trim state. Pointers are valid for the call only.
struct chrome_edit_args {
  std::int32_t open;           // the workspace is up
  std::int32_t tab;            // shell::edit_tab
  std::int32_t subject;        // shell::edit_subject: 0 none, 1 still, 2 clip
  std::int32_t crop_active;    // a crop draft is on the canvas
  std::int32_t aspect;         // shell::crop_aspect
  std::int32_t portrait;       // the locked preset is portrait
  float straighten;            // degrees: the draft's angle, else the committed one
  std::int32_t edit_count;     // ops on the item's stack
  std::int32_t show_original;  // Y held, or the strip's Original toggle
  std::int32_t crop_width;     // what Apply would keep, in pixels (0 = not known yet)
  std::int32_t crop_height;
  std::int32_t trim_flags;     // kEditTrim*
  std::uint64_t name_utf8;     // the item's file name, display only
  std::uint64_t trim_label_utf8;  // trim_state::label() while armed
  std::int32_t name_len;
  std::int32_t trim_label_len;
  // A PDF or DOCX: nothing to edit; the bar offers "Open in <app>" instead.
  // open_apps: the apps' names, one a line, default first ("" until known).
  std::uint64_t open_apps_utf8;
  std::int32_t open_apps_len;
  std::int32_t document;
};

static_assert(sizeof(chrome_edit_args) == 88, "keep in sync with IslandHost.Edit EditArgsSize");

// PR 30 (docs/design/21): the Video Editor window. Its timeline is an island on the
// editor's own top-level window; the card that says where the picture went is
// an island on the viewer's. Native owns both rects, as for the panes.
struct chrome_editor_attach_args {
  std::uint64_t editor_hwnd;  // the Video Editor window
  std::uint64_t viewer_hwnd;  // the viewer's window, for the "away" card
};

static_assert(sizeof(chrome_editor_attach_args) == 16, "keep in sync with IslandHost.VideoEditor");

struct chrome_editor_layout_args {
  std::int32_t x;  // the timeline, in the editor's client pixels
  std::int32_t y;
  std::int32_t width;
  std::int32_t height;
  std::int32_t away_visible;  // the card, in the viewer's client pixels
  std::int32_t away_x;
  std::int32_t away_y;
  std::int32_t away_width;
  std::int32_t away_height;
  std::int32_t focus;  // non-zero: move keyboard focus into the timeline
};

static_assert(sizeof(chrome_editor_layout_args) == 40, "keep in sync with IslandHost.VideoEditor");

// What the timeline shows: the Mac bridge's mv_editor_view, plus the pieces and
// the name. `generation` bumps on every edit; between edits only the playhead
// and `playing` move. Pointers are valid for the call only.
struct chrome_editor_view_args {
  std::int32_t open;
  std::int32_t ready;          // the clip is probed; the strip may still be empty
  std::int64_t length_ns;      // the program
  std::int64_t playhead_ns;    // on the program
  std::int64_t source_ns;      // the clip
  std::int32_t playing;
  std::int32_t piece_count;
  std::int32_t selected;       // -1 none
  std::int32_t can_undo;
  std::int32_t can_redo;
  std::int32_t edited;         // anything cut: Export has something to write
  std::uint64_t generation;
  std::uint64_t pieces;        // const int64_t*: (in, out) source pairs, program order
  std::uint64_t name_utf8;
  std::int32_t name_len;
  std::int32_t frame_rate_milli;  // the clip's rate x 1000 (29970); 0 unknown
  std::int64_t mark_in_ns;        // the marked range on the program; -1 unset
  std::int64_t mark_out_ns;
};

static_assert(sizeof(chrome_editor_view_args) == 104, "keep in sync with IslandHost.VideoEditor");

// One timeline thumbnail (edit/clip_strip.h strip_frame): RGBA8, sRGB.
struct chrome_editor_thumb {
  std::int64_t shown_ns;  // the frame's own source time
  std::int32_t width;
  std::int32_t height;
  std::uint64_t rgba;     // const uint8_t*, width * height * 4
};

static_assert(sizeof(chrome_editor_thumb) == 24, "keep in sync with IslandHost.VideoEditor");

// The strip and the waveform, pushed once per clip. Valid for the call only.
struct chrome_editor_strip_args {
  std::uint64_t thumbs;  // const chrome_editor_thumb*
  std::int32_t thumb_count;
  std::int32_t peak_count;
  std::uint64_t peaks;   // const float*, 0..1 across the source
};

static_assert(sizeof(chrome_editor_strip_args) == 24, "keep in sync with IslandHost.VideoEditor");

inline constexpr std::int32_t kEditTrimArmed = 1;
inline constexpr std::int32_t kEditTrimPreviewing = 2;
inline constexpr std::int32_t kEditTrimHasMarker = 4;

inline constexpr std::int32_t kMetaEditCanEdit = 1;    // an item is open
inline constexpr std::int32_t kMetaEditCanRevert = 2;  // a write to it landed this session
inline constexpr std::int32_t kMetaEditFocus = 4;      // Ctrl+I: the comment field takes the keyboard
inline constexpr std::int32_t kMetaEditDropDraft = 8;  // Esc: forget what was typed, show the file's

struct chrome_flags_args {
  std::int32_t flags;
  std::int32_t sort;  // PR 9: packed sort order, so Settings and View > Sort by show the truth
};

static_assert(sizeof(chrome_flags_args) == 8, "keep in sync with ChromeFlagsArgs");

// Native owns the playback rate: the keyboard is the only router (docs/design/16), so
// the dropdown is a view of the rate rather than a second place it is decided.
// Same one-direction rule as the settings flags — the menu changes only after
// native has applied the change.
struct chrome_rate_args {
  float        rate;
  std::int32_t reserved;
};

static_assert(sizeof(chrome_rate_args) == 8, "keep in sync with ChromeRateArgs");

// Issue #214: the same rule for the More flyout's volume, mute and audio track.
struct chrome_audio_args {
  float        volume;  // 0..1
  std::int32_t muted;   // non-zero = muted
  std::int32_t track;   // audio track index
  std::int32_t reserved;
};

static_assert(sizeof(chrome_audio_args) == 16, "keep in sync with ChromeAudioArgs");

struct chrome_navigate_args {
  std::int32_t reverse;  // non-zero = Shift+Tab
  std::int32_t reserved;
};

// PR 26: breadcrumb trail + gallery folder-tile cursor. crumbs_utf8 is
// "name\tpath\n" lines, valid for the call only (same rule as the command table).
// query_bytes is -1 when folder-find is idle; 0 or more is the live query.
struct chrome_browse_args {
  std::int32_t folder_cursor;  // -1 = on images
  std::int32_t can_go_up;
  std::int32_t crumbs_bytes;
  std::int32_t query_bytes;
  std::uint64_t crumbs_utf8;
  std::uint64_t query_utf8;
};

static_assert(sizeof(chrome_browse_args) == 32, "keep in sync with ChromeBrowseArgs");

using chrome_entry_fn = int (*)(void* arg, std::int32_t arg_size_in_bytes);

// DIP height of the command-bar strip. Physical pixels = this * dpi / 96.
inline constexpr int kChromeBarDip = 48;
inline constexpr int kFilmstripDip = 112;
// The transport bar. Issue #38 (docs/design/12 2026-09-26): it floats over the bottom
// of the video, centred and above the filmstrip, like the Mac's, and leaves
// after an idle interval while the clip plays (shell/transport_autohide.h). It
// no longer reserves canvas, so showing or hiding it never refits the video.
inline constexpr int kTransportDip = 52;
// The bar hugs its controls (chrome_cmd_transport_width); this is only its width
// before the island has reported one.
inline constexpr int kTransportMaxWidthDip = 880;
inline constexpr int kTransportPadDip = 14;     // each side, between the bar's edge and its controls
inline constexpr int kTransportMarginDip = 10;  // above the filmstrip / bottom edge
inline constexpr int kTransportSideDip = 16;    // minimum gap to the window's sides

[[nodiscard]] inline int chrome_bar_height_px(std::uint32_t dpi) noexcept {
  if (dpi == 0) dpi = 96;
  return static_cast<int>((kChromeBarDip * static_cast<int>(dpi) + 48) / 96);
}

[[nodiscard]] inline int chrome_filmstrip_height_px(std::uint32_t dpi) noexcept {
  if (dpi == 0) dpi = 96;
  return static_cast<int>((kFilmstripDip * static_cast<int>(dpi) + 48) / 96);
}

[[nodiscard]] inline int chrome_transport_height_px(std::uint32_t dpi) noexcept {
  if (dpi == 0) dpi = 96;
  return static_cast<int>((kTransportDip * static_cast<int>(dpi) + 48) / 96);
}

// IslandWindow ids of the islands that classify as focus_kind::pane, past the
// focus kinds: metadata, folder tree (PR 9), adjust (PR 11), Edit workspace
// (PR 29), Jobs (PR 13). The C# side numbers them the same (IslandHost.cs).
// A pane missing here falls through to command_bar and the router runs viewer
// commands on keys meant for it (issue #213).
inline constexpr std::int32_t kPaneIslandIds[] = {6, 7, 8, 9, 10};
inline constexpr int kPaneIslandCount = static_cast<int>(std::size(kPaneIslandIds));

// classify_focus over explicit bridge windows. `islands` is indexed by
// focus_kind [command_bar .. transport] (index 0 unused); `panes` are the pane
// islands' roots. Split out so a host test can drive it with plain HWNDs.
[[nodiscard]] focus_kind classify_island_focus(HWND focus, HWND canvas, const HWND* islands,
                                               int island_count, const HWND* panes,
                                               int pane_count) noexcept;

class chrome_host {
 public:
  chrome_host() = default;
  ~chrome_host();

  chrome_host(const chrome_host&) = delete;
  chrome_host& operator=(const chrome_host&) = delete;

  // Finds MediaViewer.Chrome.dll beside this exe, starts the runtime, and
  // resolves the entry points. Safe to call when the dll is missing — returns
  // an error and leaves the host idle.
  [[nodiscard]] expected load() noexcept;

  [[nodiscard]] bool loaded() const noexcept { return probe_ != nullptr; }
  [[nodiscard]] bool attached() const noexcept { return attached_; }

  // Returns the C# sizeof(ChromeAttachArgs) so a layout drift fails a test
  // rather than a window that never appears.
  [[nodiscard]] int probe() const noexcept;

  // The island's command-id checksum (chrome_command_checksum), or 0.
  [[nodiscard]] std::int32_t probe_commands() const noexcept;

  // Caches each island's bridge HWND. Call after the islands attach; it is one
  // managed hop per island, never per key.
  void refresh_island_windows() noexcept;

  // Which island holds `focus`. The canvas only when `focus` is the canvas
  // window itself: a flyout's popup, a null or a foreign HWND is an island.
  [[nodiscard]] focus_kind classify_focus(HWND focus, HWND canvas) const noexcept;

  // True when the cursor is over a visible island's bridge window. Cheap
  // (GetCursorPos + GetWindowRect); asked by a timer, never per mouse-move.
  [[nodiscard]] bool cursor_over_island() const noexcept;
  // The same, for the transport bar alone (issue #38: hovering it holds it up).
  [[nodiscard]] bool cursor_over_transport() const noexcept;

  // `parent` is the top-level canvas HWND. The island is MoveAndResize'd into
  // the 48 DIP strip so flyouts are siblings of the swapchain, not clipped by
  // a short child window.
  [[nodiscard]] expected attach(HWND parent, void* context, chrome_command_fn on_command,
                                int width, int height, std::uint32_t dpi) noexcept;

  void resize(int width, int height, std::uint32_t dpi) noexcept;

  // Fullscreen hides the command bar: move its bridge below the client area,
  // the same "offscreen" the other strips park in. resize() brings it back.
  void park_bar(int client_height) noexcept;

  [[nodiscard]] expected attach_filmstrip(HWND parent, void* context, chrome_command_fn on_command,
                                          void* session, int width, int height,
                                          std::uint32_t dpi) noexcept;
  void resize_filmstrip(int width, int client_height, std::uint32_t dpi) noexcept;
  [[nodiscard]] bool filmstrip_attached() const noexcept { return filmstrip_attached_; }

  // The strip stays attached when hidden: it owns the completion drain that
  // feeds both it and the gallery, so tearing it down to hide 112 DIP would
  // also stop the folder listening ([12](12-decision-log.md) 2026-09-07).
  void show_filmstrip(bool visible, int width, int client_height, std::uint32_t dpi) noexcept;
  [[nodiscard]] bool filmstrip_visible() const noexcept {
    return filmstrip_attached_ && filmstrip_visible_;
  }

  // Full-client thumbnail grid, below the command bar. Same island model as
  // the filmstrip — chrome over the swapchain, never a XAML canvas (rule 2).
  [[nodiscard]] expected attach_gallery(HWND parent, void* context, chrome_command_fn on_command,
                                        void* session, int width, int height,
                                        std::uint32_t dpi) noexcept;
  void resize_gallery(int width, int client_height, std::uint32_t dpi) noexcept;
  void show_gallery(bool visible, int width, int client_height, std::uint32_t dpi) noexcept;
  [[nodiscard]] bool gallery_attached() const noexcept { return gallery_attached_; }
  [[nodiscard]] bool gallery_visible() const noexcept {
    return gallery_attached_ && gallery_visible_;
  }

  // PR 9: the metadata pane (right) and folder tree (left), two islands that
  // float over the canvas and never inset it. Optional: an older chrome without
  // them still loads, and every call below is then a no-op.
  [[nodiscard]] expected attach_panels(HWND parent, void* context, chrome_command_fn on_command,
                                       void* session, int width, int height,
                                       std::uint32_t dpi) noexcept;
  [[nodiscard]] bool panels_attached() const noexcept { return panels_attached_; }
  void show_meta_pane(bool visible, int x, int y, int width, int height,
                      bool focus = false) noexcept;
  void show_folder_tree(bool visible, int x, int y, int width, int height,
                        bool focus = false) noexcept;
  [[nodiscard]] bool meta_pane_visible() const noexcept { return panels_attached_ && meta_visible_; }
  [[nodiscard]] bool folder_tree_visible() const noexcept { return panels_attached_ && tree_visible_; }
  // The record's tables, formatted once by the host; the pane re-renders from them.
  void set_meta_data(bool loading, const std::string& summary, const std::string& properties,
                     const std::string& streams) noexcept;
  void set_tree_root(const std::string& utf8_dir) noexcept;
  // The folder the user chose in the tree ("" if none pending).
  [[nodiscard]] std::string take_tree_path() noexcept;
  // The string the island parked for the last notification that carries one
  // (the tree, Import's open, PR 12's comment). False when nothing could be
  // read, which is not the same as an empty string: "" clears a comment.
  [[nodiscard]] bool take_parked_text(std::string& out) noexcept;
  // PR 12: the pane's rating / comment / Revert state (chrome_meta_edit_args).
  void set_meta_edit(std::int32_t rating, const std::string& comment, std::int32_t flags) noexcept;

  // PR 11: the adjust pane, a third panel island on the right (it and the
  // metadata pane share that edge; the host shows one at a time). Optional
  // like the other panes: an older chrome without it makes these no-ops.
  void show_adjust_pane(bool visible, int x, int y, int width, int height,
                        bool focus = false) noexcept;
  [[nodiscard]] bool adjust_pane_visible() const noexcept {
    return panels_attached_ && adjust_visible_;
  }
  // Readiness, slider values, histogram and clipping (shell/adjust_pane.h),
  // as one blittable struct; the pane re-renders from it.
  // Shown with `focus`, the first slider takes the keyboard (Shift+A); the
  // island reports it as pane focus, so the arrows are the slider's and Esc
  // returns to the canvas, like the metadata pane.
  void set_adjust_view(const adjust_view& view) noexcept;

  // PR 13 / 14: the Jobs pane, a fourth panel island on the right edge (the
  // host shows one right pane at a time). The pane polls the clip job queue
  // through the ABI itself. Optional like the other panes.
  void show_jobs_pane(bool visible, int x, int y, int width, int height, bool focus = false) noexcept;
  [[nodiscard]] bool jobs_pane_visible() const noexcept { return panels_attached_ && jobs_visible_; }
  // PR 13: trim mode's markers, keyframe grid and label on the scrub bar.
  // Optional: an older chrome ignores it.
  void set_trim(const chrome_trim_args& args) noexcept;

  // PR 29 (docs/design/20): the Edit workspace, a fifth panel island at the top of
  // the right column: its strip, and under it the Crop or Trim pane (the
  // Colour / Info / Jobs tabs are the panes above, placed under the strip by
  // the host). Optional like the other panes.
  void show_edit_pane(bool visible, int x, int y, int width, int height, bool focus = false) noexcept;
  [[nodiscard]] bool edit_pane_visible() const noexcept { return panels_attached_ && edit_visible_; }
  // The strip, its pane and the command bar's Edit button read this. Pushed
  // whether or not the pane is up, so the button follows the item.
  void set_edit_view(const chrome_edit_args& args) noexcept;

  // PR 30 (docs/design/21): the Video Editor window's islands -- its timeline, on the
  // editor's own window, and the card on the viewer's that says where the
  // picture went. Attached when the window opens, detached when it closes.
  // Optional: an older chrome without them fails attach_editor, and the host
  // then does not open the window.
  [[nodiscard]] bool attach_editor(HWND editor, HWND viewer) noexcept;
  void layout_editor(const chrome_editor_layout_args& args) noexcept;
  void set_editor_view(const chrome_editor_view_args& args) noexcept;
  void set_editor_strip(const chrome_editor_strip_args& args) noexcept;
  void detach_editor() noexcept;
  [[nodiscard]] bool editor_attached() const noexcept { return editor_attached_; }

  // The playback transport: a centred bar floating over the bottom of the
  // canvas, shown only while a clip is open. `filmstrip_px` is how much bottom
  // chrome is already spoken for, so the bar sits above the filmstrip.
  // park_transport() is issue #38's auto-hide: the bar keeps its content and
  // moves below the client area, so the canvas gets the pointer back and a
  // click there cannot press a button nobody can see.
  [[nodiscard]] expected attach_transport(HWND parent, void* context, chrome_command_fn on_command,
                                          void* session, int width, int height,
                                          std::uint32_t dpi) noexcept;
  void resize_transport(int width, int client_height, int filmstrip_px, std::uint32_t dpi) noexcept;
  void show_transport(bool visible, int width, int client_height, int filmstrip_px,
                      std::uint32_t dpi) noexcept;
  void park_transport(bool parked, int width, int client_height, int filmstrip_px,
                      std::uint32_t dpi) noexcept;
  // The row's natural width in DIPs (chrome_cmd_transport_width); 0 until the
  // island reports it, which falls back to kTransportMaxWidthDip. Returns
  // whether it changed, so the caller relays out only then.
  bool set_transport_content(int dip) noexcept;
  [[nodiscard]] bool transport_parked() const noexcept { return transport_parked_; }
  [[nodiscard]] bool transport_attached() const noexcept { return transport_attached_; }
  [[nodiscard]] bool transport_visible() const noexcept {
    return transport_attached_ && transport_visible_;
  }

  // Push the persisted toggles into the settings menu so the menu and the
  // keyboard cannot disagree about what is on.
  void apply_settings(std::int32_t flags, std::int32_t sort = 0) noexcept;

  // Push the current playback rate into the command bar's speed dropdown.
  void apply_rate(float rate) noexcept;
  // Issue #214: push volume, mute and the audio track into the More flyout.
  // Optional entry point: a chrome without it ignores the call.
  void apply_audio(float volume, bool muted, std::uint32_t track) noexcept;

  // The command table for `?` (describe_commands). Once at attach.
  void set_command_table(const std::string& utf8) noexcept;
  // Milestone G: open the Import window (kind 0), or import `paths_json` now
  // with the last preset (kind 1, Ctrl+Shift+F7). Optional entry point: a
  // chrome without it ignores the call.
  void show_import(std::int32_t kind, const std::string& paths_json) noexcept;
  // PR 15, Ctrl+Shift+S: Windows Share over `window` with the files in
  // `paths_json` (a UTF-8 JSON array). False when the chrome cannot share.
  bool share_files(HWND window, const std::string& paths_json) noexcept;
  // Milestone H: an add-on command by family (commands.h addon_family; 2 is
  // the AI pack, kinds 0 search / 1 similar / 2 next match / 3 previous
  // match). `json` is what is on screen. True when the add-on ran it; false
  // when there is no such entry, no add-on, or nothing to do.
  [[nodiscard]] bool show_addon(std::int32_t family, std::int32_t kind,
                                const std::string& json) noexcept;
  // docs/design/25 (2026-10-03): a command an add-on's manifest contributed
  // (commands.h addon_command_row), with the payload its row asked for as
  // JSON. The chrome hands it to the add-on's chrome. False when there is
  // no such entry or the add-on declined.
  [[nodiscard]] bool run_addon_command(const std::string& addon, const std::string& id,
                                       const std::string& json) noexcept;
  // docs/design/25: an add-on package (*.mvaddon) handed to the app. The chrome
  // shows what it is in Settings and asks; nothing is installed without the
  // answer. False when the chrome has no such entry.
  [[nodiscard]] bool offer_addon(const std::string& path_utf8) noexcept;

  // Drag-out. The answer to chrome_cmd_drag_items: the files to drag, one
  // UTF-8 path per line (empty: drag the cell alone). `own_drag` tells the
  // islands one of our drags is in flight (true with the answer, and around
  // the canvas's own drag), so they refuse to take it back; false when it ends.
  void set_drag_paths(const std::string& utf8_lines, bool own_drag) noexcept;
  // Open > Recent folders: "label\tpath\n" lines, most recent first. Pushed
  // at attach and whenever the list changes.
  void set_recent_folders(const std::string& utf8_lines) noexcept;

  // Opens a flyout on the command bar, or closes any (chrome_popup::close).
  void show_popup(chrome_popup kind, std::int32_t mode_mask) noexcept;
  // PR 10 export dialog (a flyout like `?`), preselecting `last_choice`
  // (pack_export). Confirming posts chrome_cmd_export.
  void show_export_dialog(std::int32_t last_choice) noexcept {
    show_popup(chrome_popup::export_image, last_choice);
  }
  void navigate_gallery(std::int32_t direction, std::int32_t index) noexcept;
  // File search. Optional entry: a chrome without it declines everything,
  // and every caller falls back to its old behaviour.
  [[nodiscard]] gallery_search_answer gallery_search(gallery_search_action action,
                                                     std::int32_t arg = 0) noexcept;
  void scale_gallery(std::int32_t direction, std::int32_t index) noexcept;
  void apply_browse(std::int32_t folder_cursor, bool can_go_up, const std::string& crumbs,
                    bool finding = false, const std::string& query = {}) noexcept;

  // True when the island consumed the message (do not Translate/Dispatch).
  [[nodiscard]] bool pre_translate(MSG* msg) noexcept;

  // Tab / Shift+Tab into the island. True if the island took focus.
  [[nodiscard]] bool navigate_focus(bool reverse) noexcept;

  // PR 8 updater. Hands the restart arguments (NUL-separated UTF-16) to the
  // managed updater, which arms Update.exe off the UI thread and then posts
  // chrome_cmd_update_restart(1). False if the entry is missing.
  bool request_update_restart(const std::wstring& args_blob) noexcept;

  // After the message loop (window gone): a staged update is applied once this
  // process exits. No restart. No-op in a dev build.
  void updater_exit() noexcept;

  void detach() noexcept;

  // Process exit only, after detach(): disposes the XAML runtime for this
  // thread (WindowsXamlManager, then the dispatcher queue). The chrome cannot
  // attach again in this process afterwards.
  void shutdown_for_exit() noexcept;

 private:
  [[nodiscard]] bool resolve_paths() noexcept;
  [[nodiscard]] bool load_hostfxr() noexcept;
  [[nodiscard]] bool load_runtime() noexcept;
  [[nodiscard]] chrome_entry_fn get_entry(const wchar_t* method) noexcept;

  HMODULE hostfxr_ = nullptr;
  void* context_ = nullptr;  // hostfxr_handle
  using load_assembly_fn = int (*)(const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*,
                                   void*, void**);
  load_assembly_fn load_fn_ = nullptr;
  chrome_entry_fn probe_ = nullptr;
  chrome_entry_fn attach_ = nullptr;
  chrome_entry_fn resize_ = nullptr;
  chrome_entry_fn detach_ = nullptr;
  chrome_entry_fn navigate_ = nullptr;
  chrome_entry_fn attach_filmstrip_ = nullptr;
  chrome_entry_fn resize_filmstrip_ = nullptr;
  chrome_entry_fn detach_filmstrip_ = nullptr;
  chrome_entry_fn show_filmstrip_ = nullptr;
  chrome_entry_fn attach_gallery_ = nullptr;
  chrome_entry_fn resize_gallery_ = nullptr;
  chrome_entry_fn show_gallery_ = nullptr;
  chrome_entry_fn detach_gallery_ = nullptr;
  chrome_entry_fn attach_transport_ = nullptr;
  chrome_entry_fn resize_transport_ = nullptr;
  chrome_entry_fn show_transport_ = nullptr;
  chrome_entry_fn detach_transport_ = nullptr;
  chrome_entry_fn apply_settings_ = nullptr;
  chrome_entry_fn apply_rate_ = nullptr;
  chrome_entry_fn apply_audio_ = nullptr;
  chrome_entry_fn set_command_table_ = nullptr;
  chrome_entry_fn show_import_ = nullptr;
  chrome_entry_fn share_files_ = nullptr;
  chrome_entry_fn show_addon_ = nullptr;  // Milestone H
  chrome_entry_fn offer_addon_ = nullptr;  // docs/design/25, optional
  chrome_entry_fn run_addon_command_ = nullptr;  // docs/design/25, optional
  chrome_entry_fn set_drag_paths_ = nullptr;
  chrome_entry_fn set_recent_folders_ = nullptr;
  chrome_entry_fn show_popup_ = nullptr;
  chrome_entry_fn attach_panels_ = nullptr;
  chrome_entry_fn detach_panels_ = nullptr;
  chrome_entry_fn show_meta_pane_ = nullptr;
  chrome_entry_fn show_folder_tree_ = nullptr;
  chrome_entry_fn set_meta_data_ = nullptr;
  chrome_entry_fn set_tree_root_ = nullptr;
  chrome_entry_fn take_tree_path_ = nullptr;
  chrome_entry_fn show_adjust_pane_ = nullptr;
  chrome_entry_fn set_adjust_view_ = nullptr;
  chrome_entry_fn set_meta_edit_ = nullptr;
  chrome_entry_fn show_jobs_pane_ = nullptr;  // PR 13 / 14
  chrome_entry_fn set_trim_ = nullptr;        // PR 13
  chrome_entry_fn show_edit_pane_ = nullptr;  // PR 29
  chrome_entry_fn set_edit_view_ = nullptr;   // PR 29
  chrome_entry_fn attach_editor_ = nullptr;   // PR 30
  chrome_entry_fn layout_editor_ = nullptr;
  chrome_entry_fn set_editor_view_ = nullptr;
  chrome_entry_fn set_editor_strip_ = nullptr;
  chrome_entry_fn detach_editor_ = nullptr;
  bool editor_attached_ = false;
  chrome_entry_fn navigate_gallery_ = nullptr;
  chrome_entry_fn gallery_search_ = nullptr;  // optional: file search
  chrome_entry_fn scale_gallery_ = nullptr;
  chrome_entry_fn apply_browse_ = nullptr;
  chrome_entry_fn update_restart_ = nullptr;
  chrome_entry_fn updater_exit_ = nullptr;
  bool transport_attached_ = false;
  bool transport_visible_ = false;
  bool transport_parked_ = false;  // issue #38: auto-hidden, content kept
  int transport_content_dip_ = 0;  // 0: not reported yet
  bool filmstrip_attached_ = false;
  bool filmstrip_visible_ = false;
  bool gallery_attached_ = false;
  bool gallery_visible_ = false;
  bool panels_attached_ = false;
  bool meta_visible_ = false;
  bool tree_visible_ = false;
  bool adjust_visible_ = false;
  bool jobs_visible_ = false;
  bool edit_visible_ = false;
  chrome_entry_fn island_window_ = nullptr;
  chrome_entry_fn begin_detach_ = nullptr;  // unhooks static XAML events first
  chrome_entry_fn shutdown_for_exit_ = nullptr;
  // Indexed by focus_kind: [command_bar .. transport]. Refreshed after attach.
  HWND island_hwnds_[static_cast<int>(focus_kind::transport) + 1]{};
  HWND pane_hwnds_[kPaneIslandCount]{};  // in kPaneIslandIds order
  using pre_translate_fn = BOOL(WINAPI*)(const MSG*);
  pre_translate_fn pre_translate_ = nullptr;
  bool attached_ = false;
  wchar_t chrome_dir_[MAX_PATH]{};
  wchar_t chrome_dll_[MAX_PATH]{};
  wchar_t runtime_config_[MAX_PATH]{};
  wchar_t hostfxr_path_[MAX_PATH]{};
  wchar_t dotnet_root_[MAX_PATH]{};
};

}  // namespace mv::shell
