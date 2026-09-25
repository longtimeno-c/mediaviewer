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

// The running version, from CMake project(VERSION) — the same string Windows
// shows in About. Writes a NUL-terminated string into `buf`. Returns false
// when the host was built without a version or `size` < 1. [any-thread]
bool mv_chrome_app_version(char* buf, int32_t size);

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

// Multi-folder browsing (plan/10 PR 26). The open folder's child folders are
// shown as tiles above its images; the breadcrumb runs from the highest folder
// reached to the one on screen. All [main-thread]. Folder tiles are keyed by
// their full path, which survives a relist.
int32_t mv_chrome_subfolder_count(void);
// Copies the folder's full UTF-8 path; false if `index` is out of range.
bool mv_chrome_subfolder_path(int32_t index, char* out_buf, int32_t out_buf_size);
// Navigates into the child folder (a fresh listing; the trail keeps its root).
void mv_chrome_open_subfolder(int32_t index);
// Asks for the tile's item count, folder count and cover thumbnail. Result via
// the callback below, always on the main thread. Never blocks.
void mv_chrome_request_folder_summary(int32_t index);
// `ok` is false on failure or when the host moved to another folder first;
// `cover_thumb_path_utf8` is NULL when the folder has no media to cover it.
// `flags` bit 0: the cover is a descendant (photos were found further down, this
// folder's own listing has none). Bit 1: the bounded look stopped early and
// found no photo, so the tile must not say the branch is only folders.
typedef void (*mv_chrome_folder_summary_fn)(const char* folder_path_utf8, bool ok,
                                            int32_t media_count, int32_t subfolder_count,
                                            int32_t flags, const char* cover_thumb_path_utf8);
void mv_chrome_set_folder_summary_callback(mv_chrome_folder_summary_fn callback);
// Breadcrumb: root … current. `mv_chrome_crumb_path` copies the full path
// (the last component is the label); false if out of range.
int32_t mv_chrome_crumb_count(void);
bool mv_chrome_crumb_path(int32_t index, char* out_buf, int32_t out_buf_size);
void mv_chrome_open_crumb(int32_t index);
bool mv_chrome_can_go_up(void);
void mv_chrome_navigate_up(void);
// The gallery's keyboard position while on a folder tile; -1 when on images.
int32_t mv_chrome_folder_cursor(void);
// The in-progress folder-name query (`/`, folder row active). False when idle;
// an empty string means the query is open and nothing has been typed yet.
bool mv_chrome_folder_query(char* out_buf, int32_t out_buf_size);

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

// ---- PR 12: rating, comment, revert (plan/06 "Writing", plan/16 Rate) ---------
//
// The pane's star row, its comment field and its Revert button. The keys 0-5
// go through the command table like every other key; these are the pointer
// and the text field. A change is queued in the host's write queue and lands on
// the I/O pool (a JPEG in place, everything else in an XMP sidecar); none of it
// touches a file on the main thread. [main-thread]
//
// The rating as the pane should draw it: a change still waiting to be written
// counts, so a star clicked or a key pressed shows at once. -1 rejected, 0..5.
int32_t mv_chrome_meta_rating(void);
// The comment as UTF-8, newlines kept (the summary table flattens them). A
// change still waiting counts. Returns the length needed, like the tables.
int32_t mv_chrome_meta_comment(char* buf, int32_t size);
// True when an item is open, so there is something to rate or comment.
bool mv_chrome_meta_can_edit(void);
// True when a write to the current item has landed this session, so "revert
// metadata" has a snapshot to go back to.
bool mv_chrome_meta_can_revert(void);
void mv_chrome_meta_set_rating(int32_t stars);          // 0 clears
void mv_chrome_meta_set_comment(const char* utf8);      // "" clears
void mv_chrome_meta_revert(void);
// Ctrl+I (edit_comment) shows the pane and asks for the comment field to take
// keyboard focus; the sequence moves each time, so Swift focuses once per ask.
uint64_t mv_chrome_meta_focus_seq(void);
// Hands the keyboard back to the canvas (Esc or Return in the comment field).
void mv_chrome_meta_blur(void);

// One line for the command bar: what a key or a click just did ("★★★★☆  saved",
// "Could not save the rating"). The generation moves when the text changes; the
// text is empty once it has been up long enough. Returns the length needed.
uint64_t mv_chrome_notice_generation(void);
int32_t mv_chrome_notice_text(char* buf, int32_t size);

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

// Milestone G add-ons (plan/18 "Add-ons"), implemented in src/shell/addons_mac.mm.
// Strings follow mv_chrome_command_table's rule: returns the length needed,
// writes at most `size` bytes NUL-terminated. [worker-thread] where noted:
// those hash files and must not run on the main actor.
int32_t mv_addons_state_json(char* buf, int32_t size);                 // [worker]
int32_t mv_addons_check_manifest(const uint8_t* manifest, int32_t manifest_len,
                                 const uint8_t* sig, int32_t sig_len, char* buf, int32_t size);
int32_t mv_addons_sha256(const char* path, char* buf, int32_t size);   // [worker]
int32_t mv_addons_make_staging(char* buf, int32_t size);
bool mv_addons_install(const char* staged_dir);                        // [worker]
bool mv_addons_load(void);                                             // [main-thread]
bool mv_addons_remove(bool keep_data);                                 // [main-thread]
bool mv_addons_loaded(void);
void mv_addons_open_import(void);
int32_t mv_addons_status(char* buf, int32_t size);
bool mv_addons_hint_pending(void);
void mv_addons_hint_done(bool never_again);

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

// ---- PR 13 / 14: trim mode, clip tools, Jobs pane (plan/08, plan/10) ---------
//
// The twin of the Windows transport overlay, clip tools flyout and Jobs pane
// (IslandHost.Clip.cs). The host owns trim mode (shell/trim_state.h, shared
// with Windows) and the clip job queue (abi/clip_session, the same one the
// Windows ABI drives); Swift polls these and posts back. Nothing here reads a
// file or waits on a job. [main-thread]

// Runs a command-table command by id (commands.h), as its key would. The
// transport's trim buttons and the panes' close buttons use it.
void mv_chrome_run_command(int32_t command_id);

// Trim on the scrub bar. `generation` moves whenever anything below changes.
// Times are nanoseconds on the player's timeline; -1 = unset. `cut_*` is the
// range Path 1 will write (keyframe-snapped).
typedef struct mv_trim_view {
  int32_t armed;
  int32_t index_ready;
  int32_t previewing;
  int32_t keyframe_count;
  int64_t duration_ns;
  int64_t in_ns;
  int64_t out_ns;
  int64_t cut_in_ns;
  int64_t cut_out_ns;
} mv_trim_view;
uint64_t mv_chrome_trim_generation(void);
bool mv_chrome_trim_view(mv_trim_view* out);
// Up to `cap` keyframe times; returns the full count.
int32_t mv_chrome_trim_keyframes(int64_t* out, int32_t cap);
// "In 0:12.345 · Out 0:40.000 · keyframe cut …"; length needed, as the tables.
int32_t mv_chrome_trim_label(char* buf, int32_t size);

// The clip tools sheet (⌘S on a clip). `flags` as trim_state.h kClipFlag*:
// 1 markers set, 2 has audio, 4 has video. The answer is pack_clip_choice
// (op | option << 8), exactly as the Windows flyout packs it.
bool mv_chrome_clip_tools_visible(void);
int32_t mv_chrome_clip_tool_flags(void);
void mv_chrome_clip_tool_confirm(int32_t packed);
void mv_chrome_clip_tool_cancel(void);

// The Jobs pane (⌘J). `generation` moves when a job is queued, starts or
// finishes; progress is polled while one runs.
typedef struct mv_chrome_job {
  uint64_t id;
  int32_t state;         // mv_clip_job_state: 1 queued 2 running 3 done 4 failed 5 cancelled
  int32_t op;            // mv_clip_op
  double fraction;       // 0..1
  int64_t elapsed_ms;
  int64_t eta_ms;        // -1 until measured
  int32_t error;         // mv_status when failed
  int32_t output_count;
} mv_chrome_job;
bool mv_chrome_jobs_visible(void);
uint64_t mv_chrome_jobs_generation(void);
// Newest first; returns the full count.
int32_t mv_chrome_jobs(uint64_t* ids, int32_t cap);
bool mv_chrome_job_info(uint64_t id, mv_chrome_job* out);
// which: 0 title ("Trim (re-encode, VideoToolbox)"), 1 source file name,
// 2 first output path. Length needed, as the tables.
int32_t mv_chrome_job_text(uint64_t id, int32_t which, char* buf, int32_t size);
void mv_chrome_job_cancel(uint64_t id);
void mv_chrome_job_retry(uint64_t id);
// Finder, the output selected.
void mv_chrome_job_reveal(uint64_t id);
void mv_chrome_jobs_clear_finished(void);
void mv_chrome_jobs_close(void);
// Esc in the pane: focus back to the canvas (the pane stays open).
void mv_chrome_jobs_blur(void);

#ifdef __cplusplus
}
#endif
