// SPDX-License-Identifier: GPL-2.0-or-later
// C bridge from Swift chrome to the present lab's input_snapshot
// (src/shell/input_state.h). Implemented in src/shell/main_mac.mm, not here:
// this header only declares the boundary, same shape rule as plan/14-abi.md
// (POD/void args, no C++ types, no exceptions across the line) scoped down
// to what PR 18's command-bar scaffold needs.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumps input_snapshot.fit_seq and wakes the render thread. [any-thread]
void mv_chrome_fit(void);

// Runs a menu-bar command by tag (the MvMenuCmd enum in main_mac.mm; the
// in-window menus and the system menu bar share one dispatcher). [main-thread]
void mv_chrome_menu(int32_t cmd);

// Settings screen (plan/16 Settings). View flags use the bit layout of
// view_settings::flags() (shell/settings.h): 1 filmstrip for a folder,
// 2 filmstrip for an image, 4 wrap, 8 sticky zoom, bits 4-5 canvas background.
// [main-thread]
bool mv_chrome_settings_visible(void);
int32_t mv_chrome_view_flags(void);
void mv_chrome_set_view_flags(int32_t flags);
// The live key table, one "id\tmodes\tname\tkeys\trunnable\trow" line per
// binding this host can run. Returns the length needed; writes at most `size`
// bytes, NUL-terminated.
int32_t mv_chrome_command_table(char* buf, int32_t size);
// Key remapping: begin waiting for the next key press for table row `row`
// (Esc cancels). `mv_chrome_key_capture_row` is -1 when none is pending.
// `mv_chrome_keys_generation` moves whenever the table, flags or capture change.
void mv_chrome_key_capture_begin(int32_t row);
void mv_chrome_key_capture_cancel(void);
int32_t mv_chrome_key_capture_row(void);
void mv_chrome_keys_reset(void);
uint64_t mv_chrome_keys_generation(void);

// Bumps input_snapshot.one_to_one_seq and wakes the render thread. [any-thread]
void mv_chrome_one_to_one(void);

// PR 18 (folded-in PR 4, plan/12 2026-09-17): the filmstrip/gallery
// SwiftUI-side follow-up. These all read/mutate state MvLabApp owns
// (folder_model + browse_index, src/shell/main_mac.mm) — never folder_model
// or browse_index directly, so this stays the one boundary between Swift
// chrome and the host, same as plan/14-abi.md's C-ABI shape rule scoped down
// to what this lab needs. [main-thread] for all of these: SwiftUI runs on
// the main actor, and so does every keyDown:/NSTimer call on the C++ side
// that also touches this state, so there is no cross-thread synchronization
// here beyond what input_snapshot's publish/wake already does.

// 0 when no folder is open. Matches the count `mv_chrome_item_name`/
// `mv_chrome_select_index` index against.
int32_t mv_chrome_item_count(void);

// -1 when no folder is open (mirrors browse_index::current() being
// meaningless on an empty listing).
int32_t mv_chrome_current_index(void);

// Copies the UTF-8 name of the item at `index` into `out_buf` (NUL-
// terminated, truncated to fit `out_buf_size`). Returns false and leaves
// `out_buf` untouched if `index` is out of `[0, mv_chrome_item_count())`.
bool mv_chrome_item_name(int32_t index, char* out_buf, int32_t out_buf_size);

// Navigates to `index` the same way clicking a filmstrip/gallery cell does
// (calls MvLabApp's existing -selectIndex:, the same path arrow keys use).
// A no-op if `index` is out of range or no folder is open.
void mv_chrome_select_index(int32_t index);

// Registered once at startup. Called on the main thread when a thumbnail
// requested via mv_chrome_request_thumb becomes ready; `thumb_path_utf8` is
// NULL on failure (folder_model::thumb_ready_fn's contract, forwarded here
// after the host has already hopped back to the main thread — the callback
// this registers is never invoked from a pool thread).
//
// `name_utf8` identifies which item this result is for — not the index it
// was requested at. A directory can't have two entries with the same name,
// so it is a stable key across a relist the way a raw index is not: if the
// listing reorders (including from the app's own copy/move/Trash) between
// the request and this callback firing, an index-keyed cache would attach
// the result to whatever item now sits at that index instead of the one
// that was actually asked for. Always non-NULL.
typedef void (*mv_chrome_thumb_ready_fn)(const char* name_utf8, const char* thumb_path_utf8);
void mv_chrome_set_thumb_ready_callback(mv_chrome_thumb_ready_fn callback);

// Asynchronously requests (looks up, or decodes + caches) the JPEG-512
// thumbnail for the item at `index`. The result arrives later through the
// callback registered with mv_chrome_set_thumb_ready_callback — this never
// blocks (CLAUDE.md rule 1: the request itself does real I/O on a
// folder_model job, same contract as folder_model::request_thumb).
// A no-op if `index` is out of range.
void mv_chrome_request_thumb(int32_t index);

// T / G toggle these from keyDown: (main_mac.mm); Swift polls them (they are
// plain bools, not worth a push channel the way thumbnails are) to decide
// whether to show the filmstrip/gallery views it already hosts.
bool mv_chrome_filmstrip_visible(void);
bool mv_chrome_gallery_visible(void);

// Called by the gallery view when a cell is clicked: selects `index` (same
// as mv_chrome_select_index) and closes the gallery in one call, so a click
// cannot land between the two and briefly show the old selection with the
// gallery already gone.
void mv_chrome_select_index_and_close_gallery(int32_t index);

// Bumped every time the host replaces its folder listing (a relist after a
// watch event, or opening another folder). Swift compares it against the last
// value it saw to know when a cached name/thumbnail-by-index mapping is
// stale even if the item count is unchanged (a rename, or one file added and
// another removed). [main-thread]
uint64_t mv_chrome_listing_generation(void);

// Marks (plan/16 "Marks, copy, move"): bumped whenever the mark set changes,
// so Swift can rebuild its marked-item cache only when it must.
// `mv_chrome_is_marked` is false for an out-of-range index. [main-thread]
uint64_t mv_chrome_marks_generation(void);
int32_t mv_chrome_marked_count(void);
bool mv_chrome_is_marked(int32_t index);

// The gallery reports how many cells it currently lays out per row, so the
// host can move the selection by row for Up/Down/W/S (plan/16 `G` row).
// Values < 1 are clamped to 1. [main-thread]
void mv_chrome_set_gallery_columns(int32_t columns);

// PR 20 updates (plan/13). True once Sparkle has a verified update staged and
// is waiting for the user; always false in the bare lab. Restart installs it
// and relaunches onto the same folder and file. [main-thread]
bool mv_chrome_update_ready(void);
void mv_chrome_restart_to_update(void);

// Video transport (PR 19, plan/16 "Video"). The render thread owns the clip;
// these read the status it publishes and post commands back as latched counters.
// [main-thread]
//
// Fills the out-params and returns true while a clip is on screen; false (out
// params untouched) otherwise. `rate_x100` is the playback rate times 100.
bool mv_chrome_video_status(int64_t* position_ms, int64_t* duration_ms, bool* playing,
                            int32_t* rate_x100, bool* muted, float* volume);
void mv_chrome_video_toggle(void);
// Relative skip, exact.
void mv_chrome_video_skip(int64_t delta_ms);
// Absolute seek. exact=false is the scrubber drag (nearest keyframe, instant);
// exact=true is the release (decode forward to the frame).
void mv_chrome_video_seek(int64_t position_ms, bool exact);
// One rung down (-1) / up (+1) the 0.25 / 0.5 / 1 / 1.5 / 2 / 4 ladder.
void mv_chrome_video_speed_step(int32_t direction);
void mv_chrome_video_toggle_mute(void);
// Absolute volume 0..1, from the transport strip's "More" panel slider.
void mv_chrome_video_set_volume(float volume);
// Frame step, paused only: -1 back, +1 forward (plan/16 "More" panel buttons).
void mv_chrome_video_step(int32_t frames);

// ---- PR 9: metadata pane, folder tree, sort (plan/06, plan/16) ---------------
//
// The pane reads a record the host already holds (meta_store). Nothing here
// reads the file on the main thread, and toggling the pane, the info overlay or
// the AF quads never reads it again. [main-thread] unless noted.

// Moves when the record the pane shows changes: the selection moved, or its
// read finished. Swift re-reads the tables below only when it moves.
uint64_t mv_chrome_meta_generation(void);
bool mv_chrome_meta_pane_visible(void);
// True while the current item's record has been asked for and not yet arrived.
bool mv_chrome_meta_loading(void);
// Text tables, one record per line, fields tab-separated (tabs and newlines in
// values are flattened to spaces). Each returns the length needed and writes at
// most `size` bytes, NUL-terminated, like mv_chrome_command_table.
//   summary:    "label\tvalue"                      every row for the kind, value may be empty
//   properties: "space\tgroup\tlabel\tvalue\traw_tag"  space = exif|iptc|xmp|container|computed
//   streams:    "S\tindex\tkind\tcodec" starts a stream, "F\tlabel\tvalue" adds a field to
//               it, "C\tstart_ms\ttitle" is a chapter; empty for a still
int32_t mv_chrome_meta_summary(char* buf, int32_t size);
int32_t mv_chrome_meta_properties(char* buf, int32_t size);
int32_t mv_chrome_meta_streams(char* buf, int32_t size);

// Folder tree. `mv_chrome_list_subdirectories` does a directory read, so call it
// from a background task, never from the main actor. [any-thread] Writes
// "name\tpath" lines; returns the length needed, or -1 if `dir` cannot be read.
bool mv_chrome_tree_visible(void);
int32_t mv_chrome_list_subdirectories(const char* dir_utf8, char* buf, int32_t size);
// The folder currently open in the viewer ("" when none). [main-thread]
int32_t mv_chrome_current_folder(char* buf, int32_t size);
// Opens `dir_utf8` exactly as Open Folder does. [main-thread]
void mv_chrome_open_folder(const char* dir_utf8);

// Sort (plan/16). Packed as sort_order.h pack_sort: key in bits 0-2 (0 name,
// 1 modified, 2 size, 3 type, 4 date taken), descending in bit 3. Changing it
// re-sorts the listing and keeps the current item selected. [main-thread]
int32_t mv_chrome_sort_order(void);
void mv_chrome_set_sort_order(int32_t packed);

// ---- PR 10: export sheet (plan/10 "SwiftUI crop mode and export sheet") -------
//
// The sheet's choice is one integer, packed exactly as the Windows export
// dialog packs it (shell/edit_session.h pack_export): quality in bits 0-6, PNG
// in bit 7, metadata policy (0 all, 1 minus GPS, 2 none) in bits 8-9, and the
// long-edge index (full, 3840, 2560, 2048, 1600, 1080) in bits 10-12.
// [main-thread]
int32_t mv_chrome_export_last_choice(void);
// Runs the export on the pool (beside the original, never over anything) and
// closes the sheet.
void mv_chrome_export_confirm(int32_t packed);
void mv_chrome_export_cancel(void);

// ---- PR 11: adjust pane (plan/10 "SwiftUI adjust pane", plan/07) -------------
//
// The twin of the Windows adjust pane (IslandHost.Adjust.cs). The host owns the
// edit stack; the pane posts slider values and draws what the host reports.
// Nothing here touches pixels: the canvas redraws from the new uniforms on the
// render thread, the histogram is a reduction a worker ran. [main-thread]

// Field for field shell::adjust_view (src/shell/adjust_pane.h); main_mac.mm
// static_asserts the size and offsets.
//   readiness: 0 no still, 1 preparing (sliders disabled), 2 ready, 3 failed
//   values:    exposure (EV, -5..5), contrast, saturation, temperature, tint (-100..100)
//   bins:      64 bins each of R, G, B, luma, 0..1000 (edit::pack_histogram)
typedef struct mv_adjust_view {
  int32_t readiness;
  int32_t from_raw;
  float values[5];
  float clip_high;
  float clip_low;
  int32_t histogram_valid;
  int32_t reserved;
  uint16_t bins[4 * 64];
} mv_adjust_view;

bool mv_chrome_adjust_visible(void);
// Moves whenever the view below changes for a reason other than the pane's own
// slider (readiness, the histogram, undo, reset, a new item). Swift re-reads
// the view only when it moves, so a drag is never fought.
uint64_t mv_chrome_adjust_generation(void);
bool mv_chrome_adjust_view(mv_adjust_view* out);
// `param` is edit::adjust_param (0 exposure .. 4 tint). Ignored until ready.
void mv_chrome_adjust_set(int32_t param, float value);
void mv_chrome_adjust_reset(void);
void mv_chrome_adjust_close(void);
// Esc in the pane: keyboard focus back to the canvas (the pane stays open).
void mv_chrome_adjust_blur(void);

#ifdef __cplusplus
}
#endif
