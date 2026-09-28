// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
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

// The full UTF-8 path of the original at `index` (a folder item or a result
// list's entry), for a file drag out of the gallery / filmstrip. Returns the
// byte length needed, as the tables; 0 when `index` is out of range.
int32_t mv_chrome_item_path(int32_t index, char* buf, int32_t size);

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

// Is the item at `index` a clip (a video, by extension: the same test the host
// uses to route it to the player)? Drives the gallery's play badge. False for
// an out-of-range index. [main-thread]
bool mv_chrome_item_is_video(int32_t index);

// The gallery reports how many cells it currently lays out per row, so the
// host can move the selection by row for Up/Down/W/S (plan/16 `G` row).
// Values < 1 are clamped to 1. [main-thread]
void mv_chrome_set_gallery_columns(int32_t columns);

// PR 20 updates (plan/13). Phase: 0 idle, 1 checking, 2 downloading, 3 a
// verified update is staged and waiting for the user; always 0 in the bare
// lab. The version is the one being downloaded or staged (false if none yet).
// Restart installs a staged update and relaunches onto the same folder and
// file. [main-thread]
int32_t mv_chrome_update_phase(void);
bool mv_chrome_update_version(char* out, int32_t cap);
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
// File search (plan/16 "File search", 2026-09-28): find by name over the
// gallery, base app, no index. Shows the gallery if it is hidden and gives the
// field above the grid the keyboard. The path bar's search icon runs it when
// Local search is not installed; ⌘F falls back to it the same way. [main-thread]
void mv_chrome_file_search(void);
// The field gives the keyboard back to the grid (Esc, Down, Return).
void mv_chrome_gallery_blur(void);
// What file search's name filter shows, in grid order: item indices and
// child-folder indices of the listing `listing_generation` (mv_chrome_listing_generation).
// While `active` and the gallery is up, its keys move among these only and a
// selection the filter hides moves to the first tile shown; the viewer still
// walks the whole folder. A stale generation is ignored until the next push.
void mv_chrome_set_gallery_filter(uint64_t listing_generation, bool active,
                                  const int32_t* items, int32_t item_count,
                                  const int32_t* folders, int32_t folder_count);

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
//   properties: "space\tgroup\tlabel\tvalue\traw_tag\traw\taccess"
//               space = exif|iptc|xmp|container|computed; raw = the value in the
//               form an edit takes; access (PR 29, meta::access_of) = e editable
//               (set, remove), s set only (into the XMP sidecar), r read-only
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

// PR 29 (owner, 2026-09-26): every tag editable. Queued like the PR 12
// writes, so a JPEG is rewritten in place (checked) and anything else gets its
// XMP sidecar; Revert above puts every tag back. [main-thread]
// Sets `key` (a raw_tag from the table) to `value` in its raw form; NULL
// removes it. A key the file cannot take beeps and says why.
void mv_chrome_meta_set_tag(const char* key, const char* value);
// "YYYY-MM-DD HH:MM:SS" into every capture-time tag the file carries; NULL
// removes them all.
void mv_chrome_meta_set_date(const char* value);

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

// Update channel: -1 when the app was built without an updater (hide the
// setting), 0 stable, 1 preview (signed prerelease builds as well). Setting it
// checks for updates on the new channel. [main-thread]
int32_t mv_chrome_update_channel(void);
void mv_chrome_set_update_channel(int32_t channel);

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

// Milestone H (plan/17): the same management by add-on id, for the AI pack
// ("ai" = Core, "ai-faces" = People, "ai-audio" = Sound) beside Import. Also addons_mac.mm. The
// mv_addons_* functions above stay Import's, unchanged. mv_addons_check_manifest
// now also reports "id", "part_of" and "installed_size"; mv_addons_install and
// mv_addons_make_staging / mv_addons_sha256 serve every add-on.
// False for "ai-cuda" (Windows only) and, on an Intel Mac, for the whole AI
// family: ONNX Runtime ships no x86_64 macOS build, so Local search is hidden.
bool mv_addon2_supported(const char* id);
// {"id","supported","installed","version","size","state":"ok|needs_update|invalid","why"}
int32_t mv_addon2_state_json(const char* id, char* buf, int32_t size);   // [worker]
bool mv_addon2_loaded(const char* id);                                     // [main-thread]
bool mv_addon2_loading(const char* id);                                    // [main-thread]
// "" or why the last load failed (a status name, "bundle", "chrome").
int32_t mv_addon2_load_error(const char* id, char* buf, int32_t size);     // [main-thread]
// Import loads at once; "ai" verifies and starts on a worker and attaches
// later (poll mv_addon2_loaded / mv_addon2_loading). [main-thread]
bool mv_addon2_load(const char* id);
// After a piece is installed or removed: the loaded "ai" pack re-reads its
// pieces (mv.ai.1 set_setting "reload"); People is picked up at once.
bool mv_addon2_reload(const char* id);                                     // [main-thread]
// Removing "ai" also removes its pieces; keep_data keeps the search index.
bool mv_addon2_remove(const char* id, bool keep_data);                     // [main-thread]
// A family's installed bytes and ceiling (0 = none); "ai" has 3 GB.
bool mv_addon2_family_usage(const char* family, uint64_t* used, uint64_t* ceiling);  // [worker]
// The loaded chrome's Settings view (an NSView*, owned by the chrome), or NULL.
void* mv_addon2_settings_view(const char* id);                             // [main-thread]
// Runs an add-on command by name ("search_open"), as its key would. Also the
// indexing pill's "index_anyway" (set_setting battery_override: ignore the
// battery pause until the Mac is next on power; false from an older pack).
bool mv_addon2_run_command(const char* name);                              // [main-thread]
// The command bar's indexing pill: mv.ai.1 status (mediaviewer_ai.h), POD.
// False while the AI pack is not loaded. [main-thread][no-block]
typedef struct mv_chrome_ai_status {
  int32_t state;           // mv_ai_state: 0 idle 1 indexing 2 paused 3 yielding 4 loading 5 error
  int32_t yield_reason;    // mv_ai_yield: 1 viewer 2 battery 3 frames
  int32_t backend;         // mv_ai_backend: 0 CPU 3 Core ML
  int32_t provider_fault;
  uint32_t flags;          // MV_AI_STATUS_*: 1 index full, 2 faces on, 4 faces ready, 8 no models,
                           // 16 audio ready
  uint32_t reserved;
  uint64_t assets_total;
  uint64_t assets_done;
  uint64_t frames_indexed;
  double eta_low_seconds;  // -1 unknown
  double eta_high_seconds;
  // 2026-09-27, audio (the ai-audio piece): clips indexed for sounds / speech.
  uint64_t sound_total;
  uint64_t sound_done;
  uint64_t speech_total;
  uint64_t speech_done;
} mv_chrome_ai_status;
bool mv_addon2_ai_status(mv_chrome_ai_status* out);

// ---- Milestone H: result listings and match markers (plan/17) -----------------
//
// The Mac twin of mv_folder_open_list (mediaviewer.h 0.14). A listing that is
// not a directory: search results shown by the same gallery, filmstrip,
// selection and keyboard model as a folder. Items keep the order given (best
// match first; no sort), are not paired or watched; a clip with a moment >= 0
// opens PAUSED on that frame (exact seek). While a list is open
// mv_chrome_current_folder is "" and mv_chrome_list_title is the query;
// opening a directory ends the list. [main-thread] for all of these.
//
// `moments_ms` may be NULL (all -1). `select_index` is the tile to show.
// `gallery` opens the gallery grid (Cmd+Enter) instead of the canvas.
bool mv_chrome_open_list(const char* title_utf8, const char* const* paths_utf8,
                         const int64_t* moments_ms, int32_t count, int32_t select_index,
                         bool gallery);
bool mv_chrome_list_open(void);
// The list's title; "" when a directory is open. Length needed, as the tables.
int32_t mv_chrome_list_title(char* buf, int32_t size);
// Back to the folder the list replaced (the path row's "Back to folder").
void mv_chrome_close_list(void);
// The full path of the item on the canvas ("" none). Length needed.
int32_t mv_chrome_current_item_path(char* buf, int32_t size);
// Match markers over the scrub bar for the clip at `clip_path_utf8` (the AI
// chrome's clip_matches for the active search). `current` is the index drawn
// larger, -1 none. count 0 clears. Markers for a clip that is not on screen
// are never reported.
void mv_chrome_set_scrub_markers(const char* clip_path_utf8, const int64_t* ms, int32_t count,
                                 int32_t current);
// Moves whenever the markers or the item on screen change.
uint64_t mv_chrome_scrub_markers_generation(void);
// Up to `cap` marker times (ms) for the clip on screen; returns the full count.
int32_t mv_chrome_scrub_markers(int64_t* out, int32_t cap, int32_t* current);

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

// ---- PR 29: the Edit workspace (plan/20) ---------------------------------------
//
// One visible door to the PR 10-14 edits: the strip (title, tabs, Undo / Reset
// / Original / Save copy) and its Crop and Trim panes. The Colour, Info and
// Jobs tabs are the adjust, metadata and Jobs panes above, hung under the
// strip. The host owns the workspace (shell/edit_workspace.h, shared with
// Windows) and the crop draft (shell/edit_session.h). [main-thread]

// `generation` moves whenever anything below changes.
typedef struct mv_edit_view {
  int32_t open;           // the workspace is up
  int32_t tab;            // shell::edit_tab: 0 crop, 1 colour, 2 info, 3 trim, 4 jobs
  int32_t subject;        // 0 nothing to edit, 1 a still, 2 a clip
  int32_t crop_active;    // a crop draft is on the canvas
  int32_t aspect;         // shell::crop_aspect: 0 free, 1 original, 2 1:1, 3 4:3, 4 3:2, 5 16:9, 6 5:4
  int32_t portrait;       // the locked preset is portrait
  float straighten;       // degrees: the draft's angle, else the committed one
  int32_t edit_count;     // ops on the item's stack
  int32_t show_original;  // Y held, or the strip's Original toggle
  int32_t crop_width;     // what Apply would keep, in pixels (0 = not known yet)
  int32_t crop_height;
} mv_edit_view;
uint64_t mv_chrome_edit_generation(void);
bool mv_chrome_edit_view(mv_edit_view* out);
// The item's file name (display only); length needed, as the tables.
int32_t mv_chrome_edit_name(char* buf, int32_t size);
// A tab the subject offers; others are ignored.
void mv_chrome_edit_select_tab(int32_t tab);
// Done / the close button: applies a crop draft, then closes.
void mv_chrome_edit_close(void);
// The Crop pane: a preset (+ orientation), the straighten slider (degrees),
// Cancel (drops the draft). Each starts cropping if it was not.
void mv_chrome_edit_set_aspect(int32_t aspect, int32_t portrait);
void mv_chrome_edit_set_straighten(float degrees);
void mv_chrome_edit_cancel_crop(void);
// The strip's Original toggle (the same state Y holds).
void mv_chrome_edit_show_original(int32_t on);
// Save copy…: applies a crop draft, then opens the export sheet (PR 10).
void mv_chrome_edit_save_copy(void);

// ---- PR 30: the Video Editor window (plan/21) ------------------------------------
//
// Its own window: the viewer's canvas as the preview, and this timeline. The
// cut list is the host's (shell/video_timeline.h, shared with Windows); Swift
// polls it by generation and posts edits back. Times are nanoseconds:
// "timeline" is the edited program, "source" the clip. [main-thread]
typedef struct mv_editor_view {
  int32_t open;
  int32_t ready;          // the clip is probed; the strip may still be empty
  int64_t length_ns;      // the program
  int64_t playhead_ns;    // on the program
  int64_t source_ns;      // the clip
  int32_t playing;
  int32_t piece_count;
  int32_t selected;       // -1 none
  int32_t can_undo;
  int32_t can_redo;
  int32_t edited;         // anything cut: Export has something to write
  int32_t strip_count;
  int32_t peak_count;
} mv_editor_view;
uint64_t mv_chrome_editor_generation(void);
bool mv_chrome_editor_view(mv_editor_view* out);
int32_t mv_chrome_editor_name(char* buf, int32_t size);
// Kept source ranges, (in, out) pairs, in program order; returns the count.
int32_t mv_chrome_editor_pieces(int64_t* pairs, int32_t cap_pairs);
// Thumbnail `index` (0 .. strip_count-1): RGBA8 into `rgba` (cap bytes);
// returns width * height * 4, or 0 when `cap` is short (width and height are
// still written). `source_ns` is the frame's own time.
int32_t mv_chrome_editor_thumb(int32_t index, uint8_t* rgba, int32_t cap, int32_t* width, int32_t* height,
                               int64_t* source_ns);
// The audio envelope across the clip (0..1); returns the count.
int32_t mv_chrome_editor_peaks(float* out, int32_t cap);
void mv_chrome_editor_seek(int64_t timeline_ns);
void mv_chrome_editor_toggle_play(void);
void mv_chrome_editor_step(int32_t frames);
// 1 split at the playhead, 2 delete the selected piece, 3 set in, 4 set out,
// 5 undo, 6 redo. A change that changes nothing beeps.
void mv_chrome_editor_edit(int32_t what);
void mv_chrome_editor_select(int32_t index);
// Queues keep_ranges on the Jobs pane: 0 keyframe cuts (instant), 1 exact.
void mv_chrome_editor_export(int32_t exact);
void mv_chrome_editor_close(void);

#ifdef __cplusplus
}
#endif
