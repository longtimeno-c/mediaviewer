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

#ifdef __cplusplus
}
#endif
