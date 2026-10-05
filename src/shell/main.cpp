// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// MediaViewer present lab — the Win32 entry point.
//
// This is the top-level window described in docs/design/02-architecture.md's shell/
// module. It owns the HWND and the window procedure, publishes an input
// snapshot, and does nothing else: no file I/O, no decode, no GPU waits, and no
// blocking on the core. The render thread lives in present_lab.
//
// Under the D1 amendment this window is the app, not a scaffold. PR 3 hosts
// WinUI 3 chrome inside it as XAML content islands.

#include <algorithm>
#include <windows.h>
#include <objbase.h>
#include <ole2.h>
#include <shellapi.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <shlobj.h>

#include "io/dir.h"

#include <cmath>
#include <atomic>
#include <chrono>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <cwchar>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <commctrl.h>  // LoadIconWithScaleDown (comctl32 v6 via app.manifest)

#include "abi/guard.h"
#include "shell/app_icon.h"
#include "core/trace.h"
#include "mediaviewer/mediaviewer.h"
#include "mediaviewer/mediaviewer_addon.h"
#include "mediaviewer/mediaviewer_clip.h"
#include "canvas/refinement.h"
#include "shell/adjust_pane.h"
#include "shell/browse_path.h"
#include "shell/chrome_host.h"
#include "shell/edit_session.h"
#include "shell/edit_view.h"
#include "shell/edit_workspace.h"
#include "shell/video_timeline.h"
#include "edit/clip_strip.h"
#include "shell/trim_state.h"
#include "shell/transport_autohide.h"
#include "shell/file_jobs.h"
#include "shell/key_router.h"
#include "core/job_system.h"
#include "edit/histogram.h"
#include "image/linear.h"
#include "io/file.h"
#include "io/file_port.h"
#include "io/sort_order.h"
#include "meta/meta.h"
#include "meta/tables.h"
#include "meta/write.h"
#include "core/json.h"
#include "shell/marks.h"
#include "shell/media_kind.h"
#include "shell/meta_store.h"
#include "shell/meta_writer.h"
#include "shell/open_request.h"
#include "shell/os_integration.h"
#include "shell/navigation.h"
#include "shell/slideshow.h"
#include "shell/present_lab.h"
#include "shell/settings.h"
#include "shell/shellext_install.h"
#include "shell/single_instance_win.h"
#include "shell/telemetry.h"
#include "shell/update_guard.h"
#include "shell/av_soak.h"
#include "shell/crash_reporter_win.h"

namespace {

using mv::shell::input_snapshot;
using mv::shell::lab_options;
using mv::shell::present_lab;

constexpr wchar_t kWindowClass[] = L"MediaViewer.PresentLab";
constexpr wchar_t kWindowTitle[] = L"MediaViewer";

// What the user asked for, which is not the same as what is on screen. A
// folder open is "browse this folder"; an image open is "show me this file",
// and the folder behind it is still listed so arrows and the gallery work
// (docs/design/10 PR 4 — one folder navigation model) without the strip taking a
// slice of the canvas the user did not ask to give up.
enum class open_mode { none, folder, image };

// docs/design/16 §Focus: in fullscreen, ↓ at fit (or the bottom hot-edge) shows the
// strips until navigation settles — this long after the last navigation.
constexpr UINT_PTR kMetaTimerId = 0x7501;   // PR 9: pause before a metadata read
constexpr UINT kMetaDebounceMs = 90;
constexpr UINT kMsgMetaReady = WM_APP + 0x71;  // a metadata read finished (any thread posts)
constexpr UINT_PTR kRotateTimerId = 0x7601;  // PR 10: the lossless write waits for the keys to stop
constexpr UINT kRotateDebounceMs = 400;
constexpr UINT kMsgEditJobDone = WM_APP + 0x72;  // a rotate write or an export finished (any thread posts)
constexpr UINT kMsgSiblingsReady = WM_APP + 0x73;  // parent listing for Ctrl+Left/Right (any thread posts)
// PR 11: the histogram waits for the sliders to settle; the working-image and
// histogram jobs post their results back as one message.
constexpr UINT_PTR kHistogramTimerId = 0x7701;
constexpr UINT kHistogramDebounceMs = 120;
constexpr UINT kMsgAdjustJobDone = WM_APP + 0x74;
// PR 15 (docs/design/10 "OS integration").
constexpr UINT kMsgFlattenDone = WM_APP + 0x76;     // Ctrl+Alt+C's bake finished (any thread posts)
constexpr UINT kMsgJumpListPruned = WM_APP + 0x77;  // folders the user removed from the jump list
constexpr UINT kMsgOpenForwarded = WM_APP + 0x78;   // a second instance handed over its paths
// One of our own drags ended. Posted, not handled inline, so a WM_DROPFILES
// the shell posted for that drop is seen (and refused) while the flag holds.
constexpr UINT kMsgOwnDragEnded = WM_APP + 0x7A;
constexpr UINT kThumbPrev = 0x5101;                 // taskbar thumbnail toolbar button ids
constexpr UINT kThumbPlay = 0x5102;
constexpr UINT kThumbNext = 0x5103;
// One identity for the process, its shortcuts (mediaviewer.iss [Icons]) and
// its jump list, so the pinned button, the running window and the recent
// folders are one taskbar entry. Never change it: pins are keyed on it.
constexpr wchar_t kAppUserModelId[] = L"MediaViewer.Viewer";
// PR 12: rating keys coalesce for a moment before the write; the write runs on
// the pool and posts its outcome back as one message.
constexpr UINT_PTR kMetaWriteTimerId = 0x7801;
constexpr UINT kRatingDebounceMs = 250;
constexpr UINT kCommentDebounceMs = 50;
constexpr UINT kMetaRetryMs = 200;  // a lossless rotation owns the file until it lands
constexpr UINT kMsgMetaWriteDone = WM_APP + 0x75;
constexpr ULONGLONG kNoticeMs = 3000;  // how long "★★★★☆" stays in the status line
constexpr UINT_PTR kRevealTimerId = 0x6B01;
constexpr UINT kRevealMs = 3000;
// Issue #38: one-shot, only while a clip plays with its transport up.
constexpr UINT_PTR kTransportTimerId = 0x6B02;
// view_fitted is the render thread's last pass; a `1` then ↓ inside one frame
// would read the old fit. Zoom commands open this window so the first ↓ after
// them pans instead of falling through.
constexpr ULONGLONG kZoomIntentMs = 250;
// docs/design/16 slideshow: a UI-thread tick that only decides whether to advance. It
// wakes the UI thread, never the render thread, so a still between advances is
// zero presents.
constexpr UINT_PTR kSlideshowTimerId = 0x6D01;
constexpr UINT kSlideshowTickMs = 100;
// docs/design/16 status line in the title bar. The tick only compares strings; the
// window text is written when it changes.
constexpr UINT_PTR kTitleTimerId = 0x6F01;
constexpr UINT kTitleTickMs = 250;
// PR 7 `;`: while a Live Photo's motion plays, a UI-thread tick watches for the
// end of the clip. Only armed while it plays, so a still is still zero work.
constexpr UINT_PTR kMotionTimerId = 0x7101;
constexpr UINT kMotionTickMs = 100;
// A motion clip that never opens (unreadable MOV) gives the still back.
constexpr ULONGLONG kMotionOpenGiveUpMs = 10000;
// PR 8 updater: a new version counts as "started" once the chrome attached and
// it stayed up this long (or exited in order). Until then trial.ini counts it.
constexpr UINT_PTR kUpdateConfirmTimerId = 0x7301;
constexpr UINT kUpdateConfirmMs = 10000;
// --browse-soak: time the arrow from one still to the next. A UI-thread tick
// only chooses the next index; the render thread records the present.
// Not 0x7701: that is kHistogramTimerId, checked first in WM_TIMER, which ate
// every soak tick.
constexpr UINT_PTR kBrowseTimerId = 0x7901;
constexpr UINT kBrowseTickMs = 50;
constexpr ULONGLONG kBrowseDwellMs = 3000;
constexpr ULONGLONG kBrowseStepTimeoutMs = 20000;
constexpr ULONGLONG kBrowseOpenTimeoutMs = 60000;

struct app_state {
  present_lab lab;
  input_snapshot input;
  // PR 9. Reads run on `jobs`, never here (rule 1); the record is the one the
  // overlays were last formatted from, so toggling them is not a file read.
  // PR 9 panes. The wish is kept here; apply_view_state decides what is on screen
  // (a pane hides under the gallery, fullscreen and Settings and comes back).
  bool meta_pane_visible = false;
  bool tree_visible = false;
  // One-shot: the next layout moves keyboard focus into that pane (I / Ctrl+Shift+E).
  bool focus_meta_next = false;
  bool focus_adjust_next = false;  // PR 11: Shift+A focuses the pane's first slider
  bool focus_tree_next = false;
  std::string current_dir;  // the open folder, for the tree's root
  // Milestone H: a result list (mv_folder_open_list, opened by the AI chrome)
  // is on screen instead of current_dir's listing. Its title is the last
  // breadcrumb; Up, Ctrl+Up and Esc from its gallery go back to current_dir
  // on the item that was open before (list_return_select).
  std::string list_title;
  std::string list_return_select;
  // PR 26: breadcrumb trail, gallery folder-tile cursor, auto-open for a
  // folder of folders. trail is string arithmetic, no I/O.
  mv::shell::browse_path trail;
  int folder_cursor = -1;     // >= 0: keyboard is on a folder tile
  int gallery_columns = 1;
  std::string gallery_if_empty_dir;
  std::string reveal_child;   // path of the folder we left, selected after Up
  std::vector<std::string> siblings;
  int sibling_index = -1;
  std::uint64_t sibling_generation = 0;
  bool folder_find = false;
  ULONGLONG folder_find_tick = 0;
  std::string folder_query;
  mv::job_system jobs;
  // PR 10 (docs/design/07, docs/design/16). `edits` owns every item's edit stack and crop
  // mode; the render thread gets the geometry through input.edit, tagged with
  // the item's path key and the view generation of the select that showed it.
  mv::shell::edit_session edits;
  std::string edit_path;
  std::uint64_t edit_key = 0;
  std::uint32_t edit_generation = 0;
  std::uint64_t edit_size = 0;
  std::int64_t edit_mtime = 0;
  // The export dialog's last answer, preselected next time (pack_export).
  std::int32_t export_choice = mv::shell::pack_export(mv::edit::export_options{});
  // PR 11 (docs/design/07, docs/design/16): the adjust pane's state and the preview-sized
  // FP16 working image the histogram reduces (the render thread holds its
  // own GPU copy). `adjust_generation` cancels a build for an item the user
  // has left (the job's job_context watches it), so a RAW develop never
  // finishes for a photo no longer on screen.
  mv::shell::adjust_pane adjust;
  std::shared_ptr<const mv::image::linear_image> working;
  std::atomic<mv::generation> adjust_generation{1};
  // PR 13 / 14 (docs/design/08): trim mode on the current clip, the keyframe-index
  // request in flight for it, and the Jobs pane's wish. The jobs themselves
  // are the session's clip queue (mediaviewer_clip.h); the pane polls it.
  mv::shell::trim_state trim;
  std::uint64_t trim_index_request = 0;
  bool jobs_pane_visible = false;
  bool focus_jobs_next = false;
  // PR 29 (docs/design/20): the Edit workspace (shell/edit_workspace.h, shared with
  // the Mac host) and Show original. `ws_shown` is what the panes were last
  // synced to, so closing the workspace closes only the panes it opened.
  mv::shell::edit_workspace ws;
  bool ws_shown = false;
  bool show_original = false;
  // PR 30 (docs/design/21): the Video Editor window. While it is open it owns the
  // canvas (input.canvas_window) and the keys aimed at it. `token` bumps on
  // open and close, so a strip job that lands for an older clip is dropped;
  // `retired` is a closed window waiting for the swapchain to leave it.
  struct video_editor {
    HWND window = nullptr;
    HWND retired = nullptr;
    int release_tries = 0;
    bool open = false;
    bool active = false;
    bool minimized = false;
    std::string path;
    mv::shell::video_timeline timeline;
    std::int32_t selected = -1;
    std::uint64_t generation = 0;
    std::uint64_t token = 0;
    std::vector<mv::edit::clip::strip_frame> strip;
    std::vector<float> peaks;
    std::int64_t last_seek = -1;
    std::int64_t pushed_playhead = -1;
    bool pushed_playing = false;
    double fps = 0;                         // the clip's rate; 0 unknown
    std::uint64_t exported_revision = 0;    // the timeline revision last exported
    mv::shell::editor_shuttle shuttle;      // J K L
    std::int64_t shuttle_target = 0;        // where the last J was going (program time)
    bool shuttle_skimmed = false;           // a held J left a keyframe seek to settle
    bool rate_changed = false;              // L sped the clip up: 1x again on close
    std::int32_t trim_index = -1;           // the piece whose edge is being dragged
    bool trim_in = true;
    bool close_prompt = false;              // the discard question is up
  } editor;
  bool main_active = true;  // the viewer's own WM_ACTIVATE (input.window_active also counts the editor)
  mv::shell::meta_store meta;
  std::shared_ptr<const mv::meta::metadata> meta_record;
  // PR 12 (docs/design/06 "Writing", docs/design/16 Rate). Rating, comment and revert are
  // queued here and written on `jobs`: a plain JPEG in place, everything else
  // in an XMP sidecar. `meta_written` is the paths written this session, the
  // ones Revert has a snapshot for. The notice rides the title's status line.
  mv::shell::meta_writer meta_writer;
  std::set<std::string> meta_written;
  bool focus_comment_next = false;  // Ctrl+I: the next pane push focuses the comment
  std::wstring notice;
  ULONGLONG notice_until = 0;
  // Pages of the selected stop (docs/plans/audio-and-documents.md §2.3): the page
  // on screen, the folder index it belongs to (another stop starts at page 0)
  // and the last page count the core reported for it (0 unknown).
  std::uint32_t page = 0;
  std::uint32_t page_of = UINT32_MAX;
  std::uint32_t page_count = 0;
  mv_session_t session = nullptr;
  bool tracking_mouse = false;
  bool chrome_enabled = true;
  bool chrome_on_screen = false;  // reserved bar height; cleared if attach fails
  // Launch: the core half of the argv open (mv_folder_open), already issued
  // before the chrome attached. open_folder skips that one call for it.
  std::string early_open_dir;
  std::string early_open_select;
  open_mode mode = open_mode::none;
  bool gallery_visible = false;
  // Issue #44: what the core was last told (mv_video_set_hold). The grid covers
  // the canvas, so nothing plays under it; see sync_video_hold.
  bool video_held = false;
  // WM_CLOSE has started the orderly teardown; a second close is a no-op.
  bool closing = false;
  // File-job problems waiting to be reported. One dialog at a time: a job that
  // finishes while it is up is folded in and reported after it closes.
  struct file_report {
    std::size_t total = 0;
    std::size_t refused = 0;
    std::size_t failed = 0;
    mv::shell::file_job_kind kind = mv::shell::file_job_kind::copy;
    bool showing = false;
  } report;
  // Set by the island's playback poll (chrome_cmd_video_active). The transport
  // strip follows it, so it appears with a clip and leaves with it.
  bool video_on = false;
  // Issue #38: the transport's idle state (shell/transport_autohide.h, the same
  // rule the Mac host runs). `video_playing` rides on chrome_cmd_video_active;
  // `transport_hold` is chrome_cmd_transport_hold (a scrub, the More flyout).
  mv::shell::transport_autohide autohide;
  bool video_playing = false;
  bool transport_hold = false;
  bool transport_timer = false;
  // Where a skim burst is heading, as opposed to where the clip currently is.
  // A non-exact seek lands on the preceding keyframe, so re-reading the
  // position each repeat asks to move 2 s from a point the last press already
  // rounded backwards — five presses on a 4 s GOP moved one GOP. Intent has to
  // accumulate; only the landing is quantised.
  std::int64_t skim_target_ns = 0;
  std::uint64_t skim_tick_ms = 0;
  // Q/E are two commands on one key: tap steps the speed, hold skims. Which
  // one it was is only knowable at key-up, so the down edge records and the up
  // edge decides.
  bool skim_shuttled = false;
  int  rate_index = 2;  // kRateLadder: 1.00x
  float volume = 1.0f;  // 0..1, Up / Down on a clip
  bool muted = false;   // Shift+M; a fresh clip starts unmuted
  mv::shell::view_settings settings;
  HWND window = nullptr;
  mv::shell::chrome_host chrome;
  mv::shell::key_router router;
  // Which island last reported focus (chrome_cmd_focus_changed). Only read
  // when GetFocus() is not the canvas window, so it cannot go stale there.
  mv::shell::focus_kind island_focus = mv::shell::focus_kind::command_bar;
  // docs/design/16 `F`: borderless on the window's monitor, chrome hidden. The
  // windowed placement and style come back exactly on the way out.
  bool fullscreen = false;
  // `length` is refreshed immediately before GetWindowPlacement. Value-
  // initialise the whole aggregate here so clang-cl's missing-field check is
  // not tripped by the old one-member aggregate initialiser.
  WINDOWPLACEMENT windowed_placement{};
  LONG_PTR windowed_style = 0;
  bool topmost = false;  // Ctrl+Shift+A
  bool popup_open = false;       // a `?` / go-to / find flyout is up
  bool settings_open = false;    // settings screen covering the canvas
  bool game_on = false;          // Space on an empty window started the runner
  bool file_drag_armed = false;
  int welcome_press = -1;        // the welcome card's recent row under the left press
  bool welcome_press_remove = false;  // ... and the press was on its remove button
  int file_drag_x = 0;
  int file_drag_y = 0;
  std::wstring last_title;       // the status line last written to the title bar
  bool fullscreen_reveal = false;  // strips shown over a fullscreen canvas for a while
  ULONGLONG zoom_intent_tick = 0;  // GetTickCount64 of the last zoom-in style command
  // docs/design/16 marks, copy, move. Marks are UI-thread state keyed by path; the
  // file work runs on files' own I/O worker and reports back by message.
  mv::shell::mark_set marks;
  mv::shell::file_jobs files;
  std::vector<std::string> destinations;  // F7 / F8, most recent first
  // PR 15: the jump list's recent folders (settings.ini [recent]), most recent first.
  std::vector<std::string> recent_folders;
  // The user's profile folder (FOLDERID_Profile) as UTF-8, read once: the
  // welcome card writes it as "~" (the Mac passes NSHomeDirectory()).
  std::string home_utf8;
  // One of our own file drags is in flight (the canvas's, or a gallery /
  // filmstrip cell's). Our own drop targets refuse it rather than reopening
  // the folder it came from; the Mac's cells return no operation in-app.
  bool own_drag = false;
  // Soaks and scripted runs open fixtures, not the user's folders: they never
  // reach settings.ini [recent] or the jump list.
  bool record_recent = true;
  // PR 15: the taskbar thumbnail toolbar (prev / play-pause / next). Created
  // when Explorer says the button exists; `thumb_state` is what it shows:
  // -1 not yet, 0 a still (play disabled), 1 a paused clip, 2 a playing one.
  UINT taskbar_created_msg = 0;
  ITaskbarList3* taskbar = nullptr;
  HICON thumb_icons[4]{};  // prev, play, pause, next
  int thumb_state = -1;
  // PR 15: the single instance. A second start hands its paths over here.
  mv::shell::instance_listener instance;
  std::uint64_t folder_token = 0;         // bumped per folder open
  // docs/design/16 slideshow, a mode: order and interval in `show`, advancing through
  // the same folder_select as browse.
  mv::shell::slideshow show;
  ULONGLONG show_last_advance = 0;
  bool show_entered_fullscreen = false;  // leave fullscreen again on stop
  // PR 7 `;`: a Live Photo's motion is playing over its still. Any navigation
  // (a new generation) retires the clip and clears this; the end of the clip,
  // Esc or `;` again re-selects the stop so the still comes back from the LRU.
  bool motion_playing = false;
  ULONGLONG motion_started = 0;
};

// PR 8 updater (shell/update_guard.h). Set once at startup on the UI thread.
mv::shell::update::install_layout g_install;
// Set on the UI thread when the confirm is STARTED. The worker below may not
// have finished when the process exits, which is why the exit path watches the
// atomic rather than this.
bool g_start_confirmed = false;
// Set by the confirm worker once trial.ini has actually been written. That
// worker is detached, so without this a process that exits soon after the 10 s
// timer leaves the record armed - and a perfectly good version started and
// closed quickly three times is rolled back for nothing.
std::atomic<bool> g_start_confirm_done{false};
// What an update restart asked us to put back (--restore-*). Applied once.
struct pending_restore {
  unsigned zoom_percent = 0;
  bool fullscreen = false;
  bool gallery = false;
} g_restore;
// PR 15: `--new-instance` runs a second, independent window (docs/design/09
// "overridable"); without it a second start hands its paths to the first.
bool g_new_instance = false;

// --browse-soak. Neighbours of the open photo are decoded ahead (±1, ±2, no
// wrap). Cold jumps are the photos past that window, taken before the walk
// visits them. Warm steps are Right after a dwell, so the next photo has had
// time to be decoded. Quick steps are Right as soon as the last step is on
// screen, from the first photo again: each lands on a neighbour whose
// prefetch is usually still running, the case the decode hand-off is for.
// Held steps are a held Right key: one step per tick without waiting for
// anything, then the last photo is timed to full resolution, and the job
// counts over the run say how much decode work the walk threw away.
// The clock is the render thread's, not this tick.
enum class browse_kind { warm, cold, quick, held };
const char* browse_kind_name(browse_kind k) noexcept {
  switch (k) {
    case browse_kind::cold: return "cold";
    case browse_kind::quick: return "quick";
    case browse_kind::held: return "held";
    case browse_kind::warm: break;
  }
  return "warm";
}
struct browse_row {
  char name[200]{};
  int index = 0;
  browse_kind kind = browse_kind::warm;
  int cached = 0;
  int timed_out = 0;
  double ready_ms = -1.0;
  double present_ms = -1.0;
  double refresh_ms = 0.0;
  double full_ms = -1.0;
};
enum class browse_phase {
  wait_media, dwell, cold, wait_away, go_home, wait_home, warm, wait_warm,
  quick_home, wait_quick_home, quick, wait_quick, held_home, wait_held_home, held, wait_held,
  finish
};
struct browse_run {
  bool enabled = false;
  std::wstring json_path;
  browse_phase step = browse_phase::wait_media;
  browse_phase after_dwell = browse_phase::cold;
  bool started = false;
  ULONGLONG tick0 = 0;
  ULONGLONG phase_tick = 0;
  std::uint32_t count = 0;
  int cold_targets[6]{};
  int cold_n = 0;
  int cold_i = 0;
  int warm_left = 0;
  int quick_left = 0;
  int held_left = 0;
  int held_steps = 0;
  mv_job_stats held_before{};
  mv_job_stats held_after{};
  bool held_done = false;
  std::uint64_t seq = 0;
  bool record = false;
  int pending_index = 0;
  browse_kind pending_kind = browse_kind::warm;
  char pending_name[200]{};
  browse_row rows[32]{};
  int nrows = 0;
} g_browse;

void confirm_update_start_async() noexcept {
  if (g_start_confirmed || !g_install.installed()) return;
  g_start_confirmed = true;
  try {
    // A small file write and a registry delete: off the UI thread (rule 1).
    // The uninstall-entry sweep rides here because this is the first moment
    // after an update at which Velopack has finished writing its own entry.
    std::thread([layout = g_install] {
      mv::shell::update::confirm_started(layout);
      g_start_confirm_done.store(true, std::memory_order_release);
      (void)mv::shell::update::remove_velopack_uninstall_entry(layout);
    }).detach();
  } catch (...) {
  }
}

app_state* state_from(HWND hwnd) noexcept {
  return reinterpret_cast<app_state*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// The welcome card lists recent folders only until something opens (the Mac
// host's -welcomeListsRecents). Every open route moves `mode` off `none`.
bool welcome_lists_recents(const app_state* app) noexcept {
  return app->mode == open_mode::none && app->early_open_dir.empty() && app->record_recent;
}

void publish(app_state* app) noexcept {
  // The card's rows go the moment anything opens, whichever route opened it.
  if (app->input.recents.count != 0 && !welcome_lists_recents(app)) {
    app->input.recents.count = 0;
    app->input.recents.hover = -1;
    app->input.recents.hover_remove = false;
    ++app->input.activity_seq;
  }
  app->lab.publish(app->input);
  app->lab.wake();
}

// The rows from app->recent_folders; one redraw when they change.
void refresh_welcome_recents(app_state* app) noexcept {
  mv::shell::welcome_recents next;
  if (welcome_lists_recents(app)) mv::shell::fill_welcome_recents(app->recent_folders, app->home_utf8, next);
  if (std::memcmp(&next, &app->input.recents, sizeof(next)) == 0) return;
  app->input.recents = next;
  ++app->input.activity_seq;
  publish(app);
}

// The recent row under the pointer, hit-tested on the layout the render
// thread draws (welcome_layout.h); -1 when the card lists none. `on_remove`
// says whether the pointer is on that row's remove button.
int welcome_row_at_pointer(const app_state* app, bool* on_remove = nullptr) noexcept {
  if (on_remove) *on_remove = false;
  const auto& in = app->input;
  if (in.recents.count == 0 || !in.mouse_in_client || app->game_on || !welcome_lists_recents(app)) {
    return -1;
  }
  const float scale = in.dpi_scale > 0.0f ? in.dpi_scale : 1.0f;
  const mv::shell::welcome_geometry g =
      mv::shell::layout_welcome(static_cast<float>(in.width), static_cast<float>(in.height),
                                static_cast<float>(in.chrome_height_px), scale, in.recents.count);
  const int row = mv::shell::welcome_row_at(g, in.mouse_x, in.mouse_y);
  if (row >= 0 && on_remove) *on_remove = mv::shell::welcome_on_remove(g, in.mouse_x);
  return row;
}

// The drawn hover (row and remove button) from the pointer. The caller publishes.
void update_welcome_hover(app_state* app) noexcept {
  bool on_remove = false;
  app->input.recents.hover = static_cast<std::int8_t>(welcome_row_at_pointer(app, &on_remove));
  app->input.recents.hover_remove = on_remove;
}

// The settings word the island sees: view_settings plus [update] auto_check
// and channel plus the two [telemetry] bits. One place, so both switches ride the existing
// ApplySettings push.
std::int32_t chrome_flags(const app_state* app) noexcept {
  const bool auto_check = mv::shell::app_settings().get_int("update", "auto_check", 1) != 0;
  std::int32_t flags = app->settings.flags();
  if (auto_check) flags |= mv::shell::update::kChromeFlagUpdateAutoCheck;
  if (mv::shell::app_settings().get("update", "channel") == "preview")
    flags |= mv::shell::update::kChromeFlagUpdatePreview;
  // Default off, and the island shows the first-run screen exactly while
  // `asked` is clear (docs/design/13 Part 3).
  if (mv::shell::telemetry::enabled()) flags |= mv::shell::telemetry::kChromeFlagTelemetry;
  if (mv::shell::telemetry::asked()) flags |= mv::shell::telemetry::kChromeFlagTelemetryAsked;
  return flags;
}

std::string utf8_from_wide(std::wstring_view wide) {
  if (wide.empty()) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                      nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string out(static_cast<std::size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n,
                        nullptr, nullptr);
  return out;
}

std::wstring wide_from_utf8(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                      nullptr, 0);
  if (n <= 0) return {};
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
  return out;
}

void apply_view_state(app_state* app) noexcept;
void apply_transport_autohide(app_state* app) noexcept;
void transport_activity(app_state* app) noexcept;
void push_browse_state(app_state* app);
void set_gallery(app_state* app, bool visible);
void sync_video_hold(app_state* app, bool resume = true) noexcept;
void push_tree_root(app_state* app) noexcept;
void push_meta_pane(app_state* app) noexcept;
void update_title(app_state* app) noexcept;
void layout_chrome(app_state* app) noexcept;
bool run_command(app_state* app, mv::shell::command_id command) noexcept;
void note_recent_folder(app_state* app, const std::string& utf8_dir);
void open_welcome_row(app_state* app, int row);
void remove_welcome_row(app_state* app, int row);
void push_recent_folders(app_state* app);
void trim_item_opened(app_state* app) noexcept;
void set_jobs_pane(app_state* app, bool on, bool focus = true) noexcept;
void focus_canvas(app_state* app) noexcept;
void reveal_current_in_explorer(app_state* app) noexcept;
void begin_file_drag(app_state* app, HWND hwnd, const std::string& utf8) noexcept;
void open_dropped_wide_list(app_state* app, std::wstring_view blob) noexcept;
void persist_live_keys() noexcept;
void publish_command_table(app_state* app) noexcept;
void set_settings_open(app_state* app, bool on) noexcept;
void refresh_contributed_commands(app_state* app) noexcept;
void stop_motion(app_state* app) noexcept;
// PR 29 (docs/design/20): the Edit workspace.
void push_edit_view(app_state* app) noexcept;
void workspace_item_changed(app_state* app) noexcept;
void set_editor_open(app_state* app, bool open);  // PR 30

std::string subfolder_path_at(app_state* app, std::uint32_t index);
std::string subfolder_name_at(app_state* app, std::uint32_t index);
void seed_siblings_for(app_state* app, const std::string& dir);
void refresh_siblings(app_state* app);
bool navigate_sibling(app_state* app, int delta);
int chrome_bar_px(app_state* app, std::uint32_t dpi) noexcept;

void open_folder(app_state* app, std::wstring_view wide_dir, std::wstring_view wide_select,
                 bool navigation = false) {
  if (!app || !app->session || wide_dir.empty()) return;
  const std::string dir = utf8_from_wide(wide_dir);
  if (dir.empty()) return;
  const std::string select = utf8_from_wide(wide_select);
  // Moving up into a folder that contains the one we are leaving: remember
  // that child so its tile is selected when the parent listing arrives.
  if (navigation && !app->current_dir.empty() && dir != app->current_dir &&
      mv::shell::browse_path::within(app->current_dir, dir)) {
    app->reveal_child = app->current_dir;
  } else {
    app->reveal_child.clear();
  }
  seed_siblings_for(app, dir);
  app->folder_find = false;
  app->folder_query.clear();
  // A directory ends a result list (mediaviewer.h 0.14).
  app->list_title.clear();
  app->list_return_select.clear();
  const bool opened_early =
      !navigation && dir == app->early_open_dir && select == app->early_open_select;
  app->early_open_dir.clear();
  app->early_open_select.clear();
  if (!opened_early) {
    uint64_t job_id = 0;
    (void)mv_folder_open(app->session, dir.c_str(), select.empty() ? nullptr : select.c_str(),
                         &job_id);
  }
  ++app->folder_token;
  app->current_dir = dir;
  app->folder_cursor = -1;
  app->gallery_if_empty_dir = dir;
  if (navigation) app->trail.visit(dir);
  else app->trail.reset(dir);
  push_tree_root(app);
  push_browse_state(app);
  ++app->input.activity_seq;
  publish(app);
  apply_view_state(app);
  layout_chrome(app);
}

void open_path(app_state* app, std::wstring_view wide_path, bool navigation = false) {
  if (!app || wide_path.empty()) return;
  const std::string utf8 = utf8_from_wide(wide_path);
  if (utf8.empty()) return;
  auto dir = mv::io::is_directory(utf8);
  if (dir && dir.value()) {
    if (!navigation) {
      app->mode = open_mode::folder;
      app->gallery_visible = false;
      sync_video_hold(app, false);
      note_recent_folder(app, utf8);
    } else {
      app->mode = open_mode::folder;
    }
    open_folder(app, wide_path, {}, navigation);
    return;
  }
  if (navigation) return;
  app->mode = open_mode::image;
  app->gallery_visible = false;
  sync_video_hold(app, false);
  const auto slash = wide_path.find_last_of(L"\\/");
  if (slash == std::wstring_view::npos) {
    mv_session_bump_generation(app->session, nullptr);
    uint64_t job_id = 0;
    (void)mv_image_open(app->session, utf8.c_str(), &job_id);
    ++app->input.activity_seq;
    publish(app);
    return;
  }
  // A file at a drive root keeps the root's separator: "D:" alone is the
  // drive's current directory, not its root.
  std::wstring parent(wide_path.substr(0, slash));
  if (parent.size() == 2 && parent[1] == L':') parent.push_back(L'\\');
  note_recent_folder(app, utf8_from_wide(parent));
  open_folder(app, parent, wide_path);
}

// argv and drag-and-drop (docs/design/16): the first entry that exists wins — a folder
// opens, a file opens its folder with that file selected (open_request.h).
// The attribute probe is the same one-stat-per-path open_path already makes.
mv::shell::open_request resolve_paths(const std::vector<std::wstring>& raw) {
  std::vector<mv::shell::path_probe> probes;
  probes.reserve(raw.size());
  for (const auto& r : raw) {
    mv::shell::path_probe probe;
    probe.path = mv::shell::normalize_open_path(r);
    if (probe.path.empty()) continue;
    const DWORD attr = ::GetFileAttributesW(probe.path.c_str());
    probe.exists = attr != INVALID_FILE_ATTRIBUTES;
    probe.is_directory = probe.exists && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    probes.push_back(std::move(probe));
  }
  return mv::shell::resolve_open(probes);
}

// Launch only, before attach_chrome: the WinUI islands take ~0.4 s to load,
// and nothing in the core open needs them. This issues the same
// mv_folder_open that open_paths -> open_folder would (the folder, or the
// file's folder with the file selected), so the scan and the file's decode
// run while the chrome loads. The host half - mode, trail, chrome state -
// still happens in open_paths once the chrome is up, and skips this call.
void open_paths_early(app_state* app, const std::vector<std::wstring>& raw) {
  if (!app || !app->session) return;
  const auto request = resolve_paths(raw);
  std::wstring_view dir;
  std::wstring_view select;
  if (request.kind == mv::shell::open_kind::folder) {
    dir = request.path;
  } else if (request.kind == mv::shell::open_kind::file) {
    const auto slash = request.path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;  // open_path's bare-name route; not a folder open
    dir = std::wstring_view(request.path).substr(0, slash);
    select = request.path;
  } else {
    return;
  }
  const std::string utf8_dir = utf8_from_wide(dir);
  if (utf8_dir.empty()) return;
  const std::string utf8_select = utf8_from_wide(select);
  uint64_t job_id = 0;
  if (mv_folder_open(app->session, utf8_dir.c_str(),
                     utf8_select.empty() ? nullptr : utf8_select.c_str(), &job_id) != MV_OK) {
    return;
  }
  app->early_open_dir = utf8_dir;
  app->early_open_select = utf8_select;
}

void open_paths(app_state* app, const std::vector<std::wstring>& raw) {
  if (!app) return;
  const auto request = resolve_paths(raw);
  switch (request.kind) {
    case mv::shell::open_kind::folder:
    case mv::shell::open_kind::file:
      open_path(app, request.path);
      return;
    case mv::shell::open_kind::addon_package:
      // docs/design/25: never the viewer. Settings opens and the chrome asks; the
      // package is read on a worker, by the core.
      set_settings_open(app, true);
      if (!app->chrome.offer_addon(utf8_from_wide(request.path))) ::MessageBeep(MB_ICONWARNING);
      return;
    case mv::shell::open_kind::missing:
      MV_LOG_WARN("open: none of the %zu requested paths exists", raw.size());
      ::MessageBeep(MB_ICONWARNING);
      return;
    case mv::shell::open_kind::none:
      return;
  }
}

bool pick_folder(HWND hwnd, std::wstring& out) {
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg)))) {
    return false;
  }
  FILEOPENDIALOGOPTIONS opt{};
  dlg->GetOptions(&opt);
  dlg->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  const HRESULT shown = dlg->Show(hwnd);
  if (shown != S_OK) {
    dlg->Release();
    return false;
  }
  IShellItem* item = nullptr;
  if (FAILED(dlg->GetResult(&item))) {
    dlg->Release();
    return false;
  }
  PWSTR path = nullptr;
  if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
    out = path;
    ::CoTaskMemFree(path);
  }
  item->Release();
  dlg->Release();
  return !out.empty();
}

void open_file_dialog(app_state* app, HWND hwnd) {
  wchar_t file[MAX_PATH]{};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd;
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter = L"Photos and video\0*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.webp;*.mp4;*.mov;*.mkv;*.webm;*.avi;*.ts\0All files\0*.*\0";
  ofn.nFilterIndex = 1;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
  if (::GetOpenFileNameW(&ofn)) open_path(app, file);
  focus_canvas(app);
}

void open_folder_dialog(app_state* app, HWND hwnd) {
  std::wstring folder;
  if (!pick_folder(hwnd, folder)) {
    focus_canvas(app);
    return;
  }
  // Picking a folder is the same intent as one on the command line or dropped
  // on the window: "browse this folder". open_path sets the mode for those two
  // routes; this one has to set it as well. Leaving it at `none` is not a
  // cosmetic slip — apply_view_state has no preference to consult for `none`,
  // so the strip stays off for a folder the user explicitly asked for, and T
  // then only flips the persisted flag behind an unchanged screen.
  app->mode = open_mode::folder;
  app->gallery_visible = false;
  sync_video_hold(app, false);
  open_folder(app, folder, {});
  focus_canvas(app);
}

using folder_string_fn = mv_status (MV_CALL*)(mv_session_t, std::uint32_t, char*, std::uint32_t,
                                              std::uint32_t*);

// A folder item string (UTF-8), or empty. UI thread; a copy out of the folder
// model, no I/O.
std::string folder_string_at(app_state* app, std::uint32_t index, folder_string_fn fn) {
  if (!app || !app->session) return {};
  char stack_buf[1024];
  std::uint32_t bytes = 0;
  if (fn(app->session, index, stack_buf, sizeof(stack_buf), &bytes) != MV_OK) {
    return {};
  }
  if (bytes < sizeof(stack_buf)) {
    stack_buf[sizeof(stack_buf) - 1] = '\0';
    return std::string(stack_buf);
  }
  // A long path: ask again with room for it.
  std::string out(static_cast<std::size_t>(bytes) + 1, '\0');
  if (fn(app->session, index, out.data(), bytes + 1, &bytes) != MV_OK) return {};
  out.resize(std::char_traits<char>::length(out.c_str()));
  return out;
}

// The stop's primary file (the JPEG of a RAW+JPEG, the still of a Live Photo).
std::string item_path_at(app_state* app, std::uint32_t index) {
  return folder_string_at(app, index, &mv_folder_item_path);
}

// The other half of a paired stop, or empty (PR 7).
std::string item_pair_path_at(app_state* app, std::uint32_t index) {
  return folder_string_at(app, index, &mv_folder_item_pair_path);
}

// The selected stop's index, if there is one.
bool selected_index(app_state* app, std::uint32_t& out) noexcept {
  if (!app || !app->session) return false;
  std::uint32_t count = 0;
  if (mv_folder_count(app->session, &count) != MV_OK || count == 0) return false;
  if (mv_folder_selected(app->session, &out) != MV_OK || out >= count) return false;
  return true;
}

std::string current_item_path(app_state* app) {
  std::uint32_t selected = 0;
  if (!selected_index(app, selected)) return {};
  return item_path_at(app, selected);
}

// MV_PAIR_* of the selected stop; MV_PAIR_NONE with nothing selected.
std::uint32_t current_pair_kind(app_state* app) noexcept {
  std::uint32_t selected = 0;
  if (!selected_index(app, selected)) return MV_PAIR_NONE;
  mv_folder_item rec{};
  if (mv_folder_item_at(app->session, selected, &rec) != MV_OK) return MV_PAIR_NONE;
  return rec.pair_kind;
}

// PR 7: a paired stop is two files on disk. Copy, move and delete act on both
// halves — deleting only the JPEG would bring its RAW back as a stop of its own
// on the next relist. Each primary is followed by its secondary. UI thread; a
// walk of the in-memory listing on a key press, no I/O.
std::vector<std::string> expand_pair_targets(app_state* app, std::vector<std::string> targets) {
  std::map<std::string, std::string, std::less<>> pairs;
  std::uint32_t count = 0;
  if (app && app->session && mv_folder_count(app->session, &count) == MV_OK) {
    for (std::uint32_t i = 0; i < count; ++i) {
      mv_folder_item rec{};
      if (mv_folder_item_at(app->session, i, &rec) != MV_OK || rec.pair_kind == MV_PAIR_NONE) {
        continue;
      }
      std::string secondary = item_pair_path_at(app, i);
      if (!secondary.empty()) pairs.emplace(item_path_at(app, i), std::move(secondary));
    }
  }
  if (pairs.empty()) return targets;
  std::vector<std::string> out;
  out.reserve(targets.size() * 2);
  for (auto& t : targets) {
    const auto it = pairs.find(t);
    out.push_back(std::move(t));
    if (it != pairs.end()) out.push_back(it->second);
  }
  return out;
}

// docs/design/16 Ctrl+E: open the containing folder with this file selected, so a
// culling pass can jump to Explorer without copying the path.
void reveal_current_in_explorer(app_state* app) noexcept {
  if (!app) return;
  const std::string utf8 = current_item_path(app);
  if (utf8.empty()) {
    ::MessageBeep(MB_ICONWARNING);
    return;
  }
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
  if (n <= 1) {
    ::MessageBeep(MB_ICONWARNING);
    return;
  }
  std::wstring wide(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), n);
  PIDLIST_ABSOLUTE pidl = ::ILCreateFromPathW(wide.c_str());
  if (!pidl) {
    ::MessageBeep(MB_ICONWARNING);
    return;
  }
  (void)::SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
  ::ILFree(pidl);
  // Explorer may have taken the foreground. If we still own it, put keys
  // back on the canvas — otherwise A/D wait for deactivate/reactivate.
  if (app->window && ::GetForegroundWindow() == app->window) focus_canvas(app);
}

// Our own drag is over. The flag drops once the queue has drained past it
// (kMsgOwnDragEnded), so a WM_DROPFILES or island drop posted for that very
// drop still finds it set.
void end_own_drag(app_state* app) noexcept {
  if (!app || !app->own_drag) return;
  if (app->window && ::PostMessageW(app->window, kMsgOwnDragEnded, 0, 0)) return;
  app->own_drag = false;
  app->chrome.set_drag_paths({}, false);
}

// %USERPROFILE% as UTF-8, from the known folder rather than the environment.
std::string profile_folder_utf8() {
  PWSTR path = nullptr;
  std::string out;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DEFAULT, nullptr, &path)) && path) {
    out = utf8_from_wide(path);
  }
  ::CoTaskMemFree(path);
  return out;
}

// Shell IDataObject for the file, so Explorer / other apps receive a real
// CF_HDROP. Modal; the UI thread is inside OLE's drag loop until drop or Esc.
// Copy only (the Mac returns NSDragOperationCopy): a link or a move would let
// Explorer take the original out of the folder or leave a shortcut to it.
// Our own window and islands refuse the drop while it is in flight.
void begin_file_drag(app_state* app, HWND hwnd, const std::string& utf8) noexcept {
  if (!app || !hwnd || utf8.empty()) return;
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
  if (n <= 1) return;
  std::wstring wide(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), n);
  IShellItem* item = nullptr;
  if (FAILED(::SHCreateItemFromParsingName(wide.c_str(), nullptr, IID_PPV_ARGS(&item))) ||
      !item) {
    return;
  }
  IDataObject* data = nullptr;
  const HRESULT hr =
      item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
  item->Release();
  if (FAILED(hr) || !data) return;
  app->own_drag = true;
  app->chrome.set_drag_paths({}, true);
  DWORD effect = DROPEFFECT_COPY;
  (void)::SHDoDragDrop(hwnd, data, nullptr, DROPEFFECT_COPY, &effect);
  data->Release();
  end_own_drag(app);
}

// Paths from a XAML island drop (WM_COPYDATA), newline-separated UTF-16.
void open_dropped_wide_list(app_state* app, std::wstring_view blob) noexcept {
  if (!app || blob.empty()) return;
  std::vector<std::wstring> paths;
  std::wstring cur;
  for (wchar_t c : blob) {
    if (c == L'\0') break;
    if (c == L'\n' || c == L'\r') {
      if (!cur.empty()) paths.push_back(std::move(cur));
      cur.clear();
      continue;
    }
    cur.push_back(c);
  }
  if (!cur.empty()) paths.push_back(std::move(cur));
  if (!paths.empty()) open_paths(app, paths);
}

// The marked badge. Free when nothing is marked, so key-repeat navigation with
// no marks does not look anything up.
void refresh_mark_state(app_state* app) {
  if (!app) return;
  if (app->marks.empty()) {
    app->input.item_marked = false;
    app->input.marked_count = 0;
    return;
  }
  const std::string current = current_item_path(app);
  app->input.item_marked = !current.empty() && app->marks.contains(current);
  app->input.marked_count = static_cast<std::uint32_t>(app->marks.size());
}

// The `O` line's name and position. UI thread, only while the overlay is on, so
// key-repeat navigation with it off costs nothing extra. The render thread
// reads the copy in the snapshot and never calls the folder model.
void refresh_item_info(app_state* app) noexcept {
  if (!app || !app->session || !app->input.info_overlay) return;
  app->input.item_name[0] = '\0';
  app->input.item_count = 0;
  app->input.item_index = 0;
  std::uint32_t count = 0;
  std::uint32_t selected = 0;
  if (mv_folder_count(app->session, &count) != MV_OK || count == 0) return;
  if (mv_folder_selected(app->session, &selected) != MV_OK || selected >= count) return;
  app->input.item_count = count;
  app->input.item_index = selected;
  const auto cap = static_cast<std::uint32_t>(sizeof(app->input.item_name));
  std::uint32_t bytes = 0;
  if (mv_folder_item_name(app->session, selected, app->input.item_name, cap, &bytes) != MV_OK) {
    app->input.item_name[0] = '\0';
  }
  app->input.item_name[cap - 1] = '\0';
}

// ---- PR 9: metadata for the overlays ---------------------------------------

bool metadata_wanted(const app_state* app) noexcept {
  return app->input.info_overlay || app->input.af_points || app->meta_pane_visible;
}

// The selected item as the store keys it: path + mtime + size, from one stat.
bool current_dir_entry(app_state* app, mv::io::dir_entry& out) {
  const std::string utf8 = current_item_path(app);
  if (utf8.empty()) return false;
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
  if (n <= 1) return false;
  std::wstring wide(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), n);
  WIN32_FILE_ATTRIBUTE_DATA fa{};
  if (!::GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &fa)) return false;
  ULARGE_INTEGER size{};
  size.LowPart = fa.nFileSizeLow;
  size.HighPart = fa.nFileSizeHigh;
  ULARGE_INTEGER ft{};
  ft.LowPart = fa.ftLastWriteTime.dwLowDateTime;
  ft.HighPart = fa.ftLastWriteTime.dwHighDateTime;
  out.path_utf8 = utf8;
  const std::size_t slash = utf8.find_last_of("\\/");
  out.name_utf8 = slash == std::string::npos ? utf8 : utf8.substr(slash + 1);
  out.size = size.QuadPart;
  out.mtime_unix = static_cast<std::int64_t>(ft.QuadPart / 10000000ULL) - 11644473600LL;
  return true;
}

// Pre-format everything the render thread will draw, once, here.
void adopt_metadata(app_state* app, std::shared_ptr<const mv::meta::metadata> record) {
  app->meta_record = record;
  mv::shell::meta_overlay o;
  const auto copy = [](char* dst, std::size_t cap, const std::string& src) {
    const std::size_t n = std::min(src.size(), cap - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
  };
  copy(o.camera_line, sizeof(o.camera_line), mv::meta::overlay_camera_line(*record));
  copy(o.exposure_line, sizeof(o.exposure_line), mv::meta::overlay_exposure_line(*record));
  copy(o.date_line, sizeof(o.date_line), mv::meta::overlay_date_line(*record));
  const auto af = mv::meta::displayed_af_points(*record);
  for (std::size_t i = 0; i < af.size() && i < mv::shell::meta_overlay::kMaxAf; ++i) {
    o.af[i][0] = af[i].x;
    o.af[i][1] = af[i].y;
    o.af[i][2] = af[i].w;
    o.af[i][3] = af[i].h;
    o.af[i][4] = af[i].in_focus ? 1.0f : 0.0f;
    o.af_count = static_cast<std::uint8_t>(i + 1);
  }
  app->input.meta = o;
  ++app->input.meta_seq;
  ++app->input.activity_seq;
  publish(app);
  push_meta_pane(app);
}

// A cache hit is adopted at once; a miss submits one read on the pool and its
// completion posts kMsgMetaReady back to this thread.
void request_metadata_now(app_state* app) {
  if (!app->window) return;
  ::KillTimer(app->window, kMetaTimerId);
  if (app->meta_record || !metadata_wanted(app)) return;
  mv::io::dir_entry entry;
  if (!current_dir_entry(app, entry)) return;
  const HWND hwnd = app->window;
  auto record = app->meta.get(entry, app->jobs, [hwnd](std::string) {
    ::PostMessageW(hwnd, kMsgMetaReady, 0, 0);
  });
  if (record) adopt_metadata(app, std::move(record));
}

void metadata_ready(app_state* app) {
  if (app->meta_record || !metadata_wanted(app)) return;
  mv::io::dir_entry entry;
  if (!current_dir_entry(app, entry)) return;
  if (auto record = app->meta.peek(entry)) adopt_metadata(app, std::move(record));
}

// Selection moved: the old record is no longer the item on screen. The new one
// is asked for only if something is showing metadata, and only after a pause, so
// holding an arrow key queues no read for the images that flash past.
void metadata_selection_changed(app_state* app) {
  if (!app->window) return;
  ::KillTimer(app->window, kMetaTimerId);
  if (app->meta_record) {
    app->meta_record.reset();
    app->input.meta = mv::shell::meta_overlay{};
    ++app->input.meta_seq;
  }
  if (metadata_wanted(app)) ::SetTimer(app->window, kMetaTimerId, kMetaDebounceMs, nullptr);
  push_meta_pane(app);
}

void push_meta_edit(app_state* app, bool drop_draft = false) noexcept;
void set_adjust_pane(app_state* app, bool on);

// The pane shows the record already held: three text tables, formatted here once
// per record. No record yet means "reading" while something is wanted, and the
// pane renders its empty states. Never reads the file. PR 12: the rating,
// comment and Revert state ride along (push_meta_edit). PR 29: the tags carry
// their raw form and what an edit may do (editable_properties_table).
void push_meta_pane(app_state* app) noexcept {
  if (!app || !app->chrome.meta_pane_visible()) return;
  push_meta_edit(app);
  if (app->meta_record) {
    app->chrome.set_meta_data(false, mv::meta::summary_table(*app->meta_record),
                              mv::meta::editable_properties_table(*app->meta_record),
                              mv::meta::streams_table(*app->meta_record));
    return;
  }
  std::uint32_t count = 0;
  const bool have = app->session && mv_folder_count(app->session, &count) == MV_OK && count > 0;
  app->chrome.set_meta_data(have, {}, {}, {});
}

void push_tree_root(app_state* app) noexcept {
  if (!app) return;
  app->chrome.set_tree_root(app->current_dir);
}

void set_meta_pane(app_state* app, bool on) noexcept {
  if (!app || app->meta_pane_visible == on) return;
  if (on) app->jobs_pane_visible = false;  // one right-edge pane at a time
  app->meta_pane_visible = on;
  app->focus_meta_next = on;  // `I` focuses the pane (docs/design/16); Esc returns to the canvas
  apply_view_state(app);
  if (on) {
    request_metadata_now(app);
    push_meta_pane(app);
  } else if (app->window) {
    focus_canvas(app);
  }
}

void set_folder_tree(app_state* app, bool on) noexcept {
  if (!app || app->tree_visible == on) return;
  app->tree_visible = on;
  app->focus_tree_next = on;  // Ctrl+Shift+E shows and focuses (docs/design/16)
  push_tree_root(app);
  apply_view_state(app);
  if (!on && app->window) focus_canvas(app);
}

// ---- PR 12: rating, comment, revert ------------------------------------------
//
// Keys 0-5 and the pane's controls queue a write; nothing here touches a file on
// this thread (rule 1). The queue coalesces (3 then 4 writes 4), runs one job at
// a time on the pool, and never overlaps a lossless rotation of the same JPEG.
// The Mac host's twin is main_mac.mm "PR 12".

// What a finished write posts back (kMsgMetaWriteDone's LPARAM, owned by the
// handler). The stamps are taken on the worker around the write, so the UI
// thread never stats the file to re-key the item's edits.
struct meta_write_result {
  mv::shell::meta_job job;
  mv::shell::meta_outcome out;
  bool stamped = false;
  std::uint64_t old_size = 0;
  std::int64_t old_mtime = 0;
  std::uint64_t new_size = 0;
  std::int64_t new_mtime = 0;
};

// One line in the title's status line for a few seconds (update_title reads it).
void notice_show(app_state* app, const std::string& utf8) noexcept {
  if (!app) return;
  try {
    app->notice = wide_from_utf8(utf8);
  } catch (...) {
    return;
  }
  app->notice_until = ::GetTickCount64() + kNoticeMs;
  update_title(app);
}

// The rating / comment as the pane should draw them: a change still waiting in
// the queue counts, so a key or a click shows at once.
std::int32_t meta_rating_now(app_state* app, const std::string& path) {
  if (const auto pending = app->meta_writer.pending_rating(path)) return *pending;
  return app->meta_record ? app->meta_record->s.rating : 0;
}

std::string meta_comment_now(app_state* app, const std::string& path) {
  if (auto pending = app->meta_writer.pending_comment(path)) return std::move(*pending);
  return app->meta_record ? app->meta_record->s.comment : std::string{};
}

void push_meta_edit(app_state* app, bool drop_draft) noexcept {
  if (!app || !app->chrome.meta_pane_visible()) return;
  try {
    const std::string path = current_item_path(app);
    std::int32_t flags = 0;
    if (!path.empty()) flags |= mv::shell::kMetaEditCanEdit;
    if (!path.empty() && app->meta_written.count(path) != 0) flags |= mv::shell::kMetaEditCanRevert;
    if (app->focus_comment_next) flags |= mv::shell::kMetaEditFocus;
    if (drop_draft) flags |= mv::shell::kMetaEditDropDraft;
    app->focus_comment_next = false;
    const std::int32_t rating = path.empty() ? 0 : meta_rating_now(app, path);
    const std::string comment = path.empty() ? std::string{} : meta_comment_now(app, path);
    app->chrome.set_meta_edit(rating, comment, flags);
  } catch (...) {
  }
}

void schedule_meta_write(app_state* app, UINT delay_ms) noexcept {
  if (app->window) ::SetTimer(app->window, kMetaWriteTimerId, std::max<UINT>(delay_ms, 1), nullptr);
}

bool rate_current_item(app_state* app, int stars) noexcept {
  if (stars < 0 || stars > mv::meta::kMaxRating) return false;
  try {
    const std::string path = current_item_path(app);
    if (path.empty()) return false;
    app->meta_writer.submit(path, mv::shell::rating_fields(stars));
    // The keystroke shows at once; the file catches up a moment later.
    notice_show(app, stars == 0 ? std::string("Rating cleared") : mv::meta::format_rating(stars));
    push_meta_edit(app);
    schedule_meta_write(app, kRatingDebounceMs);
    return true;
  } catch (...) {
    return false;
  }
}

// Ctrl+I: the pane comes up (it replaces the adjust pane, as `I` does) and the
// keyboard goes to its comment field rather than the pane's first control.
bool focus_comment_field(app_state* app) noexcept {
  try {
    if (current_item_path(app).empty()) return false;
    if (app->adjust.visible()) set_adjust_pane(app, false);
    app->focus_comment_next = true;
    if (!app->meta_pane_visible) {
      app->meta_pane_visible = true;
      apply_view_state(app);
      request_metadata_now(app);
    }
    push_meta_pane(app);
    return true;
  } catch (...) {
    return false;
  }
}

// The island committed the comment field (Return, or the field lost focus with
// an edit in it). The text is parked island-side; a read that fails is not an
// empty comment, which would clear the file's.
void set_comment_from_pane(app_state* app) noexcept {
  try {
    std::string text;
    const bool took = app->chrome.take_parked_text(text);
    const std::string path = current_item_path(app);
    if (path.empty()) return;
    if (!took || text.size() > mv::meta::kMaxCommentBytes) {
      ::MessageBeep(MB_ICONWARNING);
      notice_show(app, "Comment is too long");
      push_meta_edit(app, true);
      return;
    }
    if (text == meta_comment_now(app, path)) return;  // unchanged: no write
    app->meta_writer.submit(path, mv::shell::comment_fields(text));
    push_meta_edit(app);
    schedule_meta_write(app, kCommentDebounceMs);
  } catch (...) {
  }
}

void revert_current_metadata(app_state* app) noexcept {
  try {
    const std::string path = current_item_path(app);
    if (path.empty() || app->meta_written.count(path) == 0) return;
    app->meta_writer.submit_revert(path);
    notice_show(app, "Reverting metadata\xE2\x80\xA6");
    schedule_meta_write(app, kCommentDebounceMs);
  } catch (...) {
  }
}

// PR 29 (owner, 2026-09-26): any tag, from the pane's tag tree, its Add tag
// form and Summary's Remove location. The island parks one edit per line
// ("S\tkey\tvalue" sets, "R\tkey" removes); they go as one queued write, so
// removing a location is one checked rewrite, not one per GPS tag. The Mac
// host's twin is main_mac.mm metaSetTag.
void set_tags_from_pane(app_state* app) noexcept {
  try {
    std::string parked;
    const bool took = app->chrome.take_parked_text(parked);
    const std::string path = current_item_path(app);
    if (!took || path.empty()) return;
    const mv::meta::write_target target = app->meta_record && app->meta_record->writes_in_file
                                              ? mv::meta::write_target::in_file
                                              : mv::meta::write_target::sidecar;
    mv::meta::write_fields f;
    std::string refused;
    std::size_t at = 0;
    while (at < parked.size()) {
      std::size_t end = parked.find('\n', at);
      if (end == std::string::npos) end = parked.size();
      const std::string line = parked.substr(at, end - at);
      at = end + 1;
      if (line.size() < 3 || line[1] != '\t' || (line[0] != 'S' && line[0] != 'R')) continue;
      const bool remove = line[0] == 'R';
      const std::size_t tab = remove ? std::string::npos : line.find('\t', 2);
      const std::string key = line.substr(2, tab == std::string::npos ? std::string::npos : tab - 2);
      if (key.empty() || (!remove && tab == std::string::npos)) continue;
      const mv::meta::tag_access a = mv::meta::access_of(key, target);
      if (a == mv::meta::tag_access::read_only || (a == mv::meta::tag_access::via_sidecar && remove)) {
        refused = a == mv::meta::tag_access::read_only
                      ? "That tag describes the file itself and cannot be changed"
                      : "That tag is in the original, which is never rewritten";
        continue;
      }
      if (f.tags.size() >= mv::meta::kMaxTagEdits) break;
      f.tags.push_back({key, remove ? mv::meta::change<std::string>::remove()
                                    : mv::meta::change<std::string>::to(line.substr(tab + 1))});
    }
    if (!refused.empty()) {
      ::MessageBeep(MB_ICONWARNING);
      notice_show(app, refused);
    }
    if (f.tags.empty()) return;
    app->meta_writer.submit(path, f);
    schedule_meta_write(app, kCommentDebounceMs);
  } catch (...) {
  }
}

// PR 29: Summary's Date taken. Every capture-time tag the file carries moves
// together (meta::write_fields::date_taken), so no reader sees two dates.
void set_date_from_pane(app_state* app, bool remove) noexcept {
  try {
    std::string value;
    const bool took = remove || app->chrome.take_parked_text(value);
    const std::string path = current_item_path(app);
    if (!took || path.empty()) return;
    mv::meta::write_fields f;
    if (remove) {
      f.date_taken = mv::meta::change<std::string>::remove();
    } else {
      std::string exif_form, xmp_form;
      if (!mv::meta::exif_date_of(value, exif_form, xmp_form)) {
        ::MessageBeep(MB_ICONWARNING);
        notice_show(app, "Not a date: use YYYY-MM-DD HH:MM:SS");
        push_meta_edit(app, true);
        return;
      }
      f.date_taken = mv::meta::change<std::string>::to(value);
    }
    app->meta_writer.submit(path, f);
    schedule_meta_write(app, kCommentDebounceMs);
  } catch (...) {
  }
}

void start_meta_write(app_state* app) {
  if (app->window) ::KillTimer(app->window, kMetaWriteTimerId);
  if (app->meta_writer.in_flight()) return;  // its completion starts the next
  // A lossless rotation of a JPEG owns the file until it lands.
  if (app->edits.write_in_flight()) {
    schedule_meta_write(app, kMetaRetryMs);
    return;
  }
  const std::optional<mv::shell::meta_job> next = app->meta_writer.take_next();
  if (!next) return;
  const HWND hwnd = app->window;
  app->jobs.submit_at(mv::background_generation,
                      [job = *next, hwnd](const mv::job_context&) -> mv::status {
                        // No way to report back: no write (a write nobody hears of
                        // would leave the pane and the toast wrong).
                        auto r = std::unique_ptr<meta_write_result>(new (std::nothrow) meta_write_result{});
                        if (!r) return mv::status::out_of_memory;
                        // The identity the item's edits are filed under; a rewrite changes it.
                        const auto before = mv::io::stat_path(job.path);
                        const mv::shell::meta_outcome out = mv::shell::run_meta_job(job);
                        r->job = job;
                        r->out = out;
                        if (const auto after = mv::io::stat_path(job.path); before && after) {
                          r->stamped = true;
                          r->old_size = before->size;
                          r->old_mtime = before->mtime_unix;
                          r->new_size = after->size;
                          r->new_mtime = after->mtime_unix;
                        }
                        const bool ok = out.ok;
                        const mv::status error = out.error;
                        if (::PostMessageW(hwnd, kMsgMetaWriteDone, 0, reinterpret_cast<LPARAM>(r.get()))) {
                          (void)r.release();
                        }
                        return ok ? mv::status::ok : error;
                      });
}

void on_meta_write_done(app_state* app, std::unique_ptr<meta_write_result> r) {
  if (!r) return;
  const mv::shell::meta_job& job = r->job;
  const mv::shell::meta_outcome& out = r->out;
  app->meta_writer.finished(out);
  if (!out.ok) {
    (void)app->meta_writer.take_failure();
    ::MessageBeep(MB_ICONWARNING);
    MV_LOG_WARN("metadata write failed: %s", mv::status_name(out.error));  // never the path (rule 6)
    notice_show(app, job.revert                     ? "Could not revert the metadata"
                     : job.fields.rating.touches()  ? "Could not save the rating"
                     : job.fields.comment.touches() ? "Could not save the comment"
                     : out.error == mv::status::invalid_arg
                         ? "That value does not fit the tag; nothing was changed"
                         : "Could not save the metadata; nothing was changed");
    push_meta_edit(app);
    if (app->meta_writer.has_pending()) schedule_meta_write(app, 0);
    return;
  }

  // What the store cached for this file is out of date: a JPEG's stamp moved,
  // a sidecar's did not, so drop it by path either way.
  app->meta.invalidate(out.path);
  if (r->stamped) {
    // The bytes changed, the pixels did not: keep the item's edits, and let
    // the next listing see the same item rather than a new one.
    app->edits.metadata_rewritten(out.path, r->old_size, r->old_mtime, r->new_size, r->new_mtime);
    if (app->edit_path == out.path && app->edit_size == r->old_size && app->edit_mtime == r->old_mtime) {
      app->edit_size = r->new_size;
      app->edit_mtime = r->new_mtime;
    }
  }
  if (job.revert) app->meta_written.erase(out.path);
  else app->meta_written.insert(out.path);

  if (current_item_path(app) == out.path) {
    app->meta_record.reset();
    if (metadata_wanted(app)) request_metadata_now(app);
    push_meta_pane(app);
  }

  // Say where it went, unless another change to the same file is already queued
  // (its message is the one that matters).
  if (!app->meta_writer.busy_for(out.path)) {
    std::string text = job.revert ? std::string("Metadata reverted") : std::string();
    if (!job.revert && job.fields.rating.touches()) {
      const int stars =
          job.fields.rating.k == mv::meta::change<int>::kind::clear ? 0 : job.fields.rating.value;
      text = stars == 0 ? std::string("Rating cleared") : mv::meta::format_rating(stars);
    } else if (!job.revert && job.fields.comment.touches()) {
      text = job.fields.comment.k == mv::meta::change<std::string>::kind::clear ? "Comment removed"
                                                                                : "Comment saved";
    } else if (!job.revert && job.fields.date_taken.touches()) {
      text = job.fields.date_taken.k == mv::meta::change<std::string>::kind::clear ? "Date taken removed"
                                                                                   : "Date taken saved";
    } else if (!job.revert && !job.fields.tags.empty()) {
      text = job.fields.tags.size() == 1 ? std::string("Metadata saved")
                                         : std::to_string(job.fields.tags.size()) + " tags saved";
    }
    if (out.target == mv::meta::write_target::sidecar && out.sidecar_touched) {
      const std::size_t sep = out.sidecar_path.find_last_of("\\/");
      text += "  \xE2\x80\x94 " + out.sidecar_path.substr(sep == std::string::npos ? 0 : sep + 1);  // — IMG_1234.xmp
    }
    if (!text.empty()) notice_show(app, text);
  }
  if (app->meta_writer.has_pending()) schedule_meta_write(app, 0);
}

// PR 9 sort. The session owns the order (the filmstrip, the gallery and the arrow
// keys all read one list); this persists it and tells the chrome what took effect.
void set_sort(app_state* app, std::int32_t packed) noexcept {
  if (!app || !app->session) return;
  packed = mv::io::pack_sort(mv::io::unpack_sort(packed));
  if (mv_folder_set_sort(app->session, packed) != MV_OK) return;
  app->settings.sort = packed;
  mv::shell::save_view_settings(app->settings);
  app->chrome.apply_settings(chrome_flags(app), packed);
}

// Ctrl+C (docs/design/16): the eyedropper's readout when it is on and a pixel is under
// the cursor; otherwise the marked files, else the current item (the selected
// cell while the gallery is up) as CF_HDROP, pasteable in Explorer, Mail, chat.
// A pair copies both halves, as F7 does. Never asks the user anything.
struct clip_format {
  UINT format = 0;
  HGLOBAL mem = nullptr;
};

// Every format goes on in one open, so a paste target sees them together
// (Ctrl+Alt+C offers a file and the PNG). The clipboard owns each block only
// once SetClipboardData took it; the rest are freed here.
bool set_clipboard(app_state* app, std::initializer_list<clip_format> formats) {
  // A block that could not be built leaves the clipboard as it was.
  const bool complete = std::all_of(formats.begin(), formats.end(),
                                    [](const clip_format& f) { return f.mem != nullptr; });
  const bool opened = complete && app && app->window && ::OpenClipboard(app->window);
  if (opened) ::EmptyClipboard();
  bool ok = opened;
  for (const clip_format& f : formats) {
    if (!f.mem) continue;
    if (!opened || ::SetClipboardData(f.format, f.mem) == nullptr) {
      ::GlobalFree(f.mem);
      ok = false;
    }
  }
  if (opened) ::CloseClipboard();
  return ok;
}

bool set_clipboard(app_state* app, UINT format, HGLOBAL mem) {
  return set_clipboard(app, {clip_format{format, mem}});
}

// CF_UNICODETEXT of a UTF-8 string. Null for an empty one.
HGLOBAL text_to_global(const std::string& utf8) {
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
  if (n <= 1) return nullptr;
  HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(n) * sizeof(wchar_t));
  if (!mem) return nullptr;
  auto* dst = static_cast<wchar_t*>(::GlobalLock(mem));
  if (!dst) {
    ::GlobalFree(mem);
    return nullptr;
  }
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, dst, n);
  ::GlobalUnlock(mem);
  return mem;
}

// CF_HDROP: DROPFILES, then each path as UTF-16 with a NUL, then one more NUL.
HGLOBAL hdrop_to_global(const std::vector<std::string>& paths) {
  std::wstring list;
  for (const std::string& utf8 : paths) {
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (n <= 1) continue;
    std::wstring wide(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), n);
    list.append(wide.c_str(), static_cast<std::size_t>(n));  // includes its NUL
  }
  if (list.empty()) return nullptr;
  list.push_back(L'\0');
  const SIZE_T bytes = sizeof(DROPFILES) + list.size() * sizeof(wchar_t);
  HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
  if (!mem) return nullptr;
  auto* drop = static_cast<DROPFILES*>(::GlobalLock(mem));
  if (!drop) {
    ::GlobalFree(mem);
    return nullptr;
  }
  drop->pFiles = sizeof(DROPFILES);
  drop->fWide = TRUE;
  std::memcpy(reinterpret_cast<char*>(drop) + sizeof(DROPFILES), list.data(),
              list.size() * sizeof(wchar_t));
  ::GlobalUnlock(mem);
  return mem;
}

// Raw bytes for a registered format ("PNG": Office, browsers, Paint, chat apps).
HGLOBAL bytes_to_global(const std::vector<std::uint8_t>& bytes) {
  if (bytes.empty()) return nullptr;
  HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes.size());
  if (!mem) return nullptr;
  void* dst = ::GlobalLock(mem);
  if (!dst) {
    ::GlobalFree(mem);
    return nullptr;
  }
  std::memcpy(dst, bytes.data(), bytes.size());
  ::GlobalUnlock(mem);
  return mem;
}

bool copy_to_clipboard(app_state* app) {
  if (!app || !app->window) return false;
  if (app->input.eyedropper) {
    const std::string text = app->lab.eyedropper_text();
    if (!text.empty()) return set_clipboard(app, CF_UNICODETEXT, text_to_global(text));
  }
  const auto targets = expand_pair_targets(app, app->marks.targets(current_item_path(app)));
  if (targets.empty()) return false;
  return set_clipboard(app, CF_HDROP, hdrop_to_global(targets));
}

// ---- PR 10: edit stack, lossless rotate, crop, export ------------------------

// What an edit job posts back (kMsgEditJobDone's LPARAM, owned by the handler).
struct edit_job_result {
  bool export_job = false;  // else a lossless rotate write
  bool ok = false;
  std::string path;         // the file written (rotate) or the source (export)
};

struct sibling_job_result {
  std::uint64_t generation = 0;
  std::string here;
  std::vector<std::string> paths;
  int index = -1;
};

void publish_edit(app_state* app) noexcept {
  if (app->show_original && app->edits.has_item() && app->edit_key != 0) {
    // PR 29 Show original (Y held, the strip's toggle): the item with no
    // geometry and no colour, on the same blit. The stack is untouched.
    mv::shell::edit_view v;
    v.item = app->edit_key;
    v.generation = app->edit_generation;
    app->input.edit[0] = v;
  } else {
    app->input.edit[0] = mv::shell::view_of(app->edits, app->edit_key, app->edit_generation);
  }
  push_edit_view(app);  // the strip's edit count, the Crop pane's draft
}

void schedule_rotation_write(app_state* app) noexcept {
  if (app->window) ::SetTimer(app->window, kRotateTimerId, kRotateDebounceMs, nullptr);
}

void adjust_item_changed(app_state* app);
void adjust_colour_changed(app_state* app);

// The canvas is about to show the selected item (a select, a listing landing,
// a reselect after a rewrite). The previous slot keeps its geometry for the
// texture still on screen until the new pixels land (shell/edit_view.h).
void edit_item_opened(app_state* app) {
  trim_item_opened(app);
  mv::io::dir_entry entry;
  if (!current_dir_entry(app, entry)) {
    if (app->edits.has_item()) {
      app->edits.clear_item();
      app->input.edit[1] = app->input.edit[0];
      app->edit_path.clear();
      app->edit_key = 0;
      app->show_original = false;
      publish_edit(app);
      adjust_item_changed(app);
      workspace_item_changed(app);
    }
    return;
  }
  std::uint32_t generation = 0;
  (void)mv_session_current_generation(app->session, &generation);
  if (entry.path_utf8 == app->edit_path && entry.size == app->edit_size &&
      entry.mtime_unix == app->edit_mtime && generation == app->edit_generation) {
    return;  // the same bytes, the same select: nothing moved
  }
  app->input.edit[1] = app->input.edit[0];
  app->edit_path = entry.path_utf8;
  app->edit_size = entry.size;
  app->edit_mtime = entry.mtime_unix;
  app->edit_key = mv::canvas::item_key_for(entry.path_utf8.data(), entry.path_utf8.size());
  app->edit_generation = generation;
  mv::shell::edit_item e;
  e.path = entry.path_utf8;
  e.size = entry.size;
  e.mtime = entry.mtime_unix;
  // A hint: the write job probes the magic bytes and refuses anything else.
  std::string ext;
  if (const auto dot = entry.name_utf8.find_last_of('.'); dot != std::string::npos) {
    ext = entry.name_utf8.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  }
  e.jpeg = ext == "jpg" || ext == "jpeg" || ext == "jpe";
  std::uint32_t w = 0, h = 0;
  if (app->lab.still_size(app->edit_key, &w, &h)) {
    e.width = w;
    e.height = h;
  }
  const bool carried_turn = app->edits.set_item(e);
  app->show_original = false;
  publish_edit(app);
  if (carried_turn) schedule_rotation_write(app);
  adjust_item_changed(app);
  // PR 29: an open workspace follows the item to a tab it offers.
  workspace_item_changed(app);
}

void start_rotation_write(app_state* app) {
  if (app->window) ::KillTimer(app->window, kRotateTimerId);
  // PR 12: a metadata write is rewriting a JPEG; wait for it (it re-keys the
  // item's edits when it lands, so the rotation then checks the new bytes).
  if (app->meta_writer.in_flight()) {
    schedule_rotation_write(app);
    return;
  }
  const std::optional<mv::shell::rotation_write> w = app->edits.take_pending_write();
  if (!w) return;
  const HWND hwnd = app->window;
  app->jobs.submit_at(mv::background_generation,
                      [job = *w, hwnd](const mv::job_context&) -> mv::status {
                        const mv::expected written = mv::shell::run_rotation_write(job);
                        auto* r = new (std::nothrow) edit_job_result{false, static_cast<bool>(written), job.path};
                        if (r && !::PostMessageW(hwnd, kMsgEditJobDone, 0, reinterpret_cast<LPARAM>(r))) delete r;
                        return written ? mv::status::ok : written.error();
                      });
}

// Ctrl+S: the stack baked into "<name>-edit.jpg" beside the original (never
// over it). `opt` comes from the export dialog, or the defaults without chrome.
void start_export(app_state* app, const mv::edit::export_options& opt) {
  if (app->edit_path.empty()) return;
  const HWND hwnd = app->window;
  app->jobs.submit_at(mv::background_generation,
                      [path = app->edit_path, g = app->edits.export_geometry(), opt,
                       c = app->edits.colour(), hwnd](const mv::job_context&) -> mv::status {
                        const mv::result<std::string> out = mv::shell::run_export(path, g, opt, c);
                        auto* r = new (std::nothrow) edit_job_result{true, static_cast<bool>(out), path};
                        if (r && !::PostMessageW(hwnd, kMsgEditJobDone, 0, reinterpret_cast<LPARAM>(r))) delete r;
                        return out ? mv::status::ok : out.error();
                      });
}

void folder_select(app_state* app, std::uint32_t index);
bool video_mode(app_state* app) noexcept;

void on_edit_job_done(app_state* app, std::unique_ptr<edit_job_result> r) {
  if (!r) return;
  if (r->export_job) {
    // The new file shows up through the folder watcher; only a failure speaks.
    if (!r->ok) ::MessageBeep(MB_ICONWARNING);
    return;
  }
  app->edits.write_finished(r->ok);
  if (!r->ok) {
    ::MessageBeep(MB_ICONWARNING);
    publish_edit(app);
    ++app->input.activity_seq;
    publish(app);
    return;
  }
  // The navigation LRU still holds the old pixels under this path: drop them,
  // then reselect so the rewritten file decodes (ABI 0.7).
  (void)mv_folder_forget(app->session, r->path.c_str());
  std::uint32_t selected = 0;
  if (selected_index(app, selected) && current_item_path(app) == r->path) {
    folder_select(app, selected);
  }
}

// What an edit_session call asked the host to do. Shared by the keys and (PR
// 29) the Crop pane's preset buttons and straighten slider.
void apply_edit_effect(app_state* app, mv::shell::edit_effect effect) {
  switch (effect) {
    case mv::shell::edit_effect::none:
      return;
    case mv::shell::edit_effect::refused:
      ::MessageBeep(MB_ICONWARNING);
      return;
    case mv::shell::edit_effect::redraw:
      publish_edit(app);
      ++app->input.activity_seq;
      publish(app);
      adjust_colour_changed(app);  // undo / reset may have moved a slider
      return;
    case mv::shell::edit_effect::write_rotation:
      publish_edit(app);
      ++app->input.activity_seq;
      publish(app);
      schedule_rotation_write(app);
      adjust_colour_changed(app);
      return;
    case mv::shell::edit_effect::export_image:
      if (app->chrome.attached()) {
        app->chrome.show_export_dialog(app->export_choice);
      } else {
        start_export(app, mv::shell::unpack_export(app->export_choice));
      }
      return;
  }
}

bool run_edit_command(app_state* app, mv::shell::command_id command) {
  // Stills only: a clip keeps `[` `]` for trim (PR 13), an animation has no
  // single frame to turn.
  if (app->mode == open_mode::none || app->edit_path.empty() || video_mode(app) ||
      app->lab.animation() != mv::shell::animation_state::none) {
    return false;
  }
  std::uint32_t w = 0, h = 0;
  if (app->lab.still_size(app->edit_key, &w, &h)) {
    app->edits.set_size(w, h);
  } else if (command == mv::shell::command_id::crop_mode) {
    ::MessageBeep(MB_ICONWARNING);  // no pixels yet: nothing to frame a crop against
    return true;
  }
  apply_edit_effect(app, app->edits.run(command));
  return true;
}

// ---- PR 11: colour adjusts, the adjust pane, the FP16 working image ---------

// What an adjust job posts back (kMsgAdjustJobDone's LPARAM, owned by the handler).
struct adjust_job_result {
  bool histogram = false;  // else a working-image build
  std::uint64_t token = 0;
  bool ok = false;
  bool from_raw = false;
  std::shared_ptr<const mv::image::linear_image> working;
  mv::edit::histogram hist;
};

void post_adjust_result(HWND hwnd, adjust_job_result* r) noexcept {
  if (r && !::PostMessageW(hwnd, kMsgAdjustJobDone, 0, reinterpret_cast<LPARAM>(r))) delete r;
}

// The pane is for stills: a clip or an animation has no one frame to adjust.
bool adjust_still(app_state* app) noexcept {
  return app->mode != open_mode::none && !app->edit_path.empty() &&
         !mv::shell::is_video_name(app->edit_path) && !video_mode(app) &&
         app->lab.animation() == mv::shell::animation_state::none;
}

std::uint64_t adjust_item_id(app_state* app) noexcept {
  if (app->mode == open_mode::none || app->edit_path.empty() ||
      mv::shell::is_video_name(app->edit_path) || app->edit_key == 0) {
    return 0;
  }
  // A rewritten file (a lossless turn landing) is a new generation: new pixels.
  return app->edit_key ^ (static_cast<std::uint64_t>(app->edit_generation) * 0x9E3779B97F4A7C15ull);
}

void push_adjust_pane(app_state* app) noexcept {
  if (!app->chrome.adjust_pane_visible()) return;
  app->chrome.set_adjust_view(app->adjust.view(app->edits.colour()));
}

// Read, develop (for a RAW: LibRaw's linear 16-bit develop — docs/design/07 waits for
// this, never the embedded preview), downscale to the preview edge, upload as
// an immutable FP16 texture — all on the pool. Seconds for a 45 MP RAW; the
// pane says "Preparing" meanwhile and the UI thread never waits (rule 1).
void start_working_build(app_state* app, std::uint64_t token) {
  if (!app->window) return;
  app->working.reset();
  app->lab.drop_working();
  const mv::generation gen = app->adjust_generation.load(std::memory_order_relaxed);
  const HWND hwnd = app->window;
  app->jobs.submit_at(
      mv::background_generation,
      [path = app->edit_path, item = app->edit_key, view_gen = app->edit_generation, token, gen,
       hwnd, app](const mv::job_context&) -> mv::status {
        const mv::job_context ctx(0, gen, &app->adjust_generation, 0);
        auto* r = new (std::nothrow) adjust_job_result{};
        if (!r) return mv::status::out_of_memory;
        r->token = token;
        mv::status st = mv::status::ok;
        auto bytes = mv::io::read_all(path);
        if (!bytes) {
          st = bytes.error();
        } else if (auto full = mv::image::decode_linear(*bytes, &ctx); !full) {
          st = full.error();
        } else if (auto preview = mv::image::downsample(*full, mv::image::kWorkingPreviewEdge, &ctx);
                   !preview) {
          st = preview.error();
        } else if (ctx.cancelled()) {
          st = mv::status::cancelled;
        } else {
          r->from_raw = preview->from_raw;
          r->ok = app->lab.upload_working(*preview, item, view_gen);
          if (r->ok) {
            try {
              r->working = std::make_shared<const mv::image::linear_image>(std::move(preview).value());
            } catch (...) {
              r->ok = false;
            }
          }
          if (!r->ok) st = mv::status::internal;
        }
        post_adjust_result(hwnd, r);
        return st;
      });
}

void schedule_histogram(app_state* app) noexcept {
  app->adjust.histogram_dirty();
  if (app->window) ::SetTimer(app->window, kHistogramTimerId, kHistogramDebounceMs, nullptr);
}

void start_histogram(app_state* app) {
  if (app->window) ::KillTimer(app->window, kHistogramTimerId);
  if (!app->working) return;
  const std::optional<std::uint64_t> token = app->adjust.take_histogram_request();
  if (!token) return;
  const HWND hwnd = app->window;
  app->jobs.submit_at(mv::background_generation,
                      [working = app->working, u = mv::edit::uniforms_of(app->edits.colour()),
                       t = *token, hwnd](const mv::job_context&) -> mv::status {
                        auto* r = new (std::nothrow) adjust_job_result{};
                        if (!r) return mv::status::out_of_memory;
                        r->histogram = true;
                        r->token = t;
                        auto h = mv::edit::compute_histogram(*working, u);
                        r->ok = static_cast<bool>(h);
                        if (h) r->hist = *h;
                        post_adjust_result(hwnd, r);
                        return h ? mv::status::ok : h.error();
                      });
}

void on_adjust_job_done(app_state* app, std::unique_ptr<adjust_job_result> r) {
  if (!r) return;
  if (r->histogram) {
    if (r->ok && app->adjust.histogram_landed(r->token, r->hist)) push_adjust_pane(app);
    return;
  }
  if (app->adjust.working_landed(r->token, r->ok, r->from_raw)) {
    app->working = std::move(r->working);
    schedule_histogram(app);
  }
  push_adjust_pane(app);
}

void adjust_build_if(app_state* app, std::optional<std::uint64_t> token) {
  if (token) start_working_build(app, *token);
}

void adjust_item_changed(app_state* app) {
  const std::uint64_t id = adjust_item_id(app);
  const bool has_colour = app->edits.has_item() && !app->edits.colour().identity();
  const auto before = app->adjust.readiness();
  const std::optional<std::uint64_t> token = app->adjust.set_item(id, has_colour);
  if (app->adjust.readiness() == mv::shell::adjust_readiness::none &&
      before != mv::shell::adjust_readiness::none) {
    // Left the item: stop its build and release its textures.
    app->adjust_generation.fetch_add(1, std::memory_order_relaxed);
    app->working.reset();
    app->lab.drop_working();
  }
  if (token) {
    app->adjust_generation.fetch_add(1, std::memory_order_relaxed);
    start_working_build(app, *token);
  }
  push_adjust_pane(app);
}

void adjust_colour_changed(app_state* app) {
  const bool has_colour = app->edits.has_item() && !app->edits.colour().identity();
  adjust_build_if(app, app->adjust.colour_changed(has_colour));
  schedule_histogram(app);
  push_adjust_pane(app);
}

void set_adjust_pane(app_state* app, bool on) {
  if (on && !adjust_still(app)) {
    ::MessageBeep(MB_ICONWARNING);  // nothing to adjust: a clip, an animation, an empty window
    return;
  }
  if (on == app->adjust.visible()) {
    if (on) {  // already up: Shift+A takes the keyboard back to it
      app->focus_adjust_next = true;
      apply_view_state(app);
    }
    return;
  }
  // The adjust and metadata panes share the right edge: one at a time.
  if (on && app->meta_pane_visible) set_meta_pane(app, false);
  if (on) app->jobs_pane_visible = false;
  const bool has_colour = app->edits.has_item() && !app->edits.colour().identity();
  adjust_build_if(app, app->adjust.show(on, has_colour));
  if (!on && !has_colour) {
    // Nothing on the canvas needs the working image now; a reopen rebuilds it.
    app->adjust_generation.fetch_add(1, std::memory_order_relaxed);
    app->working.reset();
    app->lab.drop_working();
    app->adjust.working_dropped();
  }
  app->focus_adjust_next = on;  // docs/design/16 "Pane": opening focuses it; Esc returns to the canvas
  apply_view_state(app);
  push_adjust_pane(app);
  if (on) {
    schedule_histogram(app);
  } else if (app->window) {
    focus_canvas(app);
  }
}

// A slider moved (the pane's command carries the value). The sliders are
// disabled until the working image is ready; a late event is ignored.
void set_adjust_from_pane(app_state* app, mv::edit::adjust_param p, float value) {
  if (!app->adjust.working_ready() || !adjust_still(app)) return;
  if (app->edits.set_adjust(p, value) != mv::shell::edit_effect::redraw) return;
  publish_edit(app);
  ++app->input.activity_seq;
  publish(app);
  // No push back to the pane here: it already shows the value it sent, and
  // echoing it would fight the drag. The histogram follows when it settles.
  const bool has_colour = !app->edits.colour().identity();
  adjust_build_if(app, app->adjust.colour_changed(has_colour));
  schedule_histogram(app);
}

void reset_adjust_from_pane(app_state* app) {
  if (!adjust_still(app)) return;
  if (app->edits.reset_adjust() != mv::shell::edit_effect::redraw) return;
  publish_edit(app);
  ++app->input.activity_seq;
  publish(app);
  adjust_colour_changed(app);
}

void folder_select(app_state* app, std::uint32_t index) {
  if (!app || !app->session) return;
  // Any navigation bumps the generation, which retires a Live Photo's motion.
  if (app->motion_playing) {
    app->motion_playing = false;
    if (app->window) ::KillTimer(app->window, kMotionTimerId);
  }
  uint64_t job = 0;
  if (mv_folder_select(app->session, index, &job) == MV_OK) {
    refresh_item_info(app);
    refresh_mark_state(app);
    metadata_selection_changed(app);
    edit_item_opened(app);
    // Navigation keeps a fullscreen reveal up; it hides once this settles.
    if (app->fullscreen_reveal && app->window) {
      ::SetTimer(app->window, kRevealTimerId, kRevealMs, nullptr);
    }
    ++app->input.activity_seq;
    publish(app);
  }
}

// Ctrl+PageDown / PageUp: the next or previous page of the multi-page still
// on screen (docs/plans/audio-and-documents.md §2.3). The count is what the
// core reported for the image on screen; until it has, and on a single-page
// still or a clip, the keys do nothing.
bool turn_page(app_state* app, int delta) {
  if (!app || !app->session || video_mode(app)) return false;
  std::uint32_t index = 0;
  if (mv_folder_selected(app->session, &index) != MV_OK) return false;
  if (app->page_of != index) {
    app->page_of = index;
    app->page = 0;
    app->page_count = 0;
  }
  mv_image_info info{};
  if (mv_session_image_info(app->session, &info) == MV_OK && info.page_count > 0) {
    app->page_count = info.page_count;
  }
  if (app->page_count <= 1) return false;
  const std::int64_t want = std::clamp<std::int64_t>(static_cast<std::int64_t>(app->page) + delta, 0,
                                                     static_cast<std::int64_t>(app->page_count) - 1);
  if (want == static_cast<std::int64_t>(app->page)) return true;
  std::uint64_t job = 0;
  if (mv_folder_select_page(app->session, static_cast<std::uint32_t>(want), &job) != MV_OK) return false;
  app->page = static_cast<std::uint32_t>(want);
  notice_show(app, "Page " + std::to_string(app->page + 1) + " of " + std::to_string(app->page_count));
  ++app->input.activity_seq;
  publish(app);
  return true;
}

void folder_step(app_state* app, int delta) {
  if (!app || !app->session) return;
  uint32_t count = 0;
  uint32_t selected = 0;
  if (mv_folder_count(app->session, &count) != MV_OK || count == 0) return;
  if (mv_folder_selected(app->session, &selected) != MV_OK) return;
  // docs/design/16: wrap at the ends when the setting is on (the default).
  const auto next = mv::shell::step_index(selected, delta, count, app->settings.wrap);
  if (!next) return;
  folder_select(app, *next);
}

// Skim, not transport: J/L are the +/-10 s jumps, Q/E are the shuttle you hold
// down to find a moment. 2 s per repeat lands about where a scrubber drag does.
constexpr std::int64_t kSkimStepNs = 2'000'000'000;
// docs/design/16: J / L are the +/-10 s transport jumps.
constexpr std::int64_t kTransportStepNs = 10'000'000'000;

// docs/design/16's Video mode: "current item is a clip, playing or paused". Stopped
// means no clip, so Q/E do nothing and the key goes to the island.
// How long a skim burst stays "the same burst". Longer than key-repeat's
// ~30 ms cadence, short enough that a second press a beat later starts from
// where the clip actually is.
constexpr std::uint64_t kSkimBurstMs = 700;

// The speed ladder, shared with the command bar's dropdown. Every value is
// exactly representable in float, so the rate native applies and the rate the
// dropdown shows can be compared without an epsilon.
constexpr double kRateLadder[] = {0.25, 0.5, 1.0, 1.5, 2.0, 4.0};
constexpr int kRateLadderCount = static_cast<int>(std::size(kRateLadder));
constexpr int kRateDefaultIndex = 2;  // 1.00x

bool video_mode(app_state* app) noexcept {
  if (!app || !app->session) return false;
  std::uint32_t state = MV_PLAY_STOPPED;
  if (mv_video_state(app->session, &state) != MV_OK) return false;
  return state != MV_PLAY_STOPPED;
}

// ---- PR 13 / 14: trim mode, the clip tools and the Jobs pane (docs/design/08) --------
// Trim is host state over the clip on screen (shell/trim_state.h, shared with
// the Mac host); the keyframe index and every job run in the core behind
// mediaviewer_clip.h. Nothing here reads the file or waits on a job (rule 1).

// The scrub bar's markers, grid and label.
void push_trim(app_state* app) noexcept {
  if (!app) return;
  const mv::shell::trim_state& t = app->trim;
  const std::string label = t.armed() ? t.label() : std::string();
  const mv::edit::clip::range cut = t.keyframe_range();
  mv::shell::chrome_trim_args a{};
  a.armed = t.armed() ? 1 : 0;
  a.index_ready = t.index_ready() ? 1 : 0;
  a.duration_ns = t.duration_ns();
  a.in_ns = t.in_ns();
  a.out_ns = t.out_ns();
  a.cut_in_ns = t.has_marker() ? cut.in_ns : -1;
  a.cut_out_ns = t.has_marker() ? cut.out_ns : -1;
  a.keyframes = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(t.keyframes().data()));
  a.keyframe_count = static_cast<std::int32_t>(t.keyframes().size());
  a.label_utf8 = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(label.data()));
  a.label_len = static_cast<std::int32_t>(label.size());
  a.previewing = t.previewing() ? 1 : 0;
  app->chrome.set_trim(a);
  push_edit_view(app);  // PR 29: the Trim pane shows the same state
}

std::int64_t clip_position(app_state* app) noexcept {
  std::int64_t position = 0;
  if (app && app->session) (void)mv_video_position(app->session, &position);
  return std::max<std::int64_t>(0, position);
}

// P: the A-B loop over exactly what Path 1 will write (docs/design/08 "Preview the
// cut"); off clears the loop.
void apply_trim_preview(app_state* app) noexcept {
  if (!app || !app->session) return;
  if (app->trim.previewing()) {
    const mv::edit::clip::range r = app->trim.keyframe_range();
    (void)mv_video_set_loop(app->session, r.in_ns, r.out_ns);
  } else {
    (void)mv_video_set_loop(app->session, 0, -1);
  }
}

// The item changed: trim and its markers belong to the clip that was left.
void trim_item_opened(app_state* app) noexcept {
  if (!app || app->trim.path().empty()) return;
  if (current_item_path(app) == app->trim.path()) return;
  app->trim.forget();
  app->trim_index_request = 0;
  push_trim(app);
}

bool set_trim_mode(app_state* app, bool on) noexcept {
  if (!app || !app->session) return false;
  if (on) {
    if (!video_mode(app) || app->motion_playing) return false;
    const std::string path = current_item_path(app);
    if (path.empty()) return false;
    mv_video_info info{};
    (void)mv_video_get_info(app->session, &info);
    const bool fresh = path != app->trim.path();
    app->trim.arm(path, info.duration_ns);
    // The grid is read once per clip, on a worker; it lands through the
    // island's drain (chrome_cmd_clip_index).
    if ((fresh || !app->trim.index_ready()) && app->trim_index_request == 0) {
      std::uint64_t id = 0;
      if (mv_clip_index_request(app->session, path.c_str(), &id) == MV_OK) app->trim_index_request = id;
    }
  } else {
    const bool looping = app->trim.previewing();
    app->trim.disarm();
    if (looping) apply_trim_preview(app);
  }
  push_trim(app);
  return true;
}

void trim_index_arrived(app_state* app, std::uint64_t id) noexcept {
  if (!app || !app->session || id == 0 || id != app->trim_index_request) return;
  app->trim_index_request = 0;
  std::uint32_t count = 0;
  std::int64_t duration = 0;
  if (mv_clip_index_get(app->session, id, nullptr, 0, &count, &duration) != MV_OK) return;
  std::vector<std::int64_t> keyframes(count);
  if (count > 0 && mv_clip_index_get(app->session, id, keyframes.data(), count, &count, &duration) != MV_OK) {
    return;
  }
  keyframes.resize(std::min<std::size_t>(count, keyframes.size()));
  app->trim.set_index(app->trim.path(), std::move(keyframes), duration);
  if (app->trim.previewing()) apply_trim_preview(app);
  push_trim(app);
}

// A core request as the ABI's POD (mediaviewer_clip.h option numbers).
mv_clip_request abi_request(const mv::edit::clip::request& r) noexcept {
  namespace clip = mv::edit::clip;
  mv_clip_request q{};
  q.struct_size = sizeof(q);
  q.op = static_cast<std::uint32_t>(r.kind);
  q.in_ns = r.in_ns;
  q.out_ns = r.out_ns;
  switch (r.kind) {
    case clip::op::rotate: q.option = r.rotate_degrees == 270 ? 2u : r.rotate_degrees == 180 ? 3u : 1u; break;
    case clip::op::remux: q.option = r.remux == clip::remux_target::mkv ? 2u : 1u; break;
    case clip::op::frame: q.option = r.frame == clip::frame_format::jpeg ? 2u : 1u; break;
    case clip::op::audio:
      q.option = r.audio == clip::audio_format::wav ? 2u : r.audio == clip::audio_format::flac ? 3u : 1u;
      break;
    case clip::op::animation:
      q.option = r.animation == clip::anim_format::webp ? 2u : 1u;
      q.animation_width = r.animation_width;
      q.animation_fps = r.animation_fps;
      break;
    default: break;
  }
  return q;
}

// Queues a job and shows the Jobs pane (without taking the keyboard) so the
// progress is visible; the pane is where it is cancelled (docs/design/08: never a
// modal progress dialog).
bool submit_clip_job(app_state* app, const mv::edit::clip::request& r) noexcept {
  if (!app || !app->session || r.source.empty()) return false;
  mv_clip_request q = abi_request(r);
  // PR 30: the Video Editor's pieces, flattened for the ABI (read during the call).
  std::vector<std::int64_t> flat;
  if (r.kind == mv::edit::clip::op::keep_ranges) {
    try {
      flat.reserve(r.ranges.size() * 2);
    } catch (...) {
      ::MessageBeep(MB_ICONWARNING);
      return true;
    }
    for (const mv::edit::clip::range& g : r.ranges) {
      flat.push_back(g.in_ns);
      flat.push_back(g.out_ns);
    }
    q.ranges_ns = flat.data();
    q.range_count = static_cast<std::uint32_t>(r.ranges.size());
    q.option = r.ranges_exact ? 2u : 1u;
  }
  std::uint64_t job = 0;
  if (mv_clip_submit(app->session, r.source.c_str(), &q, &job) != MV_OK) {
    ::MessageBeep(MB_ICONWARNING);
    return true;
  }
  set_jobs_pane(app, true, false);
  return true;
}

// docs/design/13: an update restart waits for queued and running clip jobs.
bool clip_jobs_busy(app_state* app) noexcept {
  if (!app || !app->session) return false;
  std::uint32_t count = 0;
  if (mv_clip_jobs(app->session, nullptr, 0, &count) != MV_OK || count == 0) return false;
  std::vector<std::uint64_t> ids(count);
  if (mv_clip_jobs(app->session, ids.data(), count, &count) != MV_OK) return false;
  for (std::uint32_t i = 0; i < count && i < ids.size(); ++i) {
    mv_clip_progress p{};
    if (mv_clip_job_progress(app->session, ids[i], &p) == MV_OK &&
        (p.state == MV_CLIP_JOB_QUEUED || p.state == MV_CLIP_JOB_RUNNING)) {
      return true;
    }
  }
  return false;
}

void set_jobs_pane(app_state* app, bool on, bool focus) noexcept {
  if (!app) return;
  if (!app->chrome.panels_attached()) {
    if (on && focus) ::MessageBeep(MB_ICONWARNING);
    return;
  }
  if (app->jobs_pane_visible == on) {
    if (on && focus) {  // already up: Ctrl+J takes the keyboard to it
      app->focus_jobs_next = true;
      apply_view_state(app);
    }
    return;
  }
  app->jobs_pane_visible = on;
  app->focus_jobs_next = on && focus;
  apply_view_state(app);
  if (!on && app->window) focus_canvas(app);
}

// The clip tools flyout's flags (trim_state.h kClipFlag*).
std::int32_t clip_tool_flags(app_state* app) noexcept {
  std::int32_t flags = mv::shell::kClipFlagHasVideo;
  if (app->trim.has_marker() && app->trim.path() == current_item_path(app)) {
    flags |= mv::shell::kClipFlagHasRange;
  }
  mv_video_info info{};
  if (app->session && mv_video_get_info(app->session, &info) == MV_OK && (info.flags & 1u) != 0) {
    flags |= mv::shell::kClipFlagHasAudio;
  }
  return flags;
}

bool run_clip_command(app_state* app, mv::shell::command_id command) noexcept {
  using enum mv::shell::command_id;
  namespace clip = mv::edit::clip;
  switch (command) {
    case trim_mode:
      return set_trim_mode(app, !(app->trim.armed() && video_mode(app)));
    case trim_in:
    case trim_out: {
      if (!app->trim.armed()) return false;
      const std::int64_t at = clip_position(app);
      if (command == trim_in) app->trim.mark_in(at);
      else app->trim.mark_out(at);
      if (app->trim.previewing()) apply_trim_preview(app);
      push_trim(app);
      return true;
    }
    case trim_clear: {
      if (!app->trim.armed()) return false;
      const bool looping = app->trim.previewing();
      app->trim.clear();
      if (looping) apply_trim_preview(app);
      push_trim(app);
      return true;
    }
    case trim_preview: {
      if (!app->trim.armed()) return false;
      if (!app->trim.has_marker() && !app->trim.previewing()) {
        ::MessageBeep(MB_ICONWARNING);  // nothing to preview: set `[` or `]` first
        return true;
      }
      if (app->trim.toggle_preview()) {
        apply_trim_preview(app);
        (void)mv_video_seek(app->session, app->trim.keyframe_range().in_ns, 1);
        (void)mv_video_play(app->session);
      } else {
        apply_trim_preview(app);
      }
      push_trim(app);
      return true;
    }
    case trim_keyframe:
    case trim_reencode:
    case trim_remove_middle: {
      if (!app->trim.armed()) return false;
      if (!app->trim.has_marker()) {
        ::MessageBeep(MB_ICONWARNING);
        return true;
      }
      const clip::op kind = command == trim_keyframe   ? clip::op::trim_keyframe
                            : command == trim_reencode ? clip::op::trim_reencode
                                                       : clip::op::remove_middle;
      return submit_clip_job(app, app->trim.request(kind));
    }
    case keyframe_prev:
    case keyframe_next: {
      if (!app->trim.armed() || !app->trim.index_ready()) return false;
      const std::int64_t at = clip_position(app);
      const std::int64_t to = command == keyframe_prev ? app->trim.prev_keyframe(at) : app->trim.next_keyframe(at);
      if (to != at) (void)mv_video_seek(app->session, to, 1);
      return true;
    }
    case jobs_pane:
      set_jobs_pane(app, !app->jobs_pane_visible);
      return true;
    case clip_tools:
      if (!video_mode(app) || !app->chrome.attached()) return false;
      app->popup_open = true;
      if (app->fullscreen) layout_chrome(app);  // the flyout hangs off the bar
      app->chrome.show_popup(mv::shell::chrome_popup::clip_tools, clip_tool_flags(app));
      return true;
    case clip_split: {
      if (!video_mode(app)) return false;
      clip::request r;
      if (!mv::shell::clip_tool_request(mv::shell::pack_clip_choice(clip::op::split, 0), current_item_path(app),
                                        clip_position(app), &app->trim, r)) {
        return false;
      }
      return submit_clip_job(app, r);
    }
    default:
      return false;
  }
}

// The flyout's answer (chrome_cmd_clip_tool).
void run_clip_tool(app_state* app, std::int32_t packed) noexcept {
  mv::edit::clip::request r;
  if (!video_mode(app) ||
      !mv::shell::clip_tool_request(packed, current_item_path(app), clip_position(app), &app->trim, r)) {
    ::MessageBeep(MB_ICONWARNING);
    return;
  }
  (void)submit_clip_job(app, r);
}

// ---- PR 29: the Edit workspace (docs/design/20) -----------------------------------------
// One visible door (the bar's Edit image / Edit video, Enter) to what PRs 10-14
// built. shell/edit_workspace decides which tab a command lands on; the host
// shows that tab's pane and runs the command's own work exactly as before. The
// workspace docks in the right column, so the canvas frames the picture beside
// it (input.chrome_right_px) instead of under it. The Mac host's twin is
// main_mac.mm "PR 29".

// DIP height of the strip (title, tabs, actions) at the top of the right column.
constexpr int kEditStripDip = 140;

mv::shell::edit_subject edit_subject_of(app_state* app) noexcept {
  if (!app || app->mode == open_mode::none || app->edit_path.empty()) return mv::shell::edit_subject::none;
  // An audio file plays through the video path but has nothing to edit.
  if (mv::shell::is_audio_name(app->edit_path)) return mv::shell::edit_subject::none;
  // A Live Photo's motion plays through the video path but the stop is a still.
  if (mv::shell::is_video_name(app->edit_path) || (video_mode(app) && !app->motion_playing)) {
    return mv::shell::edit_subject::clip;
  }
  if (app->lab.animation() != mv::shell::animation_state::none) return mv::shell::edit_subject::none;
  return mv::shell::edit_subject::still;
}

void push_edit_view(app_state* app) noexcept {
  if (!app) return;
  try {
    const mv::shell::edit_subject subject = edit_subject_of(app);
    mv::shell::chrome_edit_args a{};
    a.open = app->ws.open || app->editor.open ? 1 : 0;  // PR 30: the bar's button reads Done
    a.tab = static_cast<std::int32_t>(app->ws.tab);
    a.subject = static_cast<std::int32_t>(subject);
    a.crop_active = app->edits.crop_active() ? 1 : 0;
    a.aspect = static_cast<std::int32_t>(app->edits.aspect());
    a.portrait = app->edits.aspect_portrait() ? 1 : 0;
    a.straighten = app->edits.crop_active() ? app->edits.crop_angle() : app->edits.export_geometry().straighten;
    a.edit_count = static_cast<std::int32_t>(app->edits.edit_count());
    a.show_original = app->show_original ? 1 : 0;
    if (subject == mv::shell::edit_subject::still && app->edits.has_item()) {
      std::uint32_t w = 0, h = 0;
      if (app->lab.still_size(app->edit_key, &w, &h)) {
        app->edits.set_size(w, h);
        const mv::edit::placement p = app->edits.preview_placement();
        if (app->edits.crop_active()) {
          const mv::edit::rect r = app->edits.crop_overlay();
          a.crop_width = static_cast<std::int32_t>(std::lround(r.w * static_cast<float>(p.cropped.w)));
          a.crop_height = static_cast<std::int32_t>(std::lround(r.h * static_cast<float>(p.cropped.h)));
        } else {
          a.crop_width = static_cast<std::int32_t>(p.cropped.w);
          a.crop_height = static_cast<std::int32_t>(p.cropped.h);
        }
      }
    }
    std::string label;
    if (subject == mv::shell::edit_subject::clip && app->trim.armed() && app->trim.path() == app->edit_path) {
      a.trim_flags |= mv::shell::kEditTrimArmed;
      if (app->trim.previewing()) a.trim_flags |= mv::shell::kEditTrimPreviewing;
      if (app->trim.has_marker()) a.trim_flags |= mv::shell::kEditTrimHasMarker;
      label = app->trim.label();
    }
    const std::size_t slash = app->edit_path.find_last_of("\\/");
    const std::string name = slash == std::string::npos ? app->edit_path : app->edit_path.substr(slash + 1);
    a.name_utf8 = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(name.data()));
    a.name_len = static_cast<std::int32_t>(name.size());
    a.trim_label_utf8 = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(label.data()));
    a.trim_label_len = static_cast<std::int32_t>(label.size());
    app->chrome.set_edit_view(a);
  } catch (...) {
  }
}

// Shows what app->ws says: the strip, the tab's pane, the right-edge pane the
// tab is, and the canvas docked beside them. Closing hides only what the
// workspace had shown.
void sync_workspace(app_state* app) {
  using mv::shell::edit_tab;
  const bool open = app->ws.open;
  const edit_tab tab = app->ws.tab;
  // A crop draft belongs to the Crop tab: leaving it applies the draft
  // (Lightroom's rule), so no crop is lost to a tab click.
  if (app->edits.crop_active() && !(open && tab == edit_tab::crop)) {
    (void)run_edit_command(app, mv::shell::command_id::crop_commit);
  }
  if (open || app->ws_shown) {
    // One right-edge pane at a time: each setter closes the others.
    const bool colour = open && tab == edit_tab::colour;
    const bool info = open && tab == edit_tab::info;
    const bool jobs = open && tab == edit_tab::jobs;
    if (!colour && app->adjust.visible()) set_adjust_pane(app, false);
    if (!info && app->meta_pane_visible) set_meta_pane(app, false);
    if (!jobs && app->jobs_pane_visible) set_jobs_pane(app, false);
    if (colour) set_adjust_pane(app, true);
    if (info) set_meta_pane(app, true);
    if (jobs) set_jobs_pane(app, true, false);
  }
  app->ws_shown = open;
  // The strip and its pane move in, and the canvas refits into the rect beside
  // them (or back): one layout, one resize_seq, the same swapchain.
  apply_view_state(app);
  push_edit_view(app);
  // Crop and Trim keys are the canvas's: keep the keyboard there.
  if (open && (tab == edit_tab::crop || tab == edit_tab::trim)) focus_canvas(app);
}

void close_workspace(app_state* app) {
  if (!app->ws.open) return;
  app->ws.open = false;
  sync_workspace(app);
}

// The item on the canvas changed (a select, a clip starting to play).
// A reload can briefly leave nothing selected: a metadata write rewrites a
// JPEG in place and the folder watcher relists it. Closing the workspace on
// that would drop the user out of the Info tab mid-edit (seen 2026-09-27, 1 run
// in 3), so "nothing to edit" is confirmed once more before it closes.
constexpr UINT_PTR kWorkspaceFollowTimerId = 0x7A02;
constexpr UINT kWorkspaceFollowMs = 400;

void workspace_item_changed(app_state* app) noexcept {
  if (!app) return;
  try {
    // PR 30: the Video Editor edits the clip it opened; another item ends it.
    if (app->editor.open && current_item_path(app) != app->editor.path) set_editor_open(app, false);
    const mv::shell::edit_subject subject = edit_subject_of(app);
    if (app->ws.open && subject == mv::shell::edit_subject::none && app->window) {
      ::SetTimer(app->window, kWorkspaceFollowTimerId, kWorkspaceFollowMs, nullptr);
      push_edit_view(app);
      return;
    }
    if (app->window) ::KillTimer(app->window, kWorkspaceFollowTimerId);
    if (mv::shell::follow_subject(app->ws, subject)) sync_workspace(app);
    else push_edit_view(app);
  } catch (...) {
  }
}

// The re-check: still nothing editable, so the workspace follows and closes.
void workspace_follow_settled(app_state* app) noexcept {
  if (!app) return;
  if (app->window) ::KillTimer(app->window, kWorkspaceFollowTimerId);
  try {
    if (mv::shell::follow_subject(app->ws, edit_subject_of(app))) sync_workspace(app);
    else push_edit_view(app);
  } catch (...) {
  }
}

// The strip's tab row.
void edit_select_tab(app_state* app, int tab) {
  if (tab < 0 || tab >= static_cast<int>(mv::shell::edit_tab::count)) return;
  const auto t = static_cast<mv::shell::edit_tab>(tab);
  if (!mv::shell::tab_offered(edit_subject_of(app), t)) return;
  if (mv::shell::apply_step(app->ws, {mv::shell::workspace_action::select, t})) sync_workspace(app);
}

// The Crop pane's buttons and slider: stills only, once the pixels are known.
bool prepare_still_edit(app_state* app) noexcept {
  if (edit_subject_of(app) != mv::shell::edit_subject::still) return false;
  std::uint32_t w = 0, h = 0;
  if (!app->lab.still_size(app->edit_key, &w, &h)) return false;
  app->edits.set_size(w, h);
  return true;
}

// crop_aspect_set's argument: the preset, + 16 for portrait.
void edit_set_aspect(app_state* app, int packed) {
  const int aspect = packed & 15;
  if (packed < 0 || aspect >= mv::shell::kCropAspectCount || !prepare_still_edit(app)) {
    ::MessageBeep(MB_ICONWARNING);
    return;
  }
  apply_edit_effect(app, app->edits.set_crop_aspect(static_cast<mv::shell::crop_aspect>(aspect), (packed & 16) != 0));
}

void edit_set_straighten(app_state* app, float degrees) {
  if (!prepare_still_edit(app)) {
    ::MessageBeep(MB_ICONWARNING);
    return;
  }
  apply_edit_effect(app, app->edits.set_straighten(degrees));
}

void set_show_original(app_state* app, bool on) noexcept {
  if (app->show_original == on) return;
  app->show_original = on;
  publish_edit(app);
  ++app->input.activity_seq;
  publish(app);
}

void run_edit_action(app_state* app, int action) {
  switch (static_cast<mv::shell::chrome_edit_action>(action)) {
    case mv::shell::chrome_edit_action::cancel_crop:
      if (!app->edits.crop_active()) return;
      app->edits.cancel_crop();
      publish_edit(app);
      ++app->input.activity_seq;
      publish(app);
      return;
    case mv::shell::chrome_edit_action::original_off:
    case mv::shell::chrome_edit_action::original_on:
      if (edit_subject_of(app) != mv::shell::edit_subject::still) return;
      set_show_original(app, action == static_cast<int>(mv::shell::chrome_edit_action::original_on));
      return;
    case mv::shell::chrome_edit_action::save_copy:
      // Save copy…: apply a crop draft, then PR 10's export (a new file).
      if (app->edits.crop_active()) (void)run_edit_command(app, mv::shell::command_id::crop_commit);
      (void)run_edit_command(app, mv::shell::command_id::export_image);
      return;
  }
}

// Native decides the rate and then tells the dropdown, rather than the two
// agreeing by luck. Same one-direction rule as the settings flags.
void apply_rate(app_state* app, int index) noexcept {
  if (!app || !app->session) return;
  if (index < 0) index = 0;
  if (index >= kRateLadderCount) index = kRateLadderCount - 1;
  app->rate_index = index;
  (void)mv_video_set_rate(app->session, kRateLadder[index]);
  app->chrome.apply_rate(static_cast<float>(kRateLadder[index]));
}

// Nearest rung to a rate the island picked, so the keyboard carries on from
// wherever the dropdown left off.
int rate_index_for(double rate) noexcept {
  int best = kRateDefaultIndex;
  double best_delta = 1e9;
  for (int i = 0; i < kRateLadderCount; ++i) {
    const double delta = rate > kRateLadder[i] ? rate - kRateLadder[i] : kRateLadder[i] - rate;
    if (delta < best_delta) { best_delta = delta; best = i; }
  }
  return best;
}

bool skim(app_state* app, std::int64_t delta_ns, bool exact) noexcept {
  if (!app || !app->session) return false;
  const std::uint64_t now = ::GetTickCount64();
  std::int64_t base = app->skim_target_ns;
  if (app->skim_tick_ms == 0 || now - app->skim_tick_ms > kSkimBurstMs) {
    if (mv_video_position(app->session, &base) != MV_OK) return false;
  }
  std::int64_t want = base + delta_ns;
  if (want < 0) want = 0;
  app->skim_target_ns = want;
  app->skim_tick_ms = now;
  return mv_video_seek(app->session, want, exact ? 1u : 0u) == MV_OK;
}

std::uint32_t folder_count(app_state* app) noexcept {
  std::uint32_t count = 0;
  if (!app || !app->session) return 0;
  if (mv_folder_count(app->session, &count) != MV_OK) return 0;
  return count;
}

std::uint32_t subfolder_count(app_state* app) noexcept {
  std::uint32_t count = 0;
  if (!app || !app->session) return 0;
  if (mv_folder_subfolder_count(app->session, &count) != MV_OK) return 0;
  return count;
}

int chrome_bar_px(app_state*, std::uint32_t dpi) noexcept {
  // The folder trail lives inside the bar, so opening a folder does not grow it.
  return mv::shell::chrome_bar_height_px(dpi);
}

std::string subfolder_path_at(app_state* app, std::uint32_t index) {
  if (!app || !app->session) return {};
  char buf[4096]{};
  std::uint32_t bytes = 0;
  if (mv_folder_subfolder_path(app->session, index, buf, sizeof(buf), &bytes) != MV_OK) return {};
  buf[sizeof(buf) - 1] = '\0';
  return buf;
}

std::string subfolder_name_at(app_state* app, std::uint32_t index) {
  if (!app || !app->session) return {};
  char buf[1024]{};
  std::uint32_t bytes = 0;
  if (mv_folder_subfolder_name(app->session, index, buf, sizeof(buf), &bytes) != MV_OK) return {};
  buf[sizeof(buf) - 1] = '\0';
  return buf;
}

bool folder_name_starts_with(std::string_view name, std::string_view query) {
  if (query.empty()) return true;
  const std::wstring wide_name = wide_from_utf8(name);
  const std::wstring wide_query = wide_from_utf8(query);
  if (wide_query.size() > wide_name.size()) return false;
  return ::CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, wide_name.data(),
                           static_cast<int>(wide_query.size()), wide_query.data(),
                           static_cast<int>(wide_query.size()), nullptr, nullptr, 0) == CSTR_EQUAL;
}

void seed_siblings_for(app_state* app, const std::string& dir) {
  if (!app) return;
  const std::uint32_t n = subfolder_count(app);
  for (std::uint32_t i = 0; i < n; ++i) {
    if (subfolder_path_at(app, i) != dir) continue;
    app->siblings.clear();
    app->siblings.reserve(n);
    for (std::uint32_t j = 0; j < n; ++j) app->siblings.push_back(subfolder_path_at(app, j));
    app->sibling_index = static_cast<int>(i);
    return;
  }
  for (std::size_t i = 0; i < app->siblings.size(); ++i) {
    if (app->siblings[i] == dir) {
      app->sibling_index = static_cast<int>(i);
      return;
    }
  }
  app->siblings.clear();
  app->sibling_index = -1;
}

void refresh_siblings(app_state* app) {
  if (!app || !app->window) return;
  const std::string parent = app->trail.parent();
  const std::string here = app->current_dir;
  const std::uint64_t gen = ++app->sibling_generation;
  if (parent.empty() || here.empty()) return;
  const HWND hwnd = app->window;
  app->jobs.submit_at(mv::background_generation,
                      [parent, here, gen, hwnd](const mv::job_context&) -> mv::status {
                        auto subs = mv::io::list_subfolders(parent);
                        auto* r = new (std::nothrow) sibling_job_result{};
                        if (!r) return subs ? mv::status::ok : subs.error();
                        r->generation = gen;
                        r->here = here;
                        if (subs) {
                          r->paths.reserve(subs.value().size());
                          for (std::size_t i = 0; i < subs.value().size(); ++i) {
                            r->paths.push_back(subs.value()[i].path_utf8);
                            if (subs.value()[i].path_utf8 == here) {
                              r->index = static_cast<int>(i);
                            }
                          }
                        }
                        if (!::PostMessageW(hwnd, kMsgSiblingsReady, 0, reinterpret_cast<LPARAM>(r))) {
                          delete r;
                        }
                        return subs ? mv::status::ok : subs.error();
                      });
}

void on_siblings_ready(app_state* app, std::unique_ptr<sibling_job_result> r) {
  if (!app || !r) return;
  if (r->generation != app->sibling_generation || r->here != app->current_dir) return;
  app->siblings = std::move(r->paths);
  app->sibling_index = r->index;
}

bool folder_find_live(app_state* app) {
  if (!app || !app->folder_find) return false;
  if (::GetTickCount64() - app->folder_find_tick > 1200) {
    app->folder_find = false;
    app->folder_query.clear();
    return false;
  }
  return true;
}

void touch_folder_find(app_state* app) {
  if (!app) return;
  app->folder_find = true;
  app->folder_find_tick = ::GetTickCount64();
}

void clear_folder_find(app_state* app) {
  if (!app) return;
  app->folder_find = false;
  app->folder_query.clear();
}

void move_folder_cursor_to_query(app_state* app) {
  if (!app || app->folder_query.empty()) return;
  const std::uint32_t n = subfolder_count(app);
  for (std::uint32_t i = 0; i < n; ++i) {
    if (!folder_name_starts_with(subfolder_name_at(app, i), app->folder_query)) continue;
    app->folder_cursor = static_cast<int>(i);
    set_gallery(app, true);
    return;
  }
}

void push_browse_state(app_state* app) {
  if (!app || !app->chrome.attached()) return;
  std::string blob;
  for (const auto& c : app->trail.crumbs()) {
    blob += c.name;
    blob += '\t';
    blob += c.path;
    blob += '\n';
  }
  // Milestone H: a result list is one step below the folder it came from.
  // Its crumb has no path (the gallery shows it as the current name), and Up
  // goes back to the folder.
  const bool list = !app->list_title.empty();
  if (list) {
    blob += "Search: ";
    for (const char ch : app->list_title) blob += ch == '\t' || ch == '\n' ? ' ' : ch;
    blob += "\t\n";
  }
  const bool finding = folder_find_live(app);
  app->chrome.apply_browse(app->folder_cursor, list || !app->trail.parent().empty(), blob, finding,
                           app->folder_query);
}

// Milestone H: back from a result list to the folder it was opened over, on
// the item that was open then. False when no list is open.
bool leave_result_list(app_state* app) {
  if (!app || app->list_title.empty() || app->current_dir.empty()) return false;
  const std::string select = app->list_return_select;
  open_folder(app, wide_from_utf8(app->current_dir), wide_from_utf8(select), true);
  return true;
}

void open_utf8_dir(app_state* app, std::string_view utf8, bool navigation) {
  const std::wstring wide = wide_from_utf8(utf8);
  if (wide.empty()) return;
  open_path(app, wide, navigation);
}

void open_subfolder_at(app_state* app, std::uint32_t index) {
  if (!app || !app->session) return;
  char buf[4096]{};
  std::uint32_t bytes = 0;
  if (mv_folder_subfolder_path(app->session, index, buf, sizeof(buf), &bytes) != MV_OK) return;
  buf[sizeof(buf) - 1] = '\0';
  open_utf8_dir(app, buf, true);
}

void open_crumb_at(app_state* app, std::int32_t index) {
  if (!app) return;
  const auto crumbs = app->trail.crumbs();
  if (index < 0 || static_cast<std::size_t>(index) >= crumbs.size()) return;
  if (crumbs[static_cast<std::size_t>(index)].path == app->trail.current()) return;
  open_utf8_dir(app, crumbs[static_cast<std::size_t>(index)].path, true);
}

bool navigate_folder_up(app_state* app) {
  if (!app) return false;
  if (leave_result_list(app)) return true;
  const std::string parent = app->trail.parent();
  if (parent.empty()) return false;
  open_utf8_dir(app, parent, true);
  return true;
}

bool navigate_sibling(app_state* app, int delta) {
  if (!app || app->sibling_index < 0 || app->siblings.empty()) return false;
  const int next = app->sibling_index + delta;
  if (next < 0 || next >= static_cast<int>(app->siblings.size())) return false;
  const std::string path = app->siblings[static_cast<std::size_t>(next)];
  app->sibling_index = next;
  open_utf8_dir(app, path, true);
  return true;
}

// Mac's galleryMoveRows: a mixed folder keeps folders in one strip; a
// folders-only view shares a column count with nothing, so Up/Down stay in
// the same column. Mixed Up/Down cross the strip in one step.
void gallery_move_rows(app_state* app, int rows) {
  if (!app || rows == 0) return;
  const int cols = std::max(1, app->gallery_columns);
  const int folders = static_cast<int>(subfolder_count(app));
  const int items = static_cast<int>(folder_count(app));
  if (folders > 0 && items > 0) {
    if (app->folder_cursor >= 0) {
      if (rows < 0) return;
      const int col = std::min(app->folder_cursor, cols - 1);
      app->folder_cursor = -1;
      folder_select(app, static_cast<std::uint32_t>(std::min(col, items - 1)));
      push_browse_state(app);
      return;
    }
    std::uint32_t selected = 0;
    if (mv_folder_selected(app->session, &selected) != MV_OK) selected = 0;
    if (rows < 0 && static_cast<int>(selected) < cols) {
      app->folder_cursor = std::min(static_cast<int>(selected), folders - 1);
      push_browse_state(app);
      return;
    }
  }
  if (items == 0) {
    if (folders == 0) return;
    if (app->folder_cursor < 0) app->folder_cursor = 0;
  }
  if (app->folder_cursor >= 0) {
    int target = app->folder_cursor + rows * cols;
    if (target < 0) return;
    if (target >= folders) {
      if (app->folder_cursor / cols < (folders - 1) / cols) {
        target = folders - 1;
      } else {
        if (items == 0) return;
        const int col = app->folder_cursor % cols;
        app->folder_cursor = -1;
        folder_select(app, static_cast<std::uint32_t>(std::min(col, items - 1)));
        push_browse_state(app);
        return;
      }
    }
    app->folder_cursor = target;
    push_browse_state(app);
    return;
  }
  std::uint32_t selected = 0;
  if (mv_folder_selected(app->session, &selected) != MV_OK) selected = 0;
  const int cur = static_cast<int>(selected);
  int target = cur + rows * cols;
  if (target < 0) {
    if (folders > 0 && rows < 0) {
      const int col = cur % cols;
      app->folder_cursor = std::min(folders - 1, ((folders - 1) / cols) * cols + col);
      push_browse_state(app);
    }
    return;
  }
  if (target >= items) {
    if (cur / cols >= (items - 1) / cols) return;
    target = items - 1;
  }
  folder_select(app, static_cast<std::uint32_t>(target));
}

bool folder_cursor_step(app_state* app, int delta) {
  if (!app || !app->gallery_visible) return false;
  const int folders = static_cast<int>(subfolder_count(app));
  if (folders == 0) return false;
  if (app->folder_cursor < 0) {
    if (folder_count(app) > 0) return false;
    app->folder_cursor = 0;
  }
  app->folder_cursor = std::clamp(app->folder_cursor + delta, 0, folders - 1);
  push_browse_state(app);
  return true;
}

// Mac toggles the gallery whenever a folder is open, even a leaf of one photo
// or a folder of folders. The empty state is a real view, not a refused key.
bool gallery_available(app_state* app) noexcept {
  return app && app->chrome.gallery_attached() && app->mode != open_mode::none;
}

void set_gallery(app_state* app, bool visible) {
  if (!app) return;
  if (visible && !gallery_available(app)) return;
  if (app->gallery_visible == visible) return;
  app->gallery_visible = visible;
  sync_video_hold(app);
  apply_view_state(app);
}

// Issue #44. The grid covers the canvas, so a clip does not play or sound under
// it: the core pauses one that is playing, leaves one selected under the grid on
// its first frame, and on the way out resumes only the clip that was playing
// when the grid opened (player/playback_hold.h, the rule the Mac host runs).
// `resume` false is for leaving the grid for something new, not back to the clip.
void sync_video_hold(app_state* app, bool resume) noexcept {
  if (!app || !app->session || app->video_held == app->gallery_visible) return;
  app->video_held = app->gallery_visible;
  (void)mv_video_set_hold(app->session, app->video_held ? 1 : 0, resume ? 1 : 0);
}

void toggle_filmstrip_setting(app_state* app) {
  if (!app) return;
  // Nothing open yet means there is no mode to toggle for. Writing the folder
  // preference here would change what the *next* folder does from an empty
  // window, with nothing on screen to show it happened — a persisted setting
  // silently flipped by a key that looked like it did nothing.
  if (app->mode == open_mode::none) return;
  // T toggles the strip for the mode you are in, and that is the preference
  // that gets written: turning it off while browsing a folder should not also
  // turn it off for the single images you open from Explorer.
  bool& flag = app->mode == open_mode::image ? app->settings.filmstrip_for_image
                                             : app->settings.filmstrip_for_folder;
  flag = !flag;
  mv::shell::save_view_settings(app->settings);
  app->chrome.apply_settings(chrome_flags(app), app->settings.sort);
  apply_view_state(app);
}

// ---- PR 30: the Video Editor window (docs/design/21, owner 2026-09-26) ----------------
//
// Enter (or Edit video) on a clip opens a window of its own: the preview on top
// -- the viewer's canvas, moved in, so there is still one swapchain and one
// present path (rule 2): the render thread retargets its DComp visual to this
// window (input.canvas_window) -- and the timeline island under it
// (IslandHost.VideoEditor.cs). The cut list is shell::video_timeline, shared
// with the Mac host; playback follows it by jumping the player over each cut;
// Export is clip::op::keep_ranges through the Jobs queue. main_mac.mm "PR 30"
// is the twin.

constexpr wchar_t kEditorWindowClass[] = L"MediaViewer.VideoEditor";
constexpr int kEditorTimelineDip = 280;        // the timeline island under the preview
constexpr int kEditorWidthDip = 1180;
constexpr int kEditorHeightDip = 820;
constexpr int kEditorMinWidthDip = 720;
constexpr int kEditorMinHeightDip = 520;
constexpr int kEditorThumbs = 48;              // thumbnails across the source
constexpr std::uint32_t kEditorThumbPx = 96;   // their height in pixels
constexpr std::uint32_t kEditorPeaks = 1200;
constexpr std::int64_t kEditorLeadNs = 20'000'000;  // a frame's worth, so a cut is not glimpsed
// While the editor is open: playback skips what was cut. USER timers round to
// the system tick (~15.6 ms), close to the Mac's 60 Hz NSTimer.
constexpr UINT_PTR kEditorTickTimerId = 0x7B01;
constexpr UINT kEditorTickMs = 15;
// After close: the editor window is destroyed once the render thread has moved
// the swapchain back out of it (lab.canvas_window()), never under it.
constexpr UINT_PTR kEditorReleaseTimerId = 0x7B02;
constexpr UINT kEditorReleaseMs = 16;
constexpr int kEditorReleaseTries = 120;
constexpr UINT kMsgEditorLoaded = WM_APP + 0x79;  // the probe + strip job finished (any thread posts)

struct editor_load_result {
  std::uint64_t token = 0;
  std::int64_t duration_ns = 0;
  double fps = 0;
  std::vector<mv::edit::clip::strip_frame> strip;
  std::vector<float> peaks;
};

void update_client_metrics(app_state* app, HWND hwnd) noexcept;
int right_pane_px(int client_width, std::uint32_t dpi) noexcept;
void enable_dark_titlebar(HWND hwnd) noexcept;
void set_editor_open(app_state* app, bool open);

std::string editor_name(const app_state* app) {
  const std::size_t slash = app->editor.path.find_last_of("\\/");
  return slash == std::string::npos ? app->editor.path : app->editor.path.substr(slash + 1);
}

// Where the playhead is on the program (the edited timeline).
std::int64_t editor_timeline_position(app_state* app) noexcept {
  const mv::shell::video_timeline& tl = app->editor.timeline;
  if (!tl.loaded() || !video_mode(app)) return 0;
  const std::int64_t source = clip_position(app);
  if (const auto t = tl.to_timeline(source)) return *t;
  // In a cut (the jump has not landed yet): the start of the next piece.
  const std::int64_t next = tl.next_play_start(source);
  if (next < 0) return tl.length();
  return tl.to_timeline(next).value_or(0);
}

bool editor_playing(app_state* app) noexcept {
  std::uint32_t state = MV_PLAY_STOPPED;
  return app->session && mv_video_state(app->session, &state) == MV_OK && state == MV_PLAY_PLAYING;
}

// What the timeline shows. `edit` bumps the generation (the pieces, the
// selection or the strip changed); the tick pushes only a moved playhead.
void push_editor_view(app_state* app, bool edit) noexcept {
  if (!app || !app->editor.open) return;
  try {
    auto& ed = app->editor;
    if (edit) ++ed.generation;
    const std::int64_t playhead = editor_timeline_position(app);
    const bool playing = editor_playing(app);
    if (!edit && playhead == ed.pushed_playhead && playing == ed.pushed_playing) return;
    ed.pushed_playhead = playhead;
    ed.pushed_playing = playing;
    std::vector<std::int64_t> flat;
    flat.reserve(ed.timeline.pieces().size() * 2);
    for (const auto& p : ed.timeline.pieces()) {
      flat.push_back(p.in_ns);
      flat.push_back(p.out_ns);
    }
    const std::string name = editor_name(app);
    mv::shell::chrome_editor_view_args a{};
    a.open = 1;
    a.ready = ed.timeline.loaded() ? 1 : 0;
    a.length_ns = ed.timeline.length();
    a.playhead_ns = playhead;
    a.source_ns = ed.timeline.source_duration();
    a.playing = playing ? 1 : 0;
    a.piece_count = static_cast<std::int32_t>(ed.timeline.pieces().size());
    a.selected = ed.selected;
    a.can_undo = ed.timeline.can_undo() ? 1 : 0;
    a.can_redo = ed.timeline.can_redo() ? 1 : 0;
    a.edited = ed.timeline.edited() ? 1 : 0;
    a.generation = ed.generation;
    a.pieces = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(flat.data()));
    a.name_utf8 = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(name.data()));
    a.name_len = static_cast<std::int32_t>(name.size());
    a.frame_rate_milli = static_cast<std::int32_t>(std::lround(ed.fps * 1000.0));
    a.mark_in_ns = ed.timeline.marked_in();
    a.mark_out_ns = ed.timeline.marked_out();
    app->chrome.set_editor_view(a);
  } catch (...) {
  }
}

void push_editor_strip(app_state* app) noexcept {
  if (!app || !app->editor.open) return;
  try {
    std::vector<mv::shell::chrome_editor_thumb> thumbs;
    thumbs.reserve(app->editor.strip.size());
    for (const auto& f : app->editor.strip) {
      mv::shell::chrome_editor_thumb t{};
      t.shown_ns = f.shown_ns;
      t.width = static_cast<std::int32_t>(f.width);
      t.height = static_cast<std::int32_t>(f.height);
      t.rgba = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(f.rgba.data()));
      thumbs.push_back(t);
    }
    mv::shell::chrome_editor_strip_args a{};
    a.thumbs = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(thumbs.data()));
    a.thumb_count = static_cast<std::int32_t>(thumbs.size());
    a.peaks = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(app->editor.peaks.data()));
    a.peak_count = static_cast<std::int32_t>(app->editor.peaks.size());
    app->chrome.set_editor_strip(a);
  } catch (...) {
  }
}

int editor_timeline_px(int client_height, std::uint32_t dpi) noexcept {
  return std::min(::MulDiv(kEditorTimelineDip, static_cast<int>(dpi), 96), std::max(client_height - 1, 1));
}

// The canvas's rectangle while the editor has it: the preview, above the
// timeline, with nothing drawn over it.
void editor_canvas_metrics(app_state* app) noexcept {
  RECT rc{};
  ::GetClientRect(app->editor.window, &rc);
  const auto dpi = ::GetDpiForWindow(app->editor.window);
  const int width = rc.right - rc.left;
  const int height = rc.bottom - rc.top;
  app->input.width = static_cast<std::uint32_t>(std::max(width, 1));
  app->input.height = static_cast<std::uint32_t>(std::max(height - editor_timeline_px(height, dpi), 1));
  app->input.dpi_scale = static_cast<float>(dpi) / 96.0f;
  app->input.chrome_height_px = 0;
  app->input.chrome_bottom_px = 0;
  app->input.chrome_left_px = 0;
  app->input.chrome_right_px = 0;
}

// Both islands: the timeline under the preview, and the viewer's card over the
// canvas area it left (beside a right-edge pane, so Jobs stays readable).
void layout_editor(app_state* app, bool focus = false) noexcept {
  if (!app || !app->editor.open || !app->editor.window) return;
  RECT rc{};
  ::GetClientRect(app->editor.window, &rc);
  const auto dpi = ::GetDpiForWindow(app->editor.window);
  const int width = rc.right - rc.left;
  const int height = rc.bottom - rc.top;
  const int timeline = editor_timeline_px(height, dpi);
  mv::shell::chrome_editor_layout_args a{};
  a.x = 0;
  a.y = height - timeline;
  a.width = width;
  a.height = timeline;
  a.focus = focus ? 1 : 0;
  if (app->window) {
    RECT vr{};
    ::GetClientRect(app->window, &vr);
    const auto vdpi = ::GetDpiForWindow(app->window);
    const int vw = vr.right - vr.left;
    const int bar = app->chrome_on_screen && !app->fullscreen ? chrome_bar_px(app, vdpi) : 0;
    const bool right_pane = app->chrome.jobs_pane_visible() || app->chrome.meta_pane_visible() ||
                            app->chrome.adjust_pane_visible();
    const int left =
        app->chrome.folder_tree_visible() ? std::min(vw / 2, ::MulDiv(280, static_cast<int>(vdpi), 96)) : 0;
    const int right = right_pane ? right_pane_px(vw, vdpi) : 0;
    a.away_visible = 1;
    a.away_x = left;
    a.away_y = bar;
    a.away_width = std::max(vw - left - right, 1);
    a.away_height = std::max(static_cast<int>(vr.bottom - vr.top) - bar, 1);
  }
  app->chrome.layout_editor(a);
}

void editor_publish_canvas(app_state* app) noexcept {
  editor_canvas_metrics(app);
  ++app->input.resize_seq;
  ++app->input.activity_seq;
  publish(app);
}

void editor_seek_source(app_state* app, std::int64_t source_ns) noexcept {
  if (app->session) (void)mv_video_seek(app->session, std::max<std::int64_t>(0, source_ns), 1);
}

void editor_seek(app_state* app, std::int64_t timeline_ns) noexcept {
  auto& ed = app->editor;
  if (!ed.timeline.loaded()) return;
  const std::int64_t t = std::clamp<std::int64_t>(timeline_ns, 0, ed.timeline.length());
  ed.last_seek = -1;
  editor_seek_source(app, ed.timeline.to_source(t));
  ed.selected = static_cast<std::int32_t>(ed.timeline.piece_at(t));
  push_editor_view(app, true);
}

void editor_set_rate(app_state* app, double rate) noexcept {
  auto& ed = app->editor;
  if (!app->session) return;
  if (rate == 1.0 && !ed.rate_changed) return;
  (void)mv_video_set_rate(app->session, rate);
  ed.rate_changed = rate != 1.0;
}

void editor_toggle_play(app_state* app) noexcept {
  auto& ed = app->editor;
  if (!ed.timeline.loaded() || !app->session) return;
  ed.shuttle.stop();
  editor_set_rate(app, 1.0);  // Space plays at 1x
  const bool playing = editor_playing(app);
  // At the end, Play starts the program again.
  if (!playing && ed.timeline.next_play_start(clip_position(app), kEditorLeadNs) < 0) {
    editor_seek_source(app, ed.timeline.pieces().front().in_ns);
  }
  if (playing) (void)mv_video_pause(app->session);
  else (void)mv_video_play(app->session);
  push_editor_view(app, false);
}

// The tick: playback skips what was cut, and stops at the end of the program.
void editor_follow_playback(app_state* app) noexcept {
  auto& ed = app->editor;
  if (!ed.open || !ed.timeline.loaded() || !video_mode(app)) return;
  if (editor_playing(app)) {
    const std::int64_t source = clip_position(app);
    const std::int64_t want = ed.timeline.next_play_start(source, kEditorLeadNs);
    if (want < 0) {
      (void)mv_video_pause(app->session);  // the end of the edit: pause there
      editor_seek_source(app, ed.timeline.pieces().back().out_ns - 1);
    } else if (want != source && want != ed.last_seek) {
      ed.last_seek = want;
      editor_seek_source(app, want);
    }
  } else {
    ed.last_seek = -1;
  }
  push_editor_view(app, false);
}

// 1 split at the playhead, 2 delete (the marked range, else the selected
// piece), 3 set in, 4 set out, 5 undo, 6 redo, 14 mark in, 15 mark out,
// 16 clear the marks (chrome_editor_action, and the Mac bridge's codes).
void editor_edit(app_state* app, int what) noexcept {
  auto& ed = app->editor;
  if (!ed.timeline.loaded()) return;
  const std::int64_t at = editor_timeline_position(app);
  bool changed = false;
  switch (what) {
    case 1:
      changed = ed.timeline.split(at);
      if (changed) ed.selected = static_cast<std::int32_t>(ed.timeline.piece_at(at));
      break;
    case 2:
      if (ed.timeline.has_marks()) {  // a marked range first, as in every editor
        const std::int64_t from = std::max<std::int64_t>(0, ed.timeline.marked_in());
        changed = ed.timeline.remove_marked();
        if (changed) editor_seek(app, std::min(from, ed.timeline.length()));
        break;
      }
      if (ed.selected < 0) ed.selected = static_cast<std::int32_t>(ed.timeline.piece_at(at));
      changed = ed.timeline.remove(static_cast<std::size_t>(ed.selected));
      if (changed) {
        ed.selected = std::min<std::int32_t>(ed.selected, static_cast<std::int32_t>(ed.timeline.pieces().size()) - 1);
        editor_seek(app, ed.timeline.piece_start(static_cast<std::size_t>(ed.selected)));
      }
      break;
    case 3:
      changed = ed.timeline.set_in(at);
      if (changed) editor_seek(app, 0);
      break;
    case 4: changed = ed.timeline.set_out(at); break;
    case 5: changed = ed.timeline.undo(); break;
    case 6: changed = ed.timeline.redo(); break;
    // Marks change no piece: they never beep and never count as an edit.
    case 14: ed.timeline.mark_in(at); push_editor_view(app, true); return;
    case 15: ed.timeline.mark_out(at); push_editor_view(app, true); return;
    case 16:
      if (!ed.timeline.has_marks()) ::MessageBeep(MB_ICONWARNING);
      ed.timeline.clear_marks();
      push_editor_view(app, true);
      return;
    default: break;
  }
  if (!changed) ::MessageBeep(MB_ICONWARNING);
  if (ed.selected >= static_cast<std::int32_t>(ed.timeline.pieces().size())) ed.selected = -1;
  push_editor_view(app, true);
}

void editor_select(app_state* app, std::int32_t index) noexcept {
  auto& ed = app->editor;
  ed.selected = index >= 0 && index < static_cast<std::int32_t>(ed.timeline.pieces().size()) ? index : -1;
  push_editor_view(app, true);
}

void editor_export(app_state* app, bool exact) noexcept {
  auto& ed = app->editor;
  if (!ed.timeline.loaded() || ed.path.empty()) return;
  if (!ed.timeline.edited()) {
    ::MessageBeep(MB_ICONWARNING);
    notice_show(app, "Nothing has been cut yet");
    return;
  }
  try {
    (void)submit_clip_job(app, ed.timeline.export_request(ed.path, exact));
    ed.exported_revision = ed.timeline.revision();
  } catch (...) {
    ::MessageBeep(MB_ICONWARNING);
    return;
  }
  notice_show(app, exact ? "Exporting the edit (exact) \xE2\x80\x94 see Jobs" : "Exporting the edit \xE2\x80\x94 see Jobs");
  layout_editor(app);  // the Jobs pane took the viewer's right edge: the card steps aside
  push_editor_view(app, true);
}

// A piece's edge dragged on the timeline. The preview shows the edge's frame:
// a keyframe seek while it moves, the exact frame on release, as the viewer's
// scrubber does. The out edge shows the last frame kept.
std::int64_t editor_edge_frame(const app_state* app, std::int64_t at) noexcept {
  return app->editor.trim_in ? at : std::max<std::int64_t>(0, at - 1);
}

void editor_trim_grab(app_state* app, int code) noexcept {
  auto& ed = app->editor;
  if (!ed.timeline.loaded()) return;
  if (code < 0) {  // let go
    if (!ed.timeline.trimming()) return;
    ed.timeline.end_trim();
    const auto& pieces = ed.timeline.pieces();
    if (ed.trim_index >= 0 && static_cast<std::size_t>(ed.trim_index) < pieces.size()) {
      const auto& p = pieces[static_cast<std::size_t>(ed.trim_index)];
      editor_seek_source(app, editor_edge_frame(app, ed.trim_in ? p.in_ns : p.out_ns));
    }
    ed.trim_index = -1;
    push_editor_view(app, true);
    return;
  }
  const int index = code / 2;
  const bool in = code % 2 == 0;
  if (!ed.timeline.begin_trim(static_cast<std::size_t>(index),
                              in ? mv::shell::video_timeline::edge::in : mv::shell::video_timeline::edge::out)) {
    return;
  }
  ed.trim_index = index;
  ed.trim_in = in;
  ed.selected = index;
  if (editor_playing(app)) (void)mv_video_pause(app->session);
  push_editor_view(app, true);
}

void editor_trim_to(app_state* app, std::int64_t source_ns) noexcept {
  auto& ed = app->editor;
  const std::int64_t at = ed.timeline.trim_to(source_ns);
  if (at < 0) return;
  if (app->session) (void)mv_video_seek(app->session, editor_edge_frame(app, at), 0);
  push_editor_view(app, true);
}

// J K L (docs/design/16): L plays at 1x, 2x, 4x on repeated presses; K stops; J
// skims back further on each quick press. A held J skims keyframes and
// settles on the exact frame when it is let go.
void editor_shuttle_key(app_state* app, int key, bool down, bool repeat) noexcept {
  auto& ed = app->editor;
  if (!ed.timeline.loaded() || !app->session) return;
  switch (key) {
    case 'J': {
      if (!down) {
        if (ed.shuttle_skimmed) editor_seek_source(app, ed.timeline.to_source(ed.shuttle_target));
        ed.shuttle_skimmed = false;
        return;
      }
      if (editor_playing(app)) (void)mv_video_pause(app->session);
      editor_set_rate(app, 1.0);
      const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch()).count();
      const std::int64_t to = ed.shuttle.back(editor_timeline_position(app), now, repeat);
      ed.shuttle_target = to;
      ed.shuttle_skimmed = repeat;
      ed.last_seek = -1;
      (void)mv_video_seek(app->session, ed.timeline.to_source(to), repeat ? 0 : 1);
      ed.selected = static_cast<std::int32_t>(ed.timeline.piece_at(to));
      push_editor_view(app, true);
      return;
    }
    case 'K':
      if (!down || repeat) return;
      ed.shuttle.stop();
      if (editor_playing(app)) (void)mv_video_pause(app->session);
      editor_set_rate(app, 1.0);
      push_editor_view(app, false);
      return;
    case 'L': {
      if (!down || repeat) return;
      const double rate = ed.shuttle.forward();
      (void)mv_video_set_rate(app->session, rate);
      ed.rate_changed = rate != 1.0;
      if (!editor_playing(app)) {
        if (ed.timeline.next_play_start(clip_position(app), kEditorLeadNs) < 0) {
          editor_seek_source(app, ed.timeline.pieces().front().in_ns);
        }
        (void)mv_video_play(app->session);
      }
      push_editor_view(app, false);
      return;
    }
    default: return;
  }
}

// Done, Esc, Ctrl+W, the close box, Enter in the viewer: an edit that has not
// been exported since it last changed asks first. Closing any other way (the
// self-test, another clip on the canvas, quitting) does not.
void editor_request_close(app_state* app) {
  auto& ed = app->editor;
  if (!ed.open || ed.close_prompt) return;
  if (ed.timeline.edited() && ed.timeline.revision() != ed.exported_revision && ed.window) {
    ed.close_prompt = true;
    const int answer = ::MessageBoxW(
        ed.window,
        L"The edit has not been exported. Closing the Video Editor discards it; the clip itself is never changed.",
        L"Discard this edit?", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
    ed.close_prompt = false;
    if (answer != IDOK || !ed.open) return;
  }
  set_editor_open(app, false);
}

void editor_run_action(app_state* app, int action) {
  using mv::shell::chrome_editor_action;
  switch (static_cast<chrome_editor_action>(action)) {
    case chrome_editor_action::split:
    case chrome_editor_action::remove:
    case chrome_editor_action::set_in:
    case chrome_editor_action::set_out:
    case chrome_editor_action::undo:
    case chrome_editor_action::redo:
    case chrome_editor_action::mark_in:
    case chrome_editor_action::mark_out:
    case chrome_editor_action::clear_marks: editor_edit(app, action); break;
    case chrome_editor_action::toggle_play: editor_toggle_play(app); break;
    case chrome_editor_action::step_back:
    case chrome_editor_action::step_forward:
      if (app->session) {
        (void)mv_video_step(app->session, action == static_cast<int>(chrome_editor_action::step_forward) ? 1 : -1);
      }
      break;
    case chrome_editor_action::export_keyframe: editor_export(app, false); break;
    case chrome_editor_action::export_exact: editor_export(app, true); break;
    case chrome_editor_action::close: editor_request_close(app); break;
    case chrome_editor_action::show:
      if (app->editor.window) ::SetForegroundWindow(app->editor.window);
      break;
  }
}

// Every key aimed at the editor window (its preview or its timeline) comes
// here, never to the browse router: A / D must not walk the folder out from
// under the edit. Tab and Enter fall through to the island (focus, buttons).
bool editor_key(app_state* app, const MSG& msg) {
  // J's release settles a skim; every other key acts on its press.
  if (msg.message == WM_KEYUP && msg.wParam == 'J') {
    editor_shuttle_key(app, 'J', false, false);
    return true;
  }
  if (msg.message != WM_KEYDOWN) return false;
  const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
  const bool repeat = (msg.lParam & (1 << 30)) != 0;
  auto& ed = app->editor;
  switch (msg.wParam) {
    case VK_SPACE: editor_toggle_play(app); return true;
    case VK_LEFT:
    case VK_RIGHT: {
      const int n = shift ? 10 : 1;
      if (app->session) (void)mv_video_step(app->session, msg.wParam == VK_LEFT ? -n : n);
      return true;
    }
    case VK_HOME: editor_seek(app, 0); return true;
    case VK_END: editor_seek(app, ed.timeline.length()); return true;
    case VK_DELETE:
    case VK_BACK: editor_edit(app, 2); return true;
    case VK_ESCAPE: editor_request_close(app); return true;
    case 'J':
    case 'K':
    case 'L':
      if (!ctrl) editor_shuttle_key(app, static_cast<int>(msg.wParam), true, repeat);
      return true;
    case 'I': if (!ctrl) editor_edit(app, 14); return true;
    case 'O': if (!ctrl) editor_edit(app, 15); return true;
    case 'X': if (!ctrl) editor_edit(app, 16); return true;
    case VK_OEM_4: if (!ctrl) editor_edit(app, 3); return true;  // [ trim start
    case VK_OEM_6: if (!ctrl) editor_edit(app, 4); return true;  // ] trim end
    case 'B': if (ctrl) editor_edit(app, 1); return true;
    case 'Z': if (ctrl) editor_edit(app, shift ? 6 : 5); return true;
    case 'Y': if (ctrl) editor_edit(app, 6); return true;
    case 'E': if (ctrl) editor_export(app, shift); return true;
    case 'W': if (ctrl) editor_request_close(app); return true;
    default: return false;
  }
}

void editor_load_clip(app_state* app) {
  const HWND hwnd = app->window;
  app->jobs.submit_at(mv::background_generation,
                      [path = app->editor.path, token = app->editor.token, hwnd](const mv::job_context&) -> mv::status {
                        auto info = mv::edit::clip::probe(path);
                        auto* r = new (std::nothrow) editor_load_result{};
                        if (!r) return mv::status::out_of_memory;
                        r->token = token;
                        r->duration_ns = info ? info->duration_ns : 0;
                        r->fps = info ? info->frame_rate : 0.0;
                        std::vector<std::int64_t> times;
                        for (int i = 0; i < kEditorThumbs && r->duration_ns > 0; ++i) {
                          times.push_back(r->duration_ns * i / kEditorThumbs);
                        }
                        if (!times.empty()) {
                          if (auto strip = mv::edit::clip::thumbnails(path, times, kEditorThumbPx)) {
                            r->strip = std::move(*strip);
                          }
                          if (auto peaks = mv::edit::clip::audio_peaks(path, kEditorPeaks)) {
                            r->peaks = std::move(*peaks);
                          }
                        }
                        const mv::status st = info ? mv::status::ok : info.error();
                        if (!::PostMessageW(hwnd, kMsgEditorLoaded, 0, reinterpret_cast<LPARAM>(r))) delete r;
                        return st;
                      });
}

void on_editor_loaded(app_state* app, std::unique_ptr<editor_load_result> r) {
  auto& ed = app->editor;
  if (!r || r->token != ed.token || !ed.open) return;  // another clip, or closed
  if (r->duration_ns <= 0) {
    ::MessageBeep(MB_ICONWARNING);
    notice_show(app, "This clip could not be opened for editing");
    set_editor_open(app, false);
    return;
  }
  ed.timeline.load(r->duration_ns);
  ed.fps = r->fps > 0 && r->fps < 1000 ? r->fps : 0;
  ed.exported_revision = ed.timeline.revision();
  ed.strip = std::move(r->strip);
  ed.peaks = std::move(r->peaks);
  push_editor_strip(app);
  push_editor_view(app, true);
}

LRESULT CALLBACK editor_window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  app_state* app = state_from(hwnd);
  if (!app || app->editor.window != hwnd) return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  switch (msg) {
    case WM_SIZE:
      if (!app->editor.open) return 0;
      app->editor.minimized = wparam == SIZE_MINIMIZED;
      app->input.window_visible = !app->editor.minimized;
      if (!app->editor.minimized) {
        editor_canvas_metrics(app);
        layout_editor(app);
        ++app->input.resize_seq;
      }
      publish(app);
      return 0;
    case WM_DPICHANGED: {
      const auto* suggested = reinterpret_cast<const RECT*>(lparam);
      ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
      if (app->editor.open) {
        layout_editor(app);
        editor_publish_canvas(app);
      }
      return 0;
    }
    case WM_GETMINMAXINFO: {
      auto* mm = reinterpret_cast<MINMAXINFO*>(lparam);
      const int dpi = static_cast<int>(::GetDpiForWindow(hwnd));
      mm->ptMinTrackSize.x = ::MulDiv(kEditorMinWidthDip, dpi, 96);
      mm->ptMinTrackSize.y = ::MulDiv(kEditorMinHeightDip, dpi, 96);
      return 0;
    }
    case WM_DISPLAYCHANGE:
    case WM_MOVE:
      ++app->input.display_change_seq;
      publish(app);
      return 0;
    case WM_ACTIVATE:
      app->editor.active = LOWORD(wparam) != WA_INACTIVE;
      app->input.window_active = app->editor.active || app->main_active;
      publish(app);
      return 0;
    case WM_TIMER:
      if (wparam == kEditorTickTimerId) editor_follow_playback(app);
      return 0;
    case WM_ERASEBKGND:
      return 1;  // the swapchain and the island own every pixel
    case WM_CLOSE:
      editor_request_close(app);
      return 0;
    default: break;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool register_editor_class() noexcept {
  static bool registered = false;
  if (registered) return true;
  const HINSTANCE instance = ::GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = &editor_window_proc;
  wc.hInstance = instance;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;  // the swapchain paints; GDI must not
  const int dpi = static_cast<int>(::GetDpiForSystem());
  (void)::LoadIconWithScaleDown(instance, MAKEINTRESOURCEW(MV_IDI_APP), ::GetSystemMetricsForDpi(SM_CXICON, dpi),
                                ::GetSystemMetricsForDpi(SM_CYICON, dpi), &wc.hIcon);
  (void)::LoadIconWithScaleDown(instance, MAKEINTRESOURCEW(MV_IDI_APP), ::GetSystemMetricsForDpi(SM_CXSMICON, dpi),
                                ::GetSystemMetricsForDpi(SM_CYSMICON, dpi), &wc.hIconSm);
  wc.lpszClassName = kEditorWindowClass;
  registered = ::RegisterClassExW(&wc) != 0;
  return registered;
}

// The window goes once the swapchain has left it (or the render thread has
// stopped); until then it is hidden, and the canvas is back in the viewer.
void editor_release_tick(app_state* app, bool force = false) noexcept {
  auto& ed = app->editor;
  const auto retired = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ed.retired));
  const bool free = force || !ed.retired || app->lab.canvas_window() != retired || app->lab.finished() ||
                    ++ed.release_tries > kEditorReleaseTries;
  if (!free) return;
  if (app->window) ::KillTimer(app->window, kEditorReleaseTimerId);
  if (ed.retired) ::DestroyWindow(ed.retired);
  ed.retired = nullptr;
  ed.release_tries = 0;
}

void set_editor_open(app_state* app, bool open) {
  auto& ed = app->editor;
  if (open == ed.open) {
    if (open && ed.window) ::SetForegroundWindow(ed.window);
    return;
  }
  if (open) {
    if (edit_subject_of(app) != mv::shell::edit_subject::clip || app->motion_playing || !app->window ||
        app->closing) {
      ::MessageBeep(MB_ICONWARNING);
      return;
    }
    const std::string path = current_item_path(app);
    if (path.empty() || !register_editor_class()) {
      ::MessageBeep(MB_ICONWARNING);
      return;
    }
    // A window still waiting on the render thread would be a second target.
    if (ed.retired && app->lab.canvas_window() == static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ed.retired))) {
      ::MessageBeep(MB_ICONWARNING);
      return;
    }
    editor_release_tick(app, true);
    if (app->ws.open) close_workspace(app);
    if (app->trim.armed()) (void)set_trim_mode(app, false);
    const int dpi = static_cast<int>(::GetDpiForWindow(app->window));
    RECT main_rc{};
    ::GetWindowRect(app->window, &main_rc);
    const std::size_t slash = path.find_last_of("\\/");
    const std::wstring title =
        L"Video Editor — " + wide_from_utf8(slash == std::string::npos ? path : path.substr(slash + 1));
    // Owned by the viewer: it stays above it, and minimises and closes with it.
    const HWND hwnd = ::CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kEditorWindowClass, title.c_str(),
                                        WS_OVERLAPPEDWINDOW, main_rc.left + ::MulDiv(40, dpi, 96),
                                        main_rc.top + ::MulDiv(40, dpi, 96), ::MulDiv(kEditorWidthDip, dpi, 96),
                                        ::MulDiv(kEditorHeightDip, dpi, 96), app->window, nullptr,
                                        ::GetModuleHandleW(nullptr), app);
    if (!hwnd) {
      ::MessageBeep(MB_ICONWARNING);
      return;
    }
    ed.window = hwnd;
    if (!app->chrome.attach_editor(hwnd, app->window)) {
      ed.window = nullptr;
      ::DestroyWindow(hwnd);
      ::MessageBeep(MB_ICONWARNING);
      return;
    }
    enable_dark_titlebar(hwnd);
    ed.open = true;
    ed.path = path;
    ed.selected = -1;
    ed.strip.clear();
    ed.peaks.clear();
    ed.timeline.load(0);
    ed.last_seek = -1;
    ed.pushed_playhead = -1;
    ed.fps = 0;
    ed.exported_revision = 0;
    ed.shuttle.stop();
    ed.shuttle_skimmed = false;
    ed.rate_changed = false;
    ed.trim_index = -1;
    ++ed.token;
    // The canvas moves in: the render thread retargets on its next frame.
    app->input.canvas_window = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(hwnd));
    ::ShowWindow(hwnd, SW_SHOWNORMAL);
    ::SetForegroundWindow(hwnd);
    ::SetFocus(hwnd);
    apply_view_state(app);  // the viewer's filmstrip and transport step aside; publishes the preview
    layout_editor(app);
    editor_load_clip(app);
    ::SetTimer(hwnd, kEditorTickTimerId, kEditorTickMs, nullptr);
    push_editor_view(app, true);
    push_edit_view(app);  // the command bar's button reads Done
  } else {
    const HWND hwnd = ed.window;
    editor_set_rate(app, 1.0);  // the viewer gets its clip back at 1x
    ed.timeline.end_trim();
    ed.trim_index = -1;
    ed.open = false;
    ed.active = false;
    ed.minimized = false;
    ++ed.token;
    if (hwnd) ::KillTimer(hwnd, kEditorTickTimerId);
    app->chrome.detach_editor();
    ed.window = nullptr;
    // The canvas goes home; the hidden window waits for it to leave.
    app->input.canvas_window = 0;
    app->input.window_visible = app->window && !::IsIconic(app->window);
    app->input.window_active = app->main_active;
    if (hwnd) {
      ::ShowWindow(hwnd, SW_HIDE);
      ed.retired = hwnd;
      ed.release_tries = 0;
      if (app->window) ::SetTimer(app->window, kEditorReleaseTimerId, kEditorReleaseMs, nullptr);
    }
    if (app->window && !app->closing) {
      ::SetForegroundWindow(app->window);
      focus_canvas(app);
    }
    apply_view_state(app);  // metrics back to the viewer's window, and a resize
    push_edit_view(app);
  }
}

void chrome_on_command(void* ctx, int command, float arg) {
  auto* app = static_cast<app_state*>(ctx);
  // WM_CLOSE pumps messages after detaching; nothing the island queued before
  // it went away may act on the app now.
  if (!app || app->closing) return;
  switch (command) {
    case mv::shell::chrome_cmd_home_colour: {
      const auto rgb = static_cast<std::uint32_t>(arg) & 0xFFFFFFu;
      if (app->input.home_background_rgb != rgb) {
        app->input.home_background_rgb = rgb;
        publish(app);
      }
      return;
    }
    case mv::shell::chrome_cmd_open:
      if (app->window) open_file_dialog(app, app->window);
      return;
    case mv::shell::chrome_cmd_open_folder:
      if (app->window) open_folder_dialog(app, app->window);
      return;
    case mv::shell::chrome_cmd_fit:         ++app->input.fit_seq; break;
    case mv::shell::chrome_cmd_one_to_one:  ++app->input.one_to_one_seq; break;
    case mv::shell::chrome_cmd_zoom_in:     ++app->input.zoom_in_seq; break;
    case mv::shell::chrome_cmd_zoom_out:    ++app->input.zoom_out_seq; break;
    case mv::shell::chrome_cmd_zoom_preset:
      app->input.zoom_preset = arg;
      ++app->input.zoom_preset_seq;
      break;
    case mv::shell::chrome_cmd_overlay:     ++app->input.toggle_overlay_seq; break;
    case mv::shell::chrome_cmd_select_item:
      folder_select(app, static_cast<std::uint32_t>(arg));
      return;
    case mv::shell::chrome_cmd_prev:
      folder_step(app, -1);
      return;
    case mv::shell::chrome_cmd_next:
      folder_step(app, 1);
      return;
    case mv::shell::chrome_cmd_toggle_gallery:
      set_gallery(app, !app->gallery_visible);
      return;
    case mv::shell::chrome_cmd_close_gallery:
      set_gallery(app, false);
      return;
    case mv::shell::chrome_cmd_gallery_activate: {
      std::uint32_t cur = 0;
      (void)mv_folder_selected(app->session, &cur);
      const auto index = static_cast<std::uint32_t>(arg);
      // A click on the tile already selected is "back to it": reselecting would
      // reopen a clip, and the one held under the grid would not resume (#44).
      if (index != cur) {
        folder_select(app, index);
        // Closing the grid would otherwise reveal the previous still until the
        // new decode lands. Drop it when the click is a jump.
        ++app->input.discard_media_seq;
        publish(app);
      }
      set_gallery(app, false);
      return;
    }
    case mv::shell::chrome_cmd_toggle_filmstrip:
      toggle_filmstrip_setting(app);
      return;
    case mv::shell::chrome_cmd_set_settings:
      {
        const std::int32_t keep_sort = app->settings.sort;
        app->settings = mv::shell::view_settings::from_flags(static_cast<std::int32_t>(arg));
        app->settings.sort = keep_sort;
      }
      mv::shell::save_view_settings(app->settings);
      mv::shell::app_settings().set_int(
          "update", "auto_check",
          (static_cast<std::int32_t>(arg) & mv::shell::update::kChromeFlagUpdateAutoCheck) != 0 ? 1 : 0);
      mv::shell::app_settings().set(
          "update", "channel",
          (static_cast<std::int32_t>(arg) & mv::shell::update::kChromeFlagUpdatePreview) != 0 ? "preview" : "stable");
      // Telemetry only ever changes through an explicit answer: the first-run
      // screen, or the Settings row. Both arrive here with the Asked bit set,
      // and a word without it leaves consent exactly as it was (docs/design/13).
      if ((static_cast<std::int32_t>(arg) & mv::shell::telemetry::kChromeFlagTelemetryAsked) != 0) {
        mv::shell::telemetry::set_enabled(
            (static_cast<std::int32_t>(arg) & mv::shell::telemetry::kChromeFlagTelemetry) != 0);
      }
      app->input.sticky_zoom = app->settings.sticky_zoom;
      app->input.background = app->settings.background;
      (void)mv_folder_set_hidden_kinds(app->session, app->settings.hidden_kinds());
      app->chrome.apply_settings(chrome_flags(app), app->settings.sort);
      apply_view_state(app);
      return;
    case mv::shell::chrome_cmd_tree_open: {
      // The island cannot pass a string through the callback; it parks the
      // chosen folder and native pulls it (chrome_host::take_tree_path).
      const std::string dir = app->chrome.take_tree_path();
      if (dir.empty()) return;
      app->mode = open_mode::folder;
      app->gallery_visible = false;
      sync_video_hold(app, false);
      {
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0);
        if (n <= 1) return;
        std::wstring wide(static_cast<std::size_t>(n), 0);
        ::MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, wide.data(), n);
        wide.resize(static_cast<std::size_t>(n) - 1);
        open_folder(app, wide, {});
      }
      if (app->window) focus_canvas(app);
      return;
    }
    case mv::shell::chrome_cmd_set_sort:
      set_sort(app, static_cast<std::int32_t>(arg));
      return;
    case mv::shell::chrome_cmd_rebind: {
      const int packed = static_cast<int>(arg);
      const int row = packed & 0xFF;
      const auto k = static_cast<mv::shell::key>((packed >> 8) & 0xFFF);
      const auto mods = static_cast<std::uint8_t>((packed >> 20) & 7);
      if (mv::shell::rebind_live(row, k, mods)) {
        persist_live_keys();
        app->router.rebuild(mv::shell::live_bindings());
        publish_command_table(app);
      }
      return;
    }
    case mv::shell::chrome_cmd_reset_keys:
      mv::shell::reset_live_bindings();
      persist_live_keys();
      app->router.rebuild(mv::shell::live_bindings());
      publish_command_table(app);
      return;
    case mv::shell::chrome_cmd_set_rate:
      apply_rate(app, rate_index_for(static_cast<double>(arg)));
      return;
    case mv::shell::chrome_cmd_video_active: {
      // 0 no clip, 1 paused or ended, 2 playing (IslandHost.Video.cs).
      const bool on = arg != 0.0f;
      const bool playing = arg >= 2.0f;
      if (app->video_on == on) {
        // Pause, end, or a media key: the controls come back (issue #38).
        if (app->video_playing != playing) {
          app->video_playing = playing;
          apply_transport_autohide(app);
        }
        return;
      }
      app->video_on = on;
      app->video_playing = playing;
      // A freshly opened media_source starts at 1.00x, so the ladder and the
      // dropdown have to start there too rather than inheriting the last clip.
      if (on) {
        apply_rate(app, kRateDefaultIndex);
        // Volume, unlike the rate, is the listener's and carries across clips.
        (void)mv_video_set_volume(app->session, app->volume);
        app->muted = false;
      }
      apply_view_state(app);
      workspace_item_changed(app);  // PR 29: a clip playing is Trim's subject
      return;
    }
    case mv::shell::chrome_cmd_open_path: {
      // Import's Enter: open a card file in the viewer (culling before
      // copying). The path is parked like the tree's (take_tree_path).
      const std::string path = app->chrome.take_tree_path();
      if (path.empty()) return;
      const int n = ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
      if (n <= 1) return;
      std::wstring wide(static_cast<std::size_t>(n), 0);
      ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), n);
      wide.resize(static_cast<std::size_t>(n) - 1);
      open_path(app, wide);
      return;
    }
    case mv::shell::chrome_cmd_drag_items: {
      // A gallery / filmstrip cell is starting a drag (FolderStore.dragFiles on
      // the Mac): a marked cell drags every marked item in listing order, each
      // with its RAW / Live pair. Answered inside this call, before
      // DragStarting returns. An unmarked cell gets no list and drags itself
      // (and its pair, which the island already holds): no pass over the
      // listing for the common case.
      app->own_drag = true;
      std::string lines;
      if (!app->marks.empty()) {
        const std::uint32_t count = folder_count(app);
        const auto index = static_cast<std::uint32_t>(arg < 0.0f ? 0.0f : arg);
        if (index < count && app->marks.contains(item_path_at(app, index))) {
          std::vector<std::string> picks;
          for (std::uint32_t i = 0; i < count; ++i) {
            std::string p = item_path_at(app, i);
            if (!p.empty() && app->marks.contains(p)) picks.push_back(std::move(p));
          }
          for (const std::string& p : expand_pair_targets(app, std::move(picks))) {
            lines += p;
            lines += '\n';
          }
        }
      }
      app->chrome.set_drag_paths(lines, true);
      return;
    }
    case mv::shell::chrome_cmd_drag_ended:
      end_own_drag(app);
      return;
    case mv::shell::chrome_cmd_open_recent:
      open_welcome_row(app, static_cast<int>(arg));
      return;
    case mv::shell::chrome_cmd_addon_state: {
      // The chrome installed, loaded, or removed an add-on: 0 / 1 Import
      // (Milestone G), 2 / 3 the AI pack (Milestone H).
      const int v = static_cast<int>(arg);
      mv::shell::set_addon_commands_available(
          v >= 2 ? mv::shell::addon_family::ai : mv::shell::addon_family::import, (v & 1) != 0);
      // docs/design/25: the rows the loaded add-ons' manifests contribute, from the
      // core (it holds the verified manifests); they supersede the built-in
      // rows of the same add-on. The router re-reads the live table.
      refresh_contributed_commands(app);
    }
      app->chrome.set_command_table(mv::shell::describe_commands());
      return;
    case mv::shell::chrome_cmd_export:
      app->export_choice = static_cast<std::int32_t>(arg);
      start_export(app, mv::shell::unpack_export(app->export_choice));
      return;
    // PR 13 / 14.
    case mv::shell::chrome_cmd_clip_tool:
      run_clip_tool(app, static_cast<std::int32_t>(arg));
      return;
    case mv::shell::chrome_cmd_clip_index:
      trim_index_arrived(app, static_cast<std::uint64_t>(arg));
      return;
    // PR 29 (docs/design/20): the Edit workspace's strip, and the Crop pane's presets
    // (arg = preset, + 16 portrait) and straighten slider (arg = degrees).
    case mv::shell::chrome_cmd_edit_tab:
      edit_select_tab(app, static_cast<int>(arg));
      return;
    case mv::shell::chrome_cmd_edit_action:
      run_edit_action(app, static_cast<int>(arg));
      return;
    // PR 30: the Video Editor's timeline.
    case mv::shell::chrome_cmd_editor_seek:
      editor_seek(app, static_cast<std::int64_t>(static_cast<double>(arg) * 1'000'000.0));
      return;
    case mv::shell::chrome_cmd_editor_action:
      editor_run_action(app, static_cast<int>(arg));
      return;
    case mv::shell::chrome_cmd_editor_trim_grab:
      editor_trim_grab(app, static_cast<int>(arg));
      return;
    case mv::shell::chrome_cmd_editor_trim_to:
      editor_trim_to(app, static_cast<std::int64_t>(static_cast<double>(arg) * 1'000'000.0));
      return;
    case static_cast<int>(mv::shell::command_id::crop_aspect_set):
      edit_set_aspect(app, static_cast<int>(arg));
      return;
    case static_cast<int>(mv::shell::command_id::crop_straighten_set):
      edit_set_straighten(app, arg);
      return;
    // PR 29: the metadata pane's tag editor and Date taken.
    case mv::shell::chrome_cmd_meta_tags:
      set_tags_from_pane(app);
      if (app->window && app->island_focus == mv::shell::focus_kind::text) focus_canvas(app);
      return;
    case mv::shell::chrome_cmd_meta_date:
      set_date_from_pane(app, arg != 0.0f);
      if (app->window && app->island_focus == mv::shell::focus_kind::text) focus_canvas(app);
      return;
    // PR 11: the adjust pane's sliders carry their value; Reset carries none.
    case static_cast<int>(mv::shell::command_id::adjust_exposure):
    case static_cast<int>(mv::shell::command_id::adjust_contrast):
    case static_cast<int>(mv::shell::command_id::adjust_saturation):
    case static_cast<int>(mv::shell::command_id::adjust_temperature):
    case static_cast<int>(mv::shell::command_id::adjust_tint):
      set_adjust_from_pane(app,
                           static_cast<mv::edit::adjust_param>(
                               command - static_cast<int>(mv::shell::command_id::adjust_exposure)),
                           arg);
      return;
    case static_cast<int>(mv::shell::command_id::adjust_reset):
      reset_adjust_from_pane(app);
      return;
    // PR 12: the pane's comment field and Revert button. Return hands the
    // keyboard back to the canvas; leaving the field by mouse already did.
    case mv::shell::chrome_cmd_meta_comment:
      set_comment_from_pane(app);
      if (app->window && app->island_focus == mv::shell::focus_kind::text) focus_canvas(app);
      return;
    case mv::shell::chrome_cmd_meta_revert:
      revert_current_metadata(app);
      return;
    case mv::shell::chrome_cmd_folder_ready: {
      // The island owns the completion drain (docs/design/12 2026-09-07), so this is
      // how the native side learns that a listing landed.
      // Milestone H: a result list, or a directory again. The item that was
      // open before the first list is where Up returns.
      {
        char title[512]{};
        std::uint32_t bytes = 0;
        if (mv_folder_list_title(app->session, title, sizeof(title), &bytes) != MV_OK) title[0] = '\0';
        title[sizeof(title) - 1] = '\0';
        if (title[0] != '\0' && app->list_title.empty()) app->list_return_select = app->edit_path;
        app->list_title = title;
        if (app->list_title.empty()) app->list_return_select.clear();
      }
      refresh_item_info(app);
      refresh_mark_state(app);
      // The watcher fires this for a folder that gained or lost a subfolder too.
      if (app->tree_visible) push_tree_root(app);
      edit_item_opened(app);
      ++app->input.activity_seq;
      publish(app);
      bool revealed = false;
      if (!app->reveal_child.empty()) {
        const std::uint32_t n = subfolder_count(app);
        for (std::uint32_t i = 0; i < n; ++i) {
          if (subfolder_path_at(app, i) != app->reveal_child) continue;
          app->folder_cursor = static_cast<int>(i);
          revealed = true;
          break;
        }
        app->reveal_child.clear();
      }
      // A folder of folders opens the gallery so the tiles are what lands.
      // Coming back up also opens it, on the tile that was left.
      if (!app->gallery_if_empty_dir.empty() && app->trail.current() == app->gallery_if_empty_dir) {
        app->gallery_if_empty_dir.clear();
        if (folder_count(app) == 0 && subfolder_count(app) > 0) {
          set_gallery(app, true);
          if (!revealed) app->folder_cursor = 0;
        }
      }
      if (revealed) set_gallery(app, true);
      refresh_siblings(app);
      push_browse_state(app);
      apply_view_state(app);
      if (g_restore.gallery && gallery_available(app)) {
        g_restore.gallery = false;
        set_gallery(app, true);
      }
      return;
    }
    case mv::shell::chrome_cmd_open_subfolder:
      open_subfolder_at(app, static_cast<std::uint32_t>(arg));
      return;
    case mv::shell::chrome_cmd_open_crumb:
      open_crumb_at(app, static_cast<std::int32_t>(arg));
      return;
    case mv::shell::chrome_cmd_gallery_columns:
      app->gallery_columns = std::max(1, static_cast<int>(arg));
      return;
    case mv::shell::chrome_cmd_focus_changed: {
      const int kind = static_cast<int>(arg);
      if (kind >= static_cast<int>(mv::shell::focus_kind::command_bar) &&
          kind <= static_cast<int>(mv::shell::focus_kind::pane)) {
        app->island_focus = static_cast<mv::shell::focus_kind>(kind);
      }
      // Focus into the transport holds it up (issue #38).
      if (kind == static_cast<int>(mv::shell::focus_kind::transport)) transport_activity(app);
      return;
    }
    case mv::shell::chrome_cmd_transport_hold:
      app->transport_hold = arg != 0.0f;
      // Letting go of a scrub or closing a menu restarts the idle clock.
      app->autohide.activity(::GetTickCount64());
      apply_transport_autohide(app);
      return;
    case mv::shell::chrome_cmd_transport_width:
      // The row grew or shrank (trim armed, a longer clock): refit the bar.
      if (app->chrome.set_transport_content(static_cast<int>(arg + 0.5f))) layout_chrome(app);
      return;
    case mv::shell::chrome_cmd_update_restart: {
      if (arg != 0.0f) {
        // Update.exe is armed and waiting for this pid: leave the ordinary way.
        if (app->window) ::PostMessageW(app->window, WM_CLOSE, 0, 0);
        return;
      }
      // docs/design/13: never restart under a playing clip (no export/trim jobs in v1).
      std::uint32_t state = MV_PLAY_STOPPED;
      if (app->session) (void)mv_video_state(app->session, &state);
      const bool playing = (app->session && mv::abi::video_open(app->session) &&
                            state == MV_PLAY_PLAYING) ||
                           app->motion_playing || clip_jobs_busy(app);
      if (playing) {
        ::MessageBeep(MB_ICONWARNING);
        return;
      }
      mv::shell::update::view_restore view;
      if (app->mode != open_mode::none) {
        const std::string utf8 = current_item_path(app);
        const int n = utf8.empty() ? 0 : ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
        if (n > 1) {
          view.path.assign(static_cast<std::size_t>(n - 1), L'\0');
          ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, view.path.data(), n);
        }
      }
      view.zoom_percent = app->lab.view_fitted() ? 0 : app->lab.status_zoom_percent();
      view.fullscreen = app->fullscreen;
      view.gallery = app->gallery_visible;
      const auto args = mv::shell::update::restart_arguments(view);
      if (!app->chrome.request_update_restart(mv::shell::update::join_arguments(args))) {
        ::MessageBeep(MB_ICONWARNING);
      }
      return;
    }
    case mv::shell::chrome_cmd_popup:
      app->popup_open = arg != 0.0f;
      // Fullscreen parks the bar again once nothing hangs off it. Do not
      // SetFocus here: Closed of a replaced flyout can arrive after the
      // a replacement flyout's field already took keyboard focus.
      if (!app->popup_open && app->fullscreen) layout_chrome(app);
      return;
    default:
      // Island chrome can post a command id (help, open, …) through the same
      // switch as its key.
      if (command > 0 && command < mv::shell::kCommandCount &&
          !mv::shell::is_reserved_notification(command) &&
          !mv::shell::is_retired_command(command)) {
        (void)run_command(app, static_cast<mv::shell::command_id>(command));
      }
      return;
  }
  ++app->input.activity_seq;
  publish(app);
}

// docs/design/16: one router. Symbol keys are resolved through the active layout so
// `?`, `+`, `[` and `\` mean the character, not a US key position. Letters and
// digits keep their virtual key (Windows already maps letters by layout).
// AltGr-only symbols do not resolve; that is a v1.1 remap concern.
mv::shell::key_event translate_key(const MSG& msg, bool is_up) noexcept {
  using mv::shell::key;
  mv::shell::key_event e;
  e.up = is_up;
  e.repeat = !is_up && (msg.lParam & (1 << 30)) != 0;
  std::uint8_t mods = mv::shell::mod_none;
  if (::GetKeyState(VK_CONTROL) & 0x8000) mods |= mv::shell::mod_ctrl;
  if (::GetKeyState(VK_SHIFT) & 0x8000) mods |= mv::shell::mod_shift;
  if (::GetKeyState(VK_MENU) & 0x8000) mods |= mv::shell::mod_alt;

  const auto vk = static_cast<UINT>(msg.wParam);
  key k = key::none;
  switch (vk) {
    case VK_SPACE: k = key::space; break;
    case VK_BACK: k = key::backspace; break;
    case VK_RETURN: k = key::enter; break;
    case VK_ESCAPE: k = key::escape; break;
    case VK_TAB: k = key::tab; break;
    case VK_INSERT: k = key::insert; break;
    case VK_DELETE: k = key::del; break;
    case VK_HOME: k = key::home; break;
    case VK_END: k = key::end; break;
    case VK_PRIOR: k = key::page_up; break;
    case VK_NEXT: k = key::page_down; break;
    case VK_LEFT: k = key::left; break;
    case VK_RIGHT: k = key::right; break;
    case VK_UP: k = key::up; break;
    case VK_DOWN: k = key::down; break;
    case VK_ADD: k = mv::shell::char_key('+'); break;
    case VK_SUBTRACT: k = mv::shell::char_key('-'); break;
    default:
      if (vk >= VK_F1 && vk <= VK_F12) {
        k = static_cast<key>(static_cast<int>(key::f1) + static_cast<int>(vk - VK_F1));
      } else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        k = mv::shell::char_key(static_cast<char>(vk));
      } else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        // PR 12 (docs/design/16 Rate): the keypad's digits are their own keys, apart
        // from the number row's, which stay zoom. With NumLock off Windows
        // sends Insert / End / arrows instead, and those keep their meaning.
        k = static_cast<key>(static_cast<int>(key::numpad0) + static_cast<int>(vk - VK_NUMPAD0));
      } else if (vk >= VK_MULTIPLY && vk <= VK_DIVIDE) {
        k = key::none;  // the keypad's other keys are unbound
      } else {
        BYTE state[256]{};
        if (!::GetKeyboardState(state)) break;
        // The character without Ctrl/Alt, so Ctrl+? still resolves to '?'.
        state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_RCONTROL] = 0;
        state[VK_MENU] = state[VK_LMENU] = state[VK_RMENU] = 0;
        wchar_t chars[4]{};
        const UINT scan = static_cast<UINT>((msg.lParam >> 16) & 0xFF);
        // 0x4: do not change keyboard state (dead keys stay pending for XAML).
        const int n = ::ToUnicodeEx(vk, scan, state, chars, 4, 0x4, ::GetKeyboardLayout(0));
        if (n == 1 && chars[0] > 0x20 && chars[0] < 0x7F) {
          k = mv::shell::char_key(static_cast<char>(chars[0]));
          mods &= static_cast<std::uint8_t>(~mv::shell::mod_shift);  // Shift made the symbol
        }
      }
      break;
  }
  e.k = k;
  e.mods = mods;
  return e;
}

mv::shell::view_state view_state_of(app_state* app) noexcept {
  mv::shell::view_state s;
  // Which island is decided natively from the focus HWND, so it cannot go
  // stale. The wire only says whether the focused XAML element is text.
  const HWND focus = ::GetFocus();
  if (!app->chrome.attached()) {
    s.focus = mv::shell::focus_kind::canvas;
  } else {
    s.focus = app->chrome.classify_focus(focus, app->window);
    // Focus on the canvas proves no text control holds it; drop a stale bit.
    if (s.focus == mv::shell::focus_kind::canvas) {
      app->island_focus = mv::shell::focus_kind::command_bar;
    }
    if (s.focus != mv::shell::focus_kind::canvas &&
        app->island_focus == mv::shell::focus_kind::text) {
      s.focus = mv::shell::focus_kind::text;
    }
  }
  if (video_mode(app)) s.item = mv::shell::item_kind::clip;
  else if (app->lab.animation() != mv::shell::animation_state::none) s.item = mv::shell::item_kind::animation;
  else if (app->mode != open_mode::none) s.item = mv::shell::item_kind::still;
  s.gallery_open = app->gallery_visible;
  s.fullscreen = app->fullscreen;
  s.loupe_held = app->input.loupe;
  s.slideshow = app->show.active();
  s.popup_open = app->popup_open;
  s.settings_open = app->settings_open;
  s.motion_playing = app->motion_playing;
  // A shown pane is a level for Esc to walk out of (docs/design/16: crop, pane, gallery, ...).
  s.pane_open = app->chrome.meta_pane_visible() || app->chrome.folder_tree_visible();
  s.crop = app->edits.crop_active();
  s.trim = app->trim.armed() && s.item == mv::shell::item_kind::clip;
  if (app->chrome.jobs_pane_visible()) s.pane_open = true;
  if (app->ws.open) s.pane_open = true;  // PR 29: Esc closes the Edit workspace
  if (app->mode != open_mode::none) app->game_on = false;  // a file opened over the runner
  s.game = app->game_on;
  // Milestone H: Esc from a result list is the path bar's "Back to folder"
  // (leave_result_list's own condition, so the key is never swallowed).
  s.list_open = !app->list_title.empty() && !app->current_dir.empty();
  return s;
}

// Host-side and cheap: a style change and a SetWindowPos. The swapchain follows
// through the usual WM_SIZE resize, so there is no second present path.
void set_fullscreen(app_state* app, bool on) noexcept {
  if (!app || !app->window || app->fullscreen == on) return;
  const HWND hwnd = app->window;
  if (on) {
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    app->windowed_placement.length = sizeof(WINDOWPLACEMENT);
    if (!::GetWindowPlacement(hwnd, &app->windowed_placement) ||
        !::GetMonitorInfoW(::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
      MV_LOG_WARN("fullscreen: window placement or monitor info unavailable (%lu)",
                  static_cast<unsigned long>(::GetLastError()));
      return;
    }
    app->windowed_style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    // Set first: the WM_SIZE this causes lays the chrome out as hidden.
    app->fullscreen = true;
    app->gallery_visible = false;
    sync_video_hold(app);
    // A hidden island must not keep keyboard focus.
    ::SetFocus(hwnd);
    ::SetWindowLongPtrW(hwnd, GWL_STYLE, app->windowed_style & ~WS_OVERLAPPEDWINDOW);
    const RECT& r = monitor.rcMonitor;
    ::SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                   SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  } else {
    app->fullscreen = false;
    app->fullscreen_reveal = false;
    ::KillTimer(hwnd, kRevealTimerId);
    ::SetWindowLongPtrW(hwnd, GWL_STYLE, app->windowed_style);
    ::SetWindowPlacement(hwnd, &app->windowed_placement);
    ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                       SWP_FRAMECHANGED);
  }
  layout_chrome(app);
  apply_view_state(app);
  // Issue #38: the change of frame is activity, so the controls are up for it.
  transport_activity(app);
}

void publish_slideshow(app_state* app) noexcept {
  app->input.blackout = app->show.active() && app->show.blackout();
  ++app->input.activity_seq;
  publish(app);
}

// F5 (docs/design/16). Fullscreen unless it already is; leaving puts it back.
void start_slideshow(app_state* app) noexcept {
  if (!app || !app->window || app->show.active()) return;
  const std::uint32_t count = folder_count(app);
  if (count == 0) return;
  std::uint32_t selected = 0;
  if (mv_folder_selected(app->session, &selected) != MV_OK) selected = 0;
  app->show.start(count, selected, ::GetTickCount64());
  app->show_last_advance = ::GetTickCount64();
  app->show_entered_fullscreen = !app->fullscreen;
  if (app->show_entered_fullscreen) set_fullscreen(app, true);
  ::SetTimer(app->window, kSlideshowTimerId, kSlideshowTickMs, nullptr);
  publish_slideshow(app);
}

void stop_slideshow(app_state* app) noexcept {
  if (!app || !app->show.active()) return;
  app->show.stop();
  if (app->window) ::KillTimer(app->window, kSlideshowTimerId);
  if (app->show_entered_fullscreen) set_fullscreen(app, false);
  app->show_entered_fullscreen = false;
  publish_slideshow(app);
}

// One tick: advance when the interval is up and a playing clip has ended —
// whichever is later (docs/design/16). A still, or a paused clip, goes on the
// interval. The advance is the ordinary folder_select, so prefetch and the
// generation counter behave exactly as they do for an arrow key.
void slideshow_tick(app_state* app) noexcept {
  if (!app || !app->show.active()) return;
  const ULONGLONG now = ::GetTickCount64();
  // Minimised: nobody is watching, so do not advance (or decode). Restoring
  // waits a full interval. Merely inactive keeps going — a slideshow on a
  // second monitor while you work elsewhere is the point.
  if (app->window && ::IsIconic(app->window)) {
    app->show_last_advance = now;
    return;
  }
  using media = mv::shell::slideshow::media;
  std::uint32_t state = MV_PLAY_STOPPED;
  if (app->session) (void)mv_video_state(app->session, &state);
  const bool clip_open = app->session && mv::abi::video_open(app->session);
  media current = media::none;
  if (clip_open) {
    switch (state) {
      case MV_PLAY_PLAYING: current = media::playing; break;
      case MV_PLAY_PAUSED:  current = media::paused; break;
      case MV_PLAY_ENDED:   current = media::finished; break;
      default:              current = media::opening; break;  // async open, no frame yet
    }
  }
  if (!clip_open) {
    // Review note 33: a finite animation advances once it has played out; one
    // that loops forever goes on the interval (media::none).
    switch (app->lab.animation()) {
      case mv::shell::animation_state::playing:  current = media::playing; break;
      case mv::shell::animation_state::paused:   current = media::paused; break;
      case mv::shell::animation_state::finished: current = media::finished; break;
      case mv::shell::animation_state::playing_forever:
      case mv::shell::animation_state::none:
        break;
    }
  }
  const ULONGLONG elapsed = now - app->show_last_advance;
  // A clip that never finishes opening must not stall the slideshow forever.
  constexpr ULONGLONG kOpenGiveUpMs = 30000;
  if (current == media::opening && elapsed > app->show.interval_ms() + kOpenGiveUpMs) {
    current = media::finished;
  }
  if (!app->show.should_advance(elapsed, mv::shell::slideshow::media_finished(current))) return;
  const std::uint32_t count = folder_count(app);
  std::uint32_t selected = 0;
  if (count == 0 || mv_folder_selected(app->session, &selected) != MV_OK) {
    stop_slideshow(app);
    return;
  }
  app->show.set_count(count, selected);
  const auto next = app->show.next(selected, app->settings.wrap);  // docs/design/16 wrap setting
  app->show_last_advance = now;
  if (!next) {
    stop_slideshow(app);
    return;
  }
  folder_select(app, *next);
}

void browse_trace(const char* msg) noexcept {
  MV_LOG_INFO("browse: %s", msg);
  wchar_t dir[MAX_PATH]{};
  if (::GetTempPathW(MAX_PATH, dir) == 0) return;
  std::wstring path = dir;
  path += L"mv-browse.log";
  FILE* f = nullptr;
  if (::_wfopen_s(&f, path.c_str(), L"ab") != 0 || f == nullptr) return;
  std::fprintf(f, "%s\n", msg);
  std::fclose(f);
}

void browse_json_string(FILE* f, const char* s) noexcept {
  std::fputc('"', f);
  for (const auto* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
    if (*p == '"' || *p == '\\') {
      std::fputc('\\', f);
      std::fputc(static_cast<int>(*p), f);
    } else if (*p < 0x20) {
      std::fprintf(f, "\\u%04x", *p);
    } else {
      std::fputc(static_cast<int>(*p), f);
    }
  }
  std::fputc('"', f);
}

bool browse_select(app_state* app, std::uint32_t index, bool record,
                   browse_kind kind) noexcept {
  char name[200]{};
  std::uint32_t bytes = 0;
  if (mv_folder_item_name(app->session, index, name, sizeof name, &bytes) != MV_OK)
    std::snprintf(name, sizeof name, "#%u", index);
  name[sizeof name - 1] = '\0';
  const std::uint64_t seq = app->lab.mark_navigation();
  if (seq == 0) return false;
  g_browse.seq = seq;
  g_browse.record = record;
  g_browse.pending_index = static_cast<int>(index);
  g_browse.pending_kind = kind;
  std::snprintf(g_browse.pending_name, sizeof g_browse.pending_name, "%s", name);
  g_browse.phase_tick = ::GetTickCount64();
  folder_select(app, index);
  char line[320];
  std::snprintf(line, sizeof line, "select %u %s seq %llu %s", index, name,
                static_cast<unsigned long long>(seq), record ? "record" : "return");
  browse_trace(line);
  return true;
}

void browse_take(app_state* app, bool timed_out) noexcept {
  if (!g_browse.record || g_browse.nrows >= static_cast<int>(std::size(g_browse.rows))) return;
  auto& row = g_browse.rows[g_browse.nrows++];
  std::snprintf(row.name, sizeof row.name, "%s", g_browse.pending_name);
  row.index = g_browse.pending_index;
  row.kind = g_browse.pending_kind;
  row.timed_out = timed_out ? 1 : 0;
  if (!timed_out) {
    const auto sample = app->lab.navigation_sample(g_browse.seq);
    row.cached = sample.cached;
    row.ready_ms = sample.ready_ms;
    row.present_ms = sample.present_ms;
    row.refresh_ms = sample.refresh_ms;
    row.full_ms = sample.full_ms;
  }
  char line[400];
  std::snprintf(line, sizeof line,
                "done %s %s cached %d present %.2f ready %.2f full %.2f timeout %d",
                row.name, browse_kind_name(row.kind), row.cached, row.present_ms, row.ready_ms,
                row.full_ms, row.timed_out);
  browse_trace(line);
}

bool browse_step_finished(app_state* app) noexcept {
  if (app->lab.navigation_done(g_browse.seq)) {
    browse_take(app, false);
    return true;
  }
  if (::GetTickCount64() - g_browse.phase_tick > kBrowseStepTimeoutMs) {
    browse_take(app, true);
    return true;
  }
  return false;
}

void browse_finish(app_state* app, const char* error) noexcept {
  if (g_browse.step == browse_phase::finish) return;
  g_browse.step = browse_phase::finish;
  if (app->window) ::KillTimer(app->window, kBrowseTimerId);
  FILE* f = nullptr;
  if (!g_browse.json_path.empty() &&
      ::_wfopen_s(&f, g_browse.json_path.c_str(), L"wb") == 0 && f != nullptr) {
    std::fprintf(f,
                 "{\n  \"schema\": 1,\n  \"dwell_s\": %.1f,\n  \"count\": %u,\n  \"error\": ",
                 static_cast<double>(kBrowseDwellMs) / 1000.0, g_browse.count);
    if (error) browse_json_string(f, error);
    else std::fputs("null", f);
    if (g_browse.held_done) {
      // Decode jobs over the held run: what the walk submitted, finished and
      // threw away (a hand-off keeps a landed-on prefetch out of `cancelled`).
      std::fprintf(f,
                   ",\n  \"held\": {\"steps\": %d, \"submitted\": %llu, \"completed\": %llu, "
                   "\"cancelled\": %llu}",
                   g_browse.held_steps,
                   static_cast<unsigned long long>(g_browse.held_after.submitted -
                                                   g_browse.held_before.submitted),
                   static_cast<unsigned long long>(g_browse.held_after.completed -
                                                   g_browse.held_before.completed),
                   static_cast<unsigned long long>(g_browse.held_after.cancelled -
                                                   g_browse.held_before.cancelled));
    }
    std::fputs(",\n  \"steps\": [\n", f);
    for (int i = 0; i < g_browse.nrows; ++i) {
      const auto& row = g_browse.rows[i];
      std::fputs("    {\"name\": ", f);
      browse_json_string(f, row.name);
      std::fprintf(f,
                   ", \"index\": %d, \"kind\": \"%s\", \"cached\": %d, \"timed_out\": %d, "
                   "\"ready_ms\": %.3f, \"present_ms\": %.3f, \"refresh_ms\": %.3f, "
                   "\"full_ms\": %.3f}%s\n",
                   row.index, browse_kind_name(row.kind), row.cached, row.timed_out,
                   row.ready_ms, row.present_ms, row.refresh_ms, row.full_ms,
                   i + 1 < g_browse.nrows ? "," : "");
    }
    std::fputs("  ]\n}\n", f);
    std::fclose(f);
  }
  browse_trace(error ? error : "wrote report");
  if (app->window) ::PostMessageW(app->window, WM_CLOSE, 0, 0);
}

void browse_tick(app_state* app) noexcept {
  if (!g_browse.enabled || !app || !app->session) return;
  if (!g_browse.started) {
    g_browse.started = true;
    g_browse.tick0 = ::GetTickCount64();
    g_browse.phase_tick = g_browse.tick0;
    browse_trace("start");
  }
  switch (g_browse.step) {
    case browse_phase::wait_media:
      if (app->lab.showing_still() || app->lab.status_width() > 0) {
        g_browse.count = folder_count(app);
        if (g_browse.count < 2) {
          browse_finish(app, "fewer than 2 items");
          return;
        }
        g_browse.cold_n = 0;
        for (std::uint32_t i = 3; i < g_browse.count && g_browse.cold_n < 6; ++i)
          g_browse.cold_targets[g_browse.cold_n++] = static_cast<int>(i);
        g_browse.warm_left = static_cast<int>(std::min<std::uint32_t>(g_browse.count - 1, 12));
        g_browse.quick_left = static_cast<int>(std::min<std::uint32_t>(g_browse.count - 1, 12));
        g_browse.held_left = static_cast<int>(std::min<std::uint32_t>(g_browse.count - 1, 12));
        g_browse.step = browse_phase::dwell;
        g_browse.after_dwell = g_browse.cold_n > 0 ? browse_phase::cold : browse_phase::warm;
        g_browse.phase_tick = ::GetTickCount64();
        browse_trace("first photo up");
      } else if (::GetTickCount64() - g_browse.tick0 > kBrowseOpenTimeoutMs) {
        browse_finish(app, "no photo appeared");
      }
      return;
    case browse_phase::dwell:
      if (::GetTickCount64() - g_browse.phase_tick < kBrowseDwellMs) return;
      g_browse.step = g_browse.after_dwell;
      browse_tick(app);
      return;
    case browse_phase::cold:
      if (g_browse.cold_i >= g_browse.cold_n) {
        g_browse.step = browse_phase::warm;
        browse_tick(app);
        return;
      }
      if (!browse_select(app, static_cast<std::uint32_t>(g_browse.cold_targets[g_browse.cold_i++]),
                         true, browse_kind::cold)) {
        browse_finish(app, "could not mark a jump");
        return;
      }
      g_browse.step = browse_phase::wait_away;
      return;
    case browse_phase::wait_away:
      if (!browse_step_finished(app)) return;
      g_browse.step = browse_phase::go_home;
      browse_tick(app);
      return;
    case browse_phase::go_home: {
      std::uint32_t selected = 0;
      if (mv_folder_selected(app->session, &selected) == MV_OK && selected == 0) {
        g_browse.step = browse_phase::dwell;
        g_browse.after_dwell = browse_phase::cold;
        g_browse.phase_tick = ::GetTickCount64();
        return;
      }
      if (!browse_select(app, 0, false, browse_kind::warm)) {
        browse_finish(app, "could not return to the first photo");
        return;
      }
      g_browse.step = browse_phase::wait_home;
      return;
    }
    case browse_phase::wait_home:
      if (!app->lab.navigation_done(g_browse.seq) &&
          ::GetTickCount64() - g_browse.phase_tick <= kBrowseStepTimeoutMs) return;
      g_browse.step = browse_phase::dwell;
      g_browse.after_dwell = browse_phase::cold;
      g_browse.phase_tick = ::GetTickCount64();
      return;
    case browse_phase::warm: {
      std::uint32_t selected = 0;
      if (g_browse.warm_left <= 0 || mv_folder_selected(app->session, &selected) != MV_OK ||
          selected + 1 >= g_browse.count) {
        g_browse.step = browse_phase::quick_home;
        browse_tick(app);
        return;
      }
      --g_browse.warm_left;
      if (!browse_select(app, selected + 1, true, browse_kind::warm)) {
        browse_finish(app, "could not mark a step");
        return;
      }
      g_browse.step = browse_phase::wait_warm;
      return;
    }
    case browse_phase::wait_warm:
      if (!browse_step_finished(app)) return;
      if (g_browse.warm_left <= 0) {
        g_browse.step = browse_phase::quick_home;
        browse_tick(app);
        return;
      }
      g_browse.step = browse_phase::dwell;
      g_browse.after_dwell = browse_phase::warm;
      g_browse.phase_tick = ::GetTickCount64();
      return;
    case browse_phase::quick_home:
      if (!browse_select(app, 0, false, browse_kind::warm)) {
        browse_finish(app, "could not return to the first photo");
        return;
      }
      g_browse.step = browse_phase::wait_quick_home;
      return;
    case browse_phase::wait_quick_home:
      if (!app->lab.navigation_done(g_browse.seq) &&
          ::GetTickCount64() - g_browse.phase_tick <= kBrowseStepTimeoutMs) return;
      // One dwell here, so the first quick step leaves from a settled window.
      g_browse.step = browse_phase::dwell;
      g_browse.after_dwell = browse_phase::quick;
      g_browse.phase_tick = ::GetTickCount64();
      return;
    case browse_phase::quick: {
      std::uint32_t selected = 0;
      if (g_browse.quick_left <= 0 || mv_folder_selected(app->session, &selected) != MV_OK ||
          selected + 1 >= g_browse.count) {
        g_browse.step = browse_phase::held_home;
        browse_tick(app);
        return;
      }
      --g_browse.quick_left;
      if (!browse_select(app, selected + 1, true, browse_kind::quick)) {
        browse_finish(app, "could not mark a quick step");
        return;
      }
      g_browse.step = browse_phase::wait_quick;
      return;
    }
    case browse_phase::wait_quick:
      if (!browse_step_finished(app)) return;
      g_browse.step = browse_phase::quick;  // no dwell: the next Right goes now
      browse_tick(app);
      return;
    case browse_phase::held_home:
      if (!browse_select(app, 0, false, browse_kind::warm)) {
        browse_finish(app, "could not return to the first photo");
        return;
      }
      g_browse.step = browse_phase::wait_held_home;
      return;
    case browse_phase::wait_held_home:
      if (!app->lab.navigation_done(g_browse.seq) &&
          ::GetTickCount64() - g_browse.phase_tick <= kBrowseStepTimeoutMs) return;
      g_browse.step = browse_phase::dwell;
      g_browse.after_dwell = browse_phase::held;
      g_browse.phase_tick = ::GetTickCount64();
      return;
    case browse_phase::held: {
      std::uint32_t selected = 0;
      if (mv_folder_selected(app->session, &selected) != MV_OK) {
        browse_finish(app, "could not read the selection");
        return;
      }
      if (g_browse.held_steps == 0) (void)mv_session_job_stats(app->session, &g_browse.held_before);
      const bool last = g_browse.held_left <= 1 || selected + 2 >= g_browse.count;
      ++g_browse.held_steps;
      --g_browse.held_left;
      if (!last) {
        folder_select(app, selected + 1);  // one key repeat; nothing waits on it
        return;
      }
      if (!browse_select(app, selected + 1, true, browse_kind::held)) {
        browse_finish(app, "could not mark the last held step");
        return;
      }
      g_browse.step = browse_phase::wait_held;
      return;
    }
    case browse_phase::wait_held: {
      const bool timed_out = ::GetTickCount64() - g_browse.phase_tick > kBrowseStepTimeoutMs;
      if (!app->lab.navigation_full(g_browse.seq) && !timed_out) return;
      // Then let the pool settle, so work the walk abandoned is counted.
      mv_job_stats now{};
      if (mv_session_job_stats(app->session, &now) == MV_OK && !timed_out &&
          (now.queue_depth != 0 || now.completed + now.cancelled < now.submitted)) {
        return;
      }
      browse_take(app, !app->lab.navigation_full(g_browse.seq));
      g_browse.held_after = now;
      g_browse.held_done = true;
      browse_finish(app, nullptr);
      return;
    }
    case browse_phase::finish:
      return;
  }
}

void persist_live_keys() noexcept {
  std::vector<mv::shell::key_override> out;
  const auto live = mv::shell::live_bindings();
  const auto def = mv::shell::default_bindings();
  const std::size_t n = live.size() < def.size() ? live.size() : def.size();
  for (std::size_t i = 0; i < n; ++i) {
    if (live[i].k == def[i].k && live[i].mods == def[i].mods) continue;
    out.push_back(mv::shell::key_override{static_cast<int>(i),
                                          static_cast<std::uint16_t>(live[i].k), live[i].mods});
  }
  mv::shell::save_key_overrides(out);
}

void publish_command_table(app_state* app) noexcept {
  if (!app || !app->chrome.attached()) return;
  app->chrome.set_command_table(mv::shell::describe_commands());
}

// docs/design/25: the loaded add-ons' contributed rows, from the core's verified
// manifests (mv_addon_commands_json), into the live table; then the router.
void refresh_contributed_commands(app_state* app) noexcept {
  if (!app) return;
  std::vector<mv::shell::addon_command_row> rows;
  std::string json(64 * 1024, '\0');
  uint32_t needed = 0;
  mv_status status = mv_addon_commands_json(json.data(), static_cast<uint32_t>(json.size()), &needed);
  if (status == MV_ERR_INVALID_ARG && needed > json.size()) {
    json.assign(needed, '\0');
    status = mv_addon_commands_json(json.data(), static_cast<uint32_t>(json.size()), &needed);
  }
  if (status == MV_OK && needed > 0) {
    json.resize(needed - 1);
    if (const auto doc = mv::json::parse(json); doc && doc->k == mv::json::kind::array) {
      for (const mv::json::value& c : doc->a) {
        const std::string* addon = c.str("addon");
        const std::string* id = c.str("id");
        const std::string* name = c.str("name");
        if (!addon || !id || !name) continue;
        mv::shell::addon_command_row r;
        r.addon = *addon;
        r.id = *id;
        r.name = *name;
        const std::string* label = c.str("windows");
        if (label && !label->empty() && !mv::shell::parse_key_label(*label, r.k, r.mods)) {
          r.k = mv::shell::key::none;  // listed; Settings may give it a key
          r.mods = 0;
        }
        const std::string* modes = c.str("modes");
        r.modes = mv::shell::parse_modes(modes ? *modes : "viewing");
        const std::string* payload = c.str("payload");
        r.payload = payload ? *payload : "none";
        rows.push_back(std::move(r));
      }
    }
  }
  mv::shell::set_addon_commands(rows);
  app->router.rebuild(mv::shell::live_bindings());
}

void set_settings_open(app_state* app, bool on) noexcept {
  if (!app || app->settings_open == on) {
    if (on && app) app->chrome.show_popup(mv::shell::chrome_popup::settings, 0);
    return;
  }
  app->settings_open = on;
  if (on) {
    mv::shell::command_id released[mv::shell::key_router::kHeldSlots]{};
    const std::size_t n = app->router.cancel_holds(released);
    for (std::size_t i = 0; i < n; ++i) (void)run_command(app, released[i]);
    // Settings covers the canvas too; leaving the grid for it keeps the clip paused.
    app->gallery_visible = false;
    sync_video_hold(app, false);
    app->chrome.show_popup(mv::shell::chrome_popup::close, 0);
    app->popup_open = false;
    // Give XAML its final viewport before measuring and focusing Settings.
    layout_chrome(app);
    app->chrome.show_popup(mv::shell::chrome_popup::settings, 0);
  } else {
    app->chrome.show_popup(mv::shell::chrome_popup::close, 0);
    focus_canvas(app);
  }
  apply_view_state(app);
  layout_chrome(app);
}

void focus_canvas(app_state* app) noexcept {
  if (!app) return;
  if (app->window) ::SetFocus(app->window);
  // Forget the text bit: XAML may not raise GotFocus again when Tab
  // returns to an element that "never lost" focus, and a stale text bit
  // would swallow every key but Esc on a button.
  app->island_focus = mv::shell::focus_kind::command_bar;
}

void walk_back(app_state* app, mv::shell::back_target target) noexcept {
  using mv::shell::back_target;
  switch (target) {
    case back_target::blur_text:
      // File search's field: the first Esc clears it, the second closes it
      // and hands the keyboard to the grid. The island says whether it had it.
      if (app->gallery_visible && app->chrome.gallery_search(mv::shell::gallery_search_action::escape) ==
                                      mv::shell::gallery_search_answer::took) {
        return;
      }
      // PR 12: Esc in the comment field drops the edit. The pane forgets the
      // draft before focus leaves, so leaving it does not commit.
      push_meta_edit(app, true);
      focus_canvas(app);
      return;
    case back_target::canvas_focus:
      focus_canvas(app);
      return;
    case back_target::gallery:
      // Milestone H: the grid over a result list closes like any gallery; the
      // next Esc (result_list) goes back to the folder, as on the Mac.
      set_gallery(app, false);
      return;
    case back_target::result_list:
      (void)leave_result_list(app);
      return;
    case back_target::pane:
      // Esc from the canvas closes what is open: the Edit workspace (PR 29;
      // a crop draft was cancelled one Esc earlier), else the tree first (it
      // is the outermost on the left), then the metadata pane.
      if (app->ws.open) close_workspace(app);
      else if (app->tree_visible) set_folder_tree(app, false);
      else set_meta_pane(app, false);
      return;
    case back_target::popup:
      app->chrome.show_popup(mv::shell::chrome_popup::close, 0);
      // Closing `?` / go-to / find must not leave the island HWND focused, or
      // the next letter waits for an Alt+Tab before it routes again.
      focus_canvas(app);
      return;
    case back_target::settings:
      set_settings_open(app, false);
      return;
    case back_target::motion:
      stop_motion(app);
      return;
    case back_target::slideshow:
      stop_slideshow(app);
      return;
    case back_target::fullscreen:
      set_fullscreen(app, false);
      return;
    case back_target::crop:
      app->edits.cancel_crop();
      publish_edit(app);
      ++app->input.activity_seq;
      publish(app);
      return;
    case back_target::trim:
      (void)set_trim_mode(app, false);
      return;
    case back_target::game:
      app->game_on = false;
      ++app->input.game_exit_seq;
      app->lab.publish(app->input);
      // At game over the render thread sits in an INFINITE idle wait; without
      // a wake the welcome card only returns on the next mouse move.
      app->lab.wake();
      return;
    // Crop lands with its slice (PR 10); resolve_back cannot name it yet.
    default:
      return;
  }
}

void folder_jump(app_state* app, long long delta) {
  const std::uint32_t count = folder_count(app);
  if (count == 0) return;
  std::uint32_t selected = 0;
  if (mv_folder_selected(app->session, &selected) != MV_OK) return;
  long long next = static_cast<long long>(selected) + delta;
  if (next < 0) next = 0;
  if (next >= static_cast<long long>(count)) next = static_cast<long long>(count) - 1;
  if (next == static_cast<long long>(selected)) return;
  folder_select(app, static_cast<std::uint32_t>(next));
}

// docs/design/16 "Status / title": name — i/N — W×H — zoom %. From the folder model
// and the render thread's published numbers; no decode, no I/O. Ratings (★)
// arrive with PR 11.
void update_title(app_state* app) noexcept {
  if (!app || !app->window) return;
  std::wstring title = kWindowTitle;
  try {
    std::uint32_t count = 0;
    std::uint32_t selected = 0;
    if (app->session && mv_folder_count(app->session, &count) == MV_OK && count > 0 &&
        mv_folder_selected(app->session, &selected) == MV_OK && selected < count) {
      char name[260]{};
      std::uint32_t bytes = 0;
      (void)mv_folder_item_name(app->session, selected, name, sizeof(name), &bytes);
      name[sizeof(name) - 1] = '\0';
      const int n = ::MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
      std::wstring wide(n > 1 ? static_cast<std::size_t>(n - 1) : 0, L'\0');
      if (n > 1) ::MultiByteToWideChar(CP_UTF8, 0, name, -1, wide.data(), n);
      wchar_t tail[128]{};
      const std::uint32_t w = app->lab.status_width();
      const std::uint32_t h = app->lab.status_height();
      if (w > 0 && h > 0) {
        (void)::swprintf_s(tail, L" — %u/%u — %u×%u — %u %%", selected + 1,
                           count, w, h, app->lab.status_zoom_percent());
      } else {
        (void)::swprintf_s(tail, L" — %u/%u", selected + 1, count);
      }
      title = wide + tail;
    }
    // PR 12: what a rating key or a metadata write just did, for a few seconds.
    if (!app->notice.empty()) {
      if (::GetTickCount64() < app->notice_until) title += L" \u2014 " + app->notice;
      else app->notice.clear();
    }
  } catch (...) {
    return;
  }
  if (title == app->last_title) return;
  app->last_title = title;
  ::SetWindowTextW(app->window, title.c_str());
}

// A level in the snapshot changed; the render thread redraws once.
bool set_level(app_state* app) noexcept {
  ++app->input.activity_seq;
  publish(app);
  return true;
}

void set_fullscreen_reveal(app_state* app, bool on) noexcept {
  if (!app || !app->window) return;
  if (on) ::SetTimer(app->window, kRevealTimerId, kRevealMs, nullptr);
  else ::KillTimer(app->window, kRevealTimerId);
  if (app->fullscreen_reveal == on) return;
  app->fullscreen_reveal = on;
  apply_view_state(app);
}

// F7 / F8: marks if any, else the current item, to the last destination — or
// the picker when there is none yet or Shift asked for one. The picker is the
// only UI-thread part; the bytes move on the file-job worker.
bool start_transfer(app_state* app, mv::shell::file_job_kind kind, bool pick) {
  if (!app || !app->window) return false;
  auto targets = expand_pair_targets(app, app->marks.targets(current_item_path(app)));
  if (targets.empty()) return false;
  std::string dest;
  if (!pick && !app->destinations.empty()) dest = app->destinations.front();
  if (dest.empty()) {
    std::wstring folder;
    if (!pick_folder(app->window, folder)) return true;  // cancelled: nothing to do
    dest = utf8_from_wide(folder);
    if (dest.empty()) return true;
  }
  // Saved only when it changes. The save is in memory; the settings store's
  // worker writes the file (docs/design/12 "Settings writes on the UI thread", PR 8).
  if (app->destinations.empty() || app->destinations.front() != dest) {
    app->destinations = mv::shell::push_destination(std::move(app->destinations), dest);
    mv::shell::save_destinations(app->destinations);
  }
  if (!app->files.submit(app->window, kind, std::move(targets), dest, app->folder_token)) {
    MV_LOG_WARN("files: could not queue the job");
    ::MessageBeep(MB_ICONWARNING);
  }
  return true;
}

// Delete: always the Recycle Bin, always asked first (docs/design/16). A location
// with no bin is refused on the worker, never deleted permanently.
bool start_recycle(app_state* app) {
  if (!app || !app->window) return false;
  const auto stops = app->marks.targets(current_item_path(app));
  if (stops.empty()) return false;
  // A paired stop goes with both halves, and the question says so (PR 7).
  auto targets = expand_pair_targets(app, stops);
  const auto wide_name = [](const std::string& path) {
    const auto slash = path.find_last_of("\\/");
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    std::wstring wide(name.size(), L'\0');
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()),
                                        wide.data(), static_cast<int>(wide.size()));
    wide.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
    return wide;
  };
  // One item: show its name (never the folder). This is the user's own screen;
  // confirming a delete without seeing what goes is the trap. Several: a count.
  std::wstring text;
  if (stops.size() == 1 && targets.size() == 1) {
    text = L"Move “" + wide_name(targets.front()) + L"” to the Recycle Bin?";
  } else if (stops.size() == 1 && targets.size() == 2) {
    text = L"Move “" + wide_name(targets[0]) + L"” and “" + wide_name(targets[1]) +
           L"” (2 files) to the Recycle Bin?";
  } else {
    wchar_t count[128]{};
    if (targets.size() == stops.size()) {
      (void)::swprintf_s(count, L"Move %zu marked items to the Recycle Bin?", stops.size());
    } else {
      (void)::swprintf_s(count, L"Move %zu marked items (%zu files) to the Recycle Bin?",
                         stops.size(), targets.size());
    }
    text = count;
  }
  if (::MessageBoxW(app->window, text.c_str(), L"Delete", MB_YESNO | MB_ICONQUESTION) != IDYES) {
    return true;
  }
  if (!app->files.submit(app->window, mv::shell::file_job_kind::recycle, std::move(targets), {},
                         app->folder_token)) {
    MV_LOG_WARN("files: could not queue the job");
    ::MessageBeep(MB_ICONWARNING);
  }
  return true;
}

// A copy / move / delete finished. Counts only in logs and dialogs (rule 6).
void on_file_job_done(app_state* app, std::unique_ptr<mv::shell::file_job_result> result) {
  if (!app || !result || app->closing) return;
  // Marks clear only for what succeeded; a failure keeps its mark to retry.
  for (const auto& item : result->items) {
    if (item.status == mv::status::ok && !item.refused) app->marks.erase(item.path);
  }
  refresh_mark_state(app);
  refresh_item_info(app);
  ++app->input.activity_seq;
  publish(app);

  const std::size_t total = result->items.size();
  const std::size_t refused = result->refused();
  const std::size_t failed = result->failed();
  MV_LOG_INFO("files: kind=%u items=%zu ok=%zu failed=%zu refused=%zu",
              static_cast<unsigned>(result->kind), total, result->succeeded(), failed, refused);
  if (refused == 0 && failed == 0) return;

  app->report.total += total;
  app->report.refused += refused;
  app->report.failed += failed;
  app->report.kind = result->kind;
  // A dialog is already up (this completion arrived in its modal loop): it is
  // reported when that one closes, not stacked on top of it.
  if (app->report.showing) return;
  app->report.showing = true;
  while (app->report.refused > 0 || app->report.failed > 0) {
    const auto pending = app->report;
    app->report.total = 0;
    app->report.refused = 0;
    app->report.failed = 0;
    const wchar_t* verb = pending.kind == mv::shell::file_job_kind::copy   ? L"copied"
                          : pending.kind == mv::shell::file_job_kind::move ? L"moved"
                                                                           : L"deleted";
    wchar_t text[320]{};
    if (pending.refused > 0) {
      (void)::swprintf_s(text,
                         L"%zu of %zu items are on a drive without a Recycle Bin and were not "
                         L"deleted.%s",
                         pending.refused, pending.total,
                         pending.failed > 0 ? L" Others could not be deleted either." : L"");
    } else {
      (void)::swprintf_s(text, L"%zu of %zu items could not be %s. They are still marked.",
                         pending.failed, pending.total, verb);
    }
    ::MessageBoxW(app->window, text, L"MediaViewer", MB_OK | MB_ICONWARNING);
    if (app->closing) break;
  }
  app->report.showing = false;
}

// PR 7 `;` (docs/design/16 View, docs/design/04 Live Photos): play the selected Live Photo's
// motion once on the same swapchain, through the PR 5 clip path. The still
// stays on screen until the first video frame; the end of the clip, Esc, `;`
// again or any navigation gives the still back. `false` on a stop that is not
// a Live Photo, so the key does nothing there.
bool start_motion(app_state* app) noexcept {
  if (!app || !app->session || !app->window) return false;
  if (app->motion_playing) {
    stop_motion(app);
    return true;
  }
  std::uint32_t selected = 0;
  if (!selected_index(app, selected)) return false;
  mv_folder_item rec{};
  if (mv_folder_item_at(app->session, selected, &rec) != MV_OK ||
      rec.pair_kind != MV_PAIR_LIVE_PHOTO) {
    return false;
  }
  const std::string motion = item_pair_path_at(app, selected);
  if (motion.empty()) return false;
  // mv_video_open bumps the view generation: in-flight work for the still is
  // done (it is in the LRU), and the clip publishes at the new one.
  uint64_t job = 0;
  if (mv_video_open(app->session, motion.c_str(), &job) != MV_OK) return false;
  app->motion_playing = true;
  app->motion_started = ::GetTickCount64();
  ::SetTimer(app->window, kMotionTimerId, kMotionTickMs, nullptr);
  ++app->input.activity_seq;
  publish(app);
  return true;
}

// Back to the still: re-select the stop. The bump retires the clip on the
// render thread's next tick and the still republishes from the GPU LRU — no
// decode unless it was evicted while the motion played.
void stop_motion(app_state* app) noexcept {
  if (!app || !app->motion_playing) return;
  app->motion_playing = false;
  if (app->window) ::KillTimer(app->window, kMotionTimerId);
  std::uint32_t selected = 0;
  if (selected_index(app, selected)) folder_select(app, selected);
}

void motion_tick(app_state* app) noexcept {
  if (!app || !app->motion_playing) {
    if (app && app->window) ::KillTimer(app->window, kMotionTimerId);
    return;
  }
  std::uint32_t state = MV_PLAY_STOPPED;
  (void)mv_video_state(app->session, &state);
  if (state == MV_PLAY_ENDED) {
    stop_motion(app);
    return;
  }
  const bool opening = state == MV_PLAY_STOPPED;
  if (opening && !mv::abi::video_open(app->session) &&
      ::GetTickCount64() - app->motion_started > kMotionOpenGiveUpMs) {
    MV_LOG_WARN("motion: the Live Photo clip did not open; back to the still");
    stop_motion(app);
  }
}

// ---- PR 15: OS integration (docs/design/10 "OS integration", docs/design/16 View) ---------

// Ctrl+Shift+C: the marked (else current) path(s) as text, one per line. A
// pair gives both halves, as Ctrl+C does.
bool copy_paths_to_clipboard(app_state* app) {
  if (!app) return false;
  const auto targets = expand_pair_targets(app, app->marks.targets(current_item_path(app)));
  const std::string text = mv::shell::paths_as_text(targets, "\r\n");
  if (text.empty()) return false;
  return set_clipboard(app, CF_UNICODETEXT, text_to_global(text));
}

struct flatten_job_result {
  bool ok = false;
  bool for_drag = false;  // Ctrl+Alt+drag: start a file drag of it, not a clipboard copy
  std::string path;
  std::vector<std::uint8_t> png;
};

// Ctrl+Alt+C: the still as the canvas shows it, edits baked, as a PNG. The
// bake is a full-resolution export, so it runs on the pool (rule 1) and lands
// in kMsgFlattenDone. Stills only; a clip's frame is PR 14's frame export.
bool start_flatten(app_state* app, bool for_drag = false) {
  if (!app || !app->window || app->edit_path.empty() || video_mode(app) ||
      app->lab.animation() != mv::shell::animation_state::none) {
    return false;
  }
  const HWND hwnd = app->window;
  app->jobs.submit_at(mv::background_generation,
                      [path = app->edit_path, g = app->edits.export_geometry(),
                       c = app->edits.colour(), hwnd, for_drag](const mv::job_context&) -> mv::status {
                        mv::result<mv::shell::flattened_copy> out = mv::shell::run_flatten(path, g, c);
                        auto* r = new (std::nothrow) flatten_job_result{};
                        if (r) r->for_drag = for_drag;
                        if (r && out) {
                          r->ok = true;
                          r->path = std::move(out->path);
                          r->png = std::move(out->png);
                        }
                        if (r && !::PostMessageW(hwnd, kMsgFlattenDone, 0, reinterpret_cast<LPARAM>(r))) delete r;
                        return out ? mv::status::ok : out.error();
                      });
  return true;
}

// The file (Explorer, Mail, chat) and the PNG itself (Office, Paint, a
// browser) in one clipboard open.
void on_flatten_done(app_state* app, std::unique_ptr<flatten_job_result> r) {
  if (!r) return;
  // Ctrl+Alt+drag (docs/design/09 "drag an edited copy directly into another app"):
  // the bake ran on the pool while the button was held; the drag starts
  // now, of the baked file, if it still is. Let go early and nothing happens.
  // The file is dragged as CF_HDROP rather than a CFSTR_FILECONTENTS stream,
  // which a drop target reads through this thread at drop time: waiting for
  // a full-resolution bake there would block the UI thread (rule 1).
  if (r->for_drag) {
    const int button = ::GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
    if (!r->ok) {
      ::MessageBeep(MB_ICONWARNING);
    } else if (app && app->window && (::GetAsyncKeyState(button) & 0x8000) != 0) {
      begin_file_drag(app, app->window, r->path);
    }
    return;
  }
  static const UINT cf_png = ::RegisterClipboardFormatW(L"PNG");
  if (!r->ok || cf_png == 0 ||
      !set_clipboard(app, {clip_format{CF_HDROP, hdrop_to_global({r->path})},
                           clip_format{cf_png, bytes_to_global(r->png)}})) {
    ::MessageBeep(MB_ICONWARNING);
  }
}

// Ctrl+Shift+S: Windows Share with the marked (else current) file(s). The
// share sheet is WinRT, so the chrome shows it (IslandHost.Share.cs).
bool share_targets(app_state* app) {
  if (!app || !app->window) return false;
  const auto targets = expand_pair_targets(app, app->marks.targets(current_item_path(app)));
  if (targets.empty()) return false;
  mv::json::writer w;
  w.begin_array();
  for (const std::string& t : targets) w.string(t);
  w.end_array();
  return app->chrome.share_files(app->window, w.str());
}

// The command line a jump list entry runs: the folder, quoted. A trailing
// backslash ("D:\") is doubled, or CommandLineToArgvW reads `\"` as a quote.
std::wstring jump_list_arguments(const std::string& utf8_dir) {
  std::wstring dir = wide_from_utf8(utf8_dir);
  if (!dir.empty() && dir.back() == L'\\') dir.push_back(L'\\');
  return L"\"" + dir + L"\"";
}

// The jump list's "Recent folders" (the Dock menu's twin). Worker thread:
// CommitList writes the list into the user's profile. Returns the folders the
// user removed from the list since the last commit: Windows refuses a
// category that adds one back, so the caller drops them from the recents.
//
// Every request takes a number on the UI thread; the pool may run two out of
// order, so one that a newer request has overtaken commits nothing (the newer
// one carries the newer list).
std::atomic<std::uint64_t> g_jump_list_seq{0};

std::vector<std::string> build_jump_list(const std::vector<std::string>& folders, const std::wstring& exe,
                                         std::uint64_t seq) {
  static std::mutex one_at_a_time;
  const std::lock_guard lock(one_at_a_time);
  if (seq != g_jump_list_seq.load(std::memory_order_acquire)) return {};
  std::vector<std::string> pruned;
  const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ICustomDestinationList* list = nullptr;
  IObjectArray* removed = nullptr;
  IObjectCollection* items = nullptr;
  IObjectArray* array = nullptr;
  UINT min_slots = 0;
  if (SUCCEEDED(::CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&list))) &&
      SUCCEEDED(list->SetAppID(kAppUserModelId)) &&
      SUCCEEDED(list->BeginList(&min_slots, IID_PPV_ARGS(&removed))) &&
      SUCCEEDED(::CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&items)))) {
    std::vector<std::wstring> removed_args;
    UINT removed_count = 0;
    if (removed && SUCCEEDED(removed->GetCount(&removed_count))) {
      for (UINT i = 0; i < removed_count; ++i) {
        IShellLinkW* link = nullptr;
        if (FAILED(removed->GetAt(i, IID_PPV_ARGS(&link))) || !link) continue;
        wchar_t args[2 * MAX_PATH]{};
        if (SUCCEEDED(link->GetArguments(args, static_cast<int>(std::size(args))))) {
          removed_args.emplace_back(args);
        }
        link->Release();
      }
    }
    SHSTOCKICONINFO folder_icon{};
    folder_icon.cbSize = sizeof(folder_icon);
    const bool have_icon = SUCCEEDED(::SHGetStockIconInfo(SIID_FOLDER, SHGSI_ICONLOCATION, &folder_icon));
    const std::vector<std::string> labels = mv::shell::recent_folder_labels(folders);
    UINT added = 0;
    for (std::size_t i = 0; !exe.empty() && i < folders.size(); ++i) {
      const std::wstring args = jump_list_arguments(folders[i]);
      if (std::find(removed_args.begin(), removed_args.end(), args) != removed_args.end()) {
        pruned.push_back(folders[i]);
        continue;
      }
      IShellLinkW* link = nullptr;
      if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        continue;
      }
      (void)link->SetPath(exe.c_str());
      (void)link->SetArguments(args.c_str());
      (void)link->SetDescription(wide_from_utf8(folders[i]).c_str());  // the tooltip
      if (have_icon) (void)link->SetIconLocation(folder_icon.szPath, folder_icon.iIcon);
      // The entry's label is its PKEY_Title (FMTID_SummaryInformation, pid 2),
      // spelled out here so neither propkey.h's data symbols nor
      // propvarutil.h's shlwapi-backed helpers join the link.
      static constexpr PROPERTYKEY kTitle = {
          {0xF29F85E0, 0x4FF9, 0x1068, {0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9}}, 2};
      const std::wstring label = wide_from_utf8(labels[i]);
      IPropertyStore* props = nullptr;
      PROPVARIANT title{};
      bool titled = false;
      const std::size_t bytes = (label.size() + 1) * sizeof(wchar_t);
      if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&props)))) {
        title.pwszVal = static_cast<LPWSTR>(::CoTaskMemAlloc(bytes));
        if (title.pwszVal) {
          title.vt = VT_LPWSTR;
          std::memcpy(title.pwszVal, label.c_str(), bytes);
          titled = SUCCEEDED(props->SetValue(kTitle, title)) && SUCCEEDED(props->Commit());
          (void)::PropVariantClear(&title);
        }
      }
      if (props) props->Release();
      if (titled && SUCCEEDED(items->AddObject(link))) ++added;
      link->Release();
    }
    if (added > 0 && SUCCEEDED(items->QueryInterface(IID_PPV_ARGS(&array)))) {
      (void)list->AppendCategory(L"Recent folders", array);
    }
    if (FAILED(list->CommitList())) MV_LOG_WARN("jump list: CommitList failed");
  }
  if (array) array->Release();
  if (items) items->Release();
  if (removed) removed->Release();
  if (list) list->Release();
  if (SUCCEEDED(com)) ::CoUninitialize();
  return pruned;
}

// The exe a jump list entry starts: the install's root stub, which survives
// updates (mediaviewer.iss [Icons] points there for the same reason); a dev
// build has no stub and uses itself.
std::wstring jump_list_exe() {
  if (g_install.installed()) return g_install.root + L"\\MediaViewer.exe";
  wchar_t exe[2 * MAX_PATH]{};
  const DWORD n = ::GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
  return n > 0 && n < std::size(exe) ? std::wstring(exe, n) : std::wstring();
}

void publish_jump_list(app_state* app) {
  if (!app || !app->window) return;
  const HWND hwnd = app->window;
  const std::uint64_t seq = g_jump_list_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
  app->jobs.submit_at(mv::background_generation,
                      [folders = app->recent_folders, exe = jump_list_exe(), hwnd, seq](const mv::job_context&) -> mv::status {
                        std::vector<std::string> pruned = build_jump_list(folders, exe, seq);
                        if (pruned.empty()) return mv::status::ok;
                        auto* r = new (std::nothrow) std::vector<std::string>(std::move(pruned));
                        if (r && !::PostMessageW(hwnd, kMsgJumpListPruned, 0, reinterpret_cast<LPARAM>(r))) delete r;
                        return mv::status::ok;
                      });
}

// A folder opened from Explorer, the jump list, Open, a drop or argv counts;
// walking siblings or the tree does not (open_path's `navigation`).
void note_recent_folder(app_state* app, const std::string& utf8_dir) {
  if (!app || !app->record_recent || utf8_dir.empty()) return;
  std::vector<std::string> next = mv::shell::push_recent_folder(app->recent_folders, utf8_dir);
  if (next == app->recent_folders) return;
  app->recent_folders = std::move(next);
  mv::shell::save_recent_folders(app->recent_folders);
  publish_jump_list(app);
  push_recent_folders(app);
  refresh_welcome_recents(app);
}

// Open > Recent folders: the jump list's labels (recent_folder_labels, as the
// Mac's File > Open Recent), "label\tpath\n" per folder.
void push_recent_folders(app_state* app) {
  if (!app || !app->chrome.attached()) return;
  const std::vector<std::string> labels = mv::shell::recent_folder_labels(app->recent_folders);
  std::string lines;
  for (std::size_t i = 0; i < app->recent_folders.size() && i < labels.size(); ++i) {
    lines += labels[i];
    lines += '\t';
    lines += app->recent_folders[i];
    lines += '\n';
  }
  app->chrome.set_recent_folders(lines);
}

// Drops `dir` from every recent list: settings, jump list, chrome, card.
void forget_recent_folder(app_state* app, const std::string& dir) {
  const std::string gone = dir;  // `dir` may be an element of the list
  std::erase(app->recent_folders, gone);
  mv::shell::save_recent_folders(app->recent_folders);
  publish_jump_list(app);
  push_recent_folders(app);
  refresh_welcome_recents(app);
}

// A click on one of the welcome card's recent folders, or Open > Recent
// folders: the jump list's route.
void open_welcome_row(app_state* app, int row) {
  if (!app || row < 0 || static_cast<std::size_t>(row) >= app->recent_folders.size()) return;
  const std::string dir = app->recent_folders[static_cast<std::size_t>(row)];
  const auto is_dir = mv::io::is_directory(dir);
  if (!is_dir || !is_dir.value()) {
    // The card was ejected or the folder deleted: it is no longer a place to go.
    ::MessageBeep(MB_ICONWARNING);
    forget_recent_folder(app, dir);
    return;
  }
  open_path(app, wide_from_utf8(dir));
  focus_canvas(app);
}

// The x on a welcome card row: the folder leaves the card, the jump list and
// Open > Recent folders. The folder itself is not touched.
void remove_welcome_row(app_state* app, int row) {
  if (!app || row < 0 || static_cast<std::size_t>(row) >= app->recent_folders.size()) return;
  forget_recent_folder(app, app->recent_folders[static_cast<std::size_t>(row)]);
  // The next folder slides up under the pointer: hover it without waiting for a move.
  update_welcome_hover(app);
  ++app->input.activity_seq;
  publish(app);
}

void on_jump_list_pruned(app_state* app, std::unique_ptr<std::vector<std::string>> pruned) {
  if (!app || !pruned) return;
  const std::size_t before = app->recent_folders.size();
  std::erase_if(app->recent_folders, [&](const std::string& f) {
    return std::find(pruned->begin(), pruned->end(), f) != pruned->end();
  });
  if (app->recent_folders.size() != before) {
    mv::shell::save_recent_folders(app->recent_folders);
    push_recent_folders(app);
  }
  refresh_welcome_recents(app);
}

// The taskbar thumbnail toolbar's glyphs, drawn at the small-icon size: white
// on transparent, as the taskbar expects. 0 previous, 1 play, 2 pause, 3 next.
HICON make_thumb_icon(int glyph, int size) noexcept {
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = size;
  bi.bmiHeader.biHeight = -size;  // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC screen = ::GetDC(nullptr);
  HBITMAP colour = ::CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  ::ReleaseDC(nullptr, screen);
  if (!colour || !bits) return nullptr;
  HDC dc = ::CreateCompatibleDC(nullptr);
  HGDIOBJ old_bitmap = ::SelectObject(dc, colour);
  HGDIOBJ old_brush = ::SelectObject(dc, ::GetStockObject(WHITE_BRUSH));
  HGDIOBJ old_pen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
  const int m = size / 4;  // margin
  const int bar = std::max(2, size / 8);
  const int mid = size / 2;
  switch (glyph) {
    case 0: {  // |<
      (void)::Rectangle(dc, m, m, m + bar + 1, size - m + 1);
      const POINT tri[] = {{size - m, m}, {size - m, size - m}, {m + bar, mid}};
      (void)::Polygon(dc, tri, 3);
      break;
    }
    case 1: {  // >
      const POINT tri[] = {{m, m}, {m, size - m}, {size - m, mid}};
      (void)::Polygon(dc, tri, 3);
      break;
    }
    case 2: {  // ||
      const int w = std::max(2, (size - 2 * m) / 3);
      (void)::Rectangle(dc, m, m, m + w + 1, size - m + 1);
      (void)::Rectangle(dc, size - m - w, m, size - m + 1, size - m + 1);
      break;
    }
    default: {  // >|
      const POINT tri[] = {{m, m}, {m, size - m}, {size - m - bar, mid}};
      (void)::Polygon(dc, tri, 3);
      (void)::Rectangle(dc, size - m - bar, m, size - m + 1, size - m + 1);
      break;
    }
  }
  ::GdiFlush();
  ::SelectObject(dc, old_pen);
  ::SelectObject(dc, old_brush);
  ::SelectObject(dc, old_bitmap);
  ::DeleteDC(dc);
  // GDI leaves alpha at 0: every drawn (white) pixel becomes opaque.
  auto* px = static_cast<std::uint32_t*>(bits);
  for (int i = 0; i < size * size; ++i) {
    if (px[i] & 0x00FFFFFFu) px[i] = 0xFFFFFFFFu;
  }
  HBITMAP mask = ::CreateBitmap(size, size, 1, 1, nullptr);
  ICONINFO info{};
  info.fIcon = TRUE;
  info.hbmMask = mask;
  info.hbmColor = colour;
  HICON icon = mask ? ::CreateIconIndirect(&info) : nullptr;
  if (mask) ::DeleteObject(mask);
  ::DeleteObject(colour);
  return icon;
}

// What the toolbar shows, from the clip state the title tick already reads.
void update_thumb_bar(app_state* app) {
  if (!app || !app->taskbar || !app->window) return;
  std::uint32_t state = MV_PLAY_STOPPED;
  if (app->session) (void)mv_video_state(app->session, &state);
  const int want = state == MV_PLAY_STOPPED ? 0 : (state == MV_PLAY_PLAYING ? 2 : 1);
  if (want == app->thumb_state) return;
  THUMBBUTTON play{};
  play.dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
  play.iId = kThumbPlay;
  play.hIcon = app->thumb_icons[want == 2 ? 2 : 1];
  (void)::wcscpy_s(play.szTip, want == 2 ? L"Pause" : L"Play");
  play.dwFlags = want == 0 ? THBF_DISABLED : THBF_ENABLED;
  if (SUCCEEDED(app->taskbar->ThumbBarUpdateButtons(app->window, 1, &play))) app->thumb_state = want;
}

// "TaskbarButtonCreated": Explorer made (or remade, after it restarted) the
// button, so the toolbar is added now.
void on_taskbar_button_created(app_state* app) {
  if (!app || !app->window) return;
  if (!app->taskbar) {
    if (FAILED(::CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&app->taskbar))) ||
        FAILED(app->taskbar->HrInit())) {
      if (app->taskbar) app->taskbar->Release();
      app->taskbar = nullptr;
      return;
    }
  }
  const UINT dpi = ::GetDpiForWindow(app->window);
  const int size = ::GetSystemMetricsForDpi(SM_CXSMICON, dpi ? dpi : 96);
  for (int i = 0; i < 4; ++i) {
    if (!app->thumb_icons[i]) app->thumb_icons[i] = make_thumb_icon(i, size);
  }
  THUMBBUTTON buttons[3]{};
  const UINT ids[] = {kThumbPrev, kThumbPlay, kThumbNext};
  const HICON icons[] = {app->thumb_icons[0], app->thumb_icons[1], app->thumb_icons[3]};
  const wchar_t* tips[] = {L"Previous", L"Play", L"Next"};
  for (int i = 0; i < 3; ++i) {
    buttons[i].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    buttons[i].iId = ids[i];
    buttons[i].hIcon = icons[i];
    (void)::wcscpy_s(buttons[i].szTip, tips[i]);
    buttons[i].dwFlags = i == 1 ? THBF_DISABLED : THBF_ENABLED;
  }
  app->thumb_state = -1;
  if (SUCCEEDED(app->taskbar->ThumbBarAddButtons(app->window, 3, buttons))) update_thumb_bar(app);
}

void release_taskbar(app_state* app) noexcept {
  if (!app) return;
  if (app->taskbar) app->taskbar->Release();
  app->taskbar = nullptr;
  for (HICON& icon : app->thumb_icons) {
    if (icon) ::DestroyIcon(icon);
    icon = nullptr;
  }
}

// Command effects. A switch over a dense enum is the jump table docs/design/16 asks
// for. Returning false means "not applicable here" and sends the key on to the
// island — Q/E on a still, or a command whose slice has not landed yet.
bool run_command(app_state* app, mv::shell::command_id command) noexcept {
  using enum mv::shell::command_id;
  const auto bump = [app](std::uint32_t& seq) {
    ++seq;
    ++app->input.activity_seq;
    publish(app);
    return true;
  };
  // PR 30 (docs/design/21, owner): video is edited in its own window, not a pane.
  if (command == edit_workspace &&
      (app->editor.open || edit_subject_of(app) == mv::shell::edit_subject::clip)) {
    if (app->editor.open) {
      editor_request_close(app);
    } else {
      set_editor_open(app, true);
    }
    return true;
  }
  // PR 29 (docs/design/20): the keys that open a tab of the Edit workspace. The
  // workspace decides the tab; crop_mode and trim_mode then do their own work.
  if (command == edit_workspace || command == crop_mode || command == adjust_pane ||
      command == trim_mode || command == metadata_pane || command == jobs_pane) {
    const mv::shell::workspace_step step = mv::shell::route_workspace(app->ws, edit_subject_of(app), command);
    if (step.action != mv::shell::workspace_action::none) {
      if (mv::shell::apply_step(app->ws, step)) sync_workspace(app);
      if (command != crop_mode && command != trim_mode) return true;
    } else if (command == edit_workspace) {
      ::MessageBeep(MB_ICONWARNING);  // nothing on the canvas to edit
      return true;
    } else if (app->ws.open && (command == metadata_pane || command == jobs_pane || command == adjust_pane)) {
      // A pane the workspace does not hold here takes the edge on its own.
      close_workspace(app);
    }
  }
  switch (command) {
    case open:
      if (app->window) open_file_dialog(app, app->window);
      return true;
    case open_folder:
      if (app->window) open_folder_dialog(app, app->window);
      return true;
    case reveal_in_explorer:
      reveal_current_in_explorer(app);
      return true;
    case open_settings:
      set_settings_open(app, !app->settings_open);
      return true;
    // Milestone G: Import's commands exist only while it is installed; with
    // it absent the key falls through as if unbound (docs/design/18).
    // docs/design/25: a row an add-on's manifest contributed. The payload it asked
    // for rides along as JSON; the add-on's chrome does the work.
    case addon_cmd_0: case addon_cmd_1: case addon_cmd_2: case addon_cmd_3:
    case addon_cmd_4: case addon_cmd_5: case addon_cmd_6: case addon_cmd_7: {
      const mv::shell::addon_command_row* row = mv::shell::addon_command(command);
      if (!row) return false;
      mv::json::writer w;
      if (row->payload == "marks") {
        w.begin_array();
        if (!app->marks.empty()) {
          for (const std::string& t : expand_pair_targets(app, app->marks.targets({}))) w.string(t);
        }
        w.end_array();
      } else if (row->payload == "marked_or_current") {
        const auto targets = expand_pair_targets(app, app->marks.targets(current_item_path(app)));
        if (targets.empty()) return false;
        w.begin_array();
        for (const std::string& t : targets) w.string(t);
        w.end_array();
      } else if (row->payload == "screen") {
        w.begin_object();
        w.key("path").string(current_item_path(app));
        w.key("video").boolean(video_mode(app));
        w.end_object();
      } else {
        w.null();
      }
      return app->chrome.run_addon_command(row->addon, row->id, w.str());
    }
    case open_import: {
      if (!mv::shell::addon_commands_available()) return false;
      // The viewer's marks ride along for the window's "Marked in viewer"
      // selection (docs/design/18 "Selection"). None marked: an empty list.
      mv::json::writer w;
      w.begin_array();
      if (!app->marks.empty()) {
        for (const std::string& t : expand_pair_targets(app, app->marks.targets({}))) w.string(t);
      }
      w.end_array();
      app->chrome.show_import(0, w.str());
      return true;
    }
    case import_now: {
      if (!mv::shell::addon_commands_available()) return false;
      const auto targets =
          expand_pair_targets(app, app->marks.targets(current_item_path(app)));
      if (targets.empty()) return false;
      mv::json::writer w;
      w.begin_array();
      for (const std::string& t : targets) w.string(t);
      w.end_array();
      app->chrome.show_import(1, w.str());
      return true;
    }
    // Milestone H (docs/design/17 "UI and commands"): the AI pack's commands exist
    // only while it is loaded; its chrome does the work. What is on screen
    // rides along (the chrome can also read it from the session).
    case search_open:
    case search_similar:
    case search_next_match:
    case search_prev_match: {
      if (!mv::shell::addon_command_available(command)) {
        // Ctrl+F without the pack (docs/design/16 "File search", 2026-09-28): the
        // island opens Local search's panel once a starting pack attaches,
        // or file search, which may need the gallery shown first.
        if (command != search_open) return false;
        using mv::shell::gallery_search_answer;
        const gallery_search_answer answer = app->chrome.gallery_search(mv::shell::gallery_search_action::open);
        if (answer == gallery_search_answer::needs_gallery) {
          set_gallery(app, true);
          return app->gallery_visible;
        }
        return answer == gallery_search_answer::took;
      }
      const std::int32_t kind = command == search_open        ? 0
                                : command == search_similar   ? 1
                                : command == search_next_match ? 2
                                                               : 3;
      const bool video = video_mode(app);
      std::uint32_t state = MV_PLAY_STOPPED;
      if (video) (void)mv_video_state(app->session, &state);
      mv::json::writer w;
      w.begin_object();
      w.key("path").string(current_item_path(app));
      w.key("video").boolean(video);
      w.key("position_ms").integer(video ? clip_position(app) / 1000000 : -1);
      w.key("paused").boolean(state != MV_PLAY_PLAYING);
      w.end_object();
      // N with no further match (or no pack to ask) falls through as unbound.
      return app->chrome.show_addon(static_cast<std::int32_t>(mv::shell::addon_family::ai), kind,
                                    w.str());
    }
    case close_window:
      if (app->window) ::PostMessageW(app->window, WM_CLOSE, 0, 0);
      return true;
    case prev:
      if (folder_cursor_step(app, -1)) return true;
      // File search: with a name filter on, Left walks its matches.
      if (app->gallery_visible && app->chrome.gallery_search(mv::shell::gallery_search_action::step, -1) ==
                                      mv::shell::gallery_search_answer::took) {
        return true;
      }
      folder_step(app, -1);
      return true;
    case next:
      if (folder_cursor_step(app, 1)) return true;
      if (app->gallery_visible && app->chrome.gallery_search(mv::shell::gallery_search_action::step, 1) ==
                                      mv::shell::gallery_search_answer::took) {
        return true;
      }
      // Nothing open: Space starts the empty-window runner (dino_game.h).
      if (folder_count(app) == 0 && app->mode == open_mode::none && !video_mode(app)) {
        app->game_on = true;
        return bump(app->input.toggle_animation_seq);
      }
      folder_step(app, 1);
      return true;
    case first: folder_jump(app, -(1LL << 32)); return true;
    case last: folder_jump(app, 1LL << 32); return true;
    case skip_back: folder_jump(app, -10); return true;
    case skip_forward: folder_jump(app, 10); return true;
    case next_page: return turn_page(app, 1);
    case prev_page: return turn_page(app, -1);
    case toggle_gallery: set_gallery(app, !app->gallery_visible); return true;
    case gallery_open_selected:
      if (!app->gallery_visible) return false;
      if (app->folder_cursor >= 0) {
        open_subfolder_at(app, static_cast<std::uint32_t>(app->folder_cursor));
        return true;
      }
      // Enter returns to the normal viewer, including the configured filmstrip.
      // It also leaves any slideshow that was running behind the gallery.
      stop_slideshow(app);
      set_gallery(app, false);
      set_fullscreen(app, false);
      if (app->window) ::SetFocus(app->window);
      return true;
    case gallery_up:
    case gallery_down: {
      if (!app->gallery_visible) return false;
      gallery_move_rows(app, command == gallery_up ? -1 : 1);
      return true;
    }
    case gallery_larger:
    case gallery_smaller: {
      if (!app->gallery_visible) return false;
      std::uint32_t selected = 0;
      (void)mv_folder_selected(app->session, &selected);
      app->chrome.scale_gallery(command == gallery_larger ? 1 : -1,
                                static_cast<std::int32_t>(selected));
      return true;
    }
    case toggle_filmstrip: toggle_filmstrip_setting(app); return true;
    case fit:
      app->zoom_intent_tick = 0;
      return bump(app->input.fit_seq);
    case one_to_one:
      app->zoom_intent_tick = ::GetTickCount64();
      return bump(app->input.one_to_one_seq);
    case zoom_in:
      app->zoom_intent_tick = ::GetTickCount64();
      return bump(app->input.zoom_in_seq);
    case zoom_out: return bump(app->input.zoom_out_seq);
    case zoom_200:
    case zoom_400:
      app->input.zoom_preset = command == zoom_200 ? 2.0f : 4.0f;
      app->zoom_intent_tick = ::GetTickCount64();
      return bump(app->input.zoom_preset_seq);
    case game_toggle_3d:
      if (app->mode != open_mode::none || !app->game_on) return false;
      return bump(app->input.game_view_seq);
    case overlay: return bump(app->input.toggle_overlay_seq);
    case reset_stats: return bump(app->input.reset_stats_seq);

    case play_pause: {
      // docs/design/16: on an animation Space plays and pauses it, like a clip.
      if (app->lab.animation() != mv::shell::animation_state::none) {
        ++app->input.anim_toggle_seq;
        return set_level(app);
      }
      std::uint32_t state = MV_PLAY_STOPPED;
      (void)mv_video_state(app->session, &state);
      if (state == MV_PLAY_PLAYING) (void)mv_video_pause(app->session);
      else if (state != MV_PLAY_STOPPED) (void)mv_video_play(app->session);
      return true;
    }
    case pause: (void)mv_video_pause(app->session); return true;
    case mute:
      if (!video_mode(app)) return false;
      app->muted = !app->muted;
      (void)mv_video_set_muted(app->session, app->muted ? 1 : 0);
      return true;
    // docs/design/16: J / L are -10 s / +10 s, and a jump is not part of a skim burst.
    case jump_back:
    case jump_forward:
      app->skim_tick_ms = 0;
      (void)skim(app, (command == jump_forward ? 1 : -1) * kTransportStepNs, true);
      return true;
    case frame_back:
    case frame_forward:
      if (app->lab.animation() != mv::shell::animation_state::none) {
        app->input.anim_steps += command == frame_forward ? 1 : -1;
        return set_level(app);
      }
      (void)mv_video_step(app->session, command == frame_forward ? 1 : -1);
      return true;
    // Speed ladder: the command-bar dropdown is the owner. Q/E used to step
    // it on tap; they skip instead (docs/design/12 2026-09-13).
    case rate_down:
    case rate_up:
      if (!video_mode(app)) return false;
      apply_rate(app, app->rate_index + (command == rate_up ? 1 : -1));
      return true;
    // Q/E: tap is one exact ±2 s skip; hold shuttles on the non-exact seek
    // (nearest keyframe) so a held key cannot queue a decode-forward per
    // repeat (docs/design/16 speed rule 1). A new burst re-reads the position.
    case skim_back:
    case skim_forward: {
      if (!video_mode(app)) return false;
      const std::uint64_t now = ::GetTickCount64();
      const bool new_burst = !app->skim_shuttled || app->skim_tick_ms == 0 ||
                             now - app->skim_tick_ms > kSkimBurstMs;
      if (new_burst) app->skim_tick_ms = 0;
      app->skim_shuttled = true;
      (void)skim(app, (command == skim_forward ? 1 : -1) * kSkimStepNs, new_burst);
      return true;
    }
    // Release settles on the exact frame, the way letting go of the scrubber
    // does — otherwise it stops on whatever keyframe the last cheap seek hit.
    case skim_settle:
      if (!video_mode(app) || !app->skim_shuttled) return false;
      (void)mv_video_seek(app->session, app->skim_target_ns, 1);
      app->skim_shuttled = false;
      app->skim_tick_ms = 0;
      return true;

    case fullscreen: set_fullscreen(app, !app->fullscreen); return true;
    case fill:
      app->zoom_intent_tick = ::GetTickCount64();
      return bump(app->input.fill_seq);
    // docs/design/16: Ctrl+0 resets pan/zoom, which is the opening view — fit.
    case reset_view:
      app->zoom_intent_tick = 0;
      return bump(app->input.fit_seq);
    // docs/design/16: pan only when zoomed. At fit the view is locked, so the key is
    // not ours and falls through to whatever else wants it.
    case pan_up:
    case pan_down:
    case pan_left:
    case pan_right: {
      const bool zooming = app->zoom_intent_tick != 0 &&
                           ::GetTickCount64() - app->zoom_intent_tick < kZoomIntentMs;
      if (app->lab.view_fitted() && !zooming) {
        // docs/design/16 §Focus: fullscreen hides the strips, and ↓ at fit is how a
        // keyboard user gets them (and a clip's transport) back.
        if (command == pan_down && app->fullscreen) {
          set_fullscreen_reveal(app, true);
          return true;
        }
        // A fitted clip has nothing to pan: ↑ ↓ are its volume.
        if ((command == pan_up || command == pan_down) && video_mode(app)) {
          app->volume = std::clamp(app->volume + (command == pan_up ? 0.1f : -0.1f), 0.0f, 1.0f);
          (void)mv_video_set_volume(app->session, app->volume);
          return true;
        }
        return false;
      }
    }
      if (command == pan_up) --app->input.pan_steps_y;
      if (command == pan_down) ++app->input.pan_steps_y;
      if (command == pan_left) --app->input.pan_steps_x;
      if (command == pan_right) ++app->input.pan_steps_x;
      ++app->input.activity_seq;
      publish(app);
      return true;

    // View state the render thread draws from (levels, not edges).
    case cycle_background:
      app->input.background = static_cast<std::uint8_t>((app->input.background + 1) % 5);
      return set_level(app);
    case sticky_zoom:
      app->input.sticky_zoom = !app->input.sticky_zoom;
      return set_level(app);
    case clipping:
      app->input.clipping = !app->input.clipping;
      return set_level(app);
    case loupe:
    case loupe_release:
      // The loupe magnifies a still. On a clip or an empty canvas Z is not
      // ours: do not swallow it and then draw nothing.
      if (command == loupe && !app->lab.showing_still()) return false;
      app->input.loupe = command == loupe;
      if (command == loupe) {
        // Each hold starts at the cursor, or the canvas centre with none.
        app->input.loupe_steps_x = 0;
        app->input.loupe_steps_y = 0;
      }
      return set_level(app);
    case loupe_nudge_left: --app->input.loupe_steps_x; return set_level(app);
    case loupe_nudge_right: ++app->input.loupe_steps_x; return set_level(app);
    case loupe_nudge_up: --app->input.loupe_steps_y; return set_level(app);
    case loupe_nudge_down: ++app->input.loupe_steps_y; return set_level(app);
    case hold_previous:
    case hold_previous_release:
      app->input.hold_previous = command == hold_previous;
      return set_level(app);
    case info_overlay:
      app->input.info_overlay = !app->input.info_overlay;
      refresh_item_info(app);
      request_metadata_now(app);
      return set_level(app);
    // PR 9: both read the record the store already holds; toggling them never
    // reads the file (the verify line).
    case af_points:
      app->input.af_points = !app->input.af_points;
      request_metadata_now(app);
      return set_level(app);
    case eyedropper:
      app->input.eyedropper = !app->input.eyedropper;
      return set_level(app);
    case copy_clipboard:
      return copy_to_clipboard(app);
    // PR 15 (docs/design/16 View): the keyboard twins of drag-out.
    case copy_path:
      return copy_paths_to_clipboard(app);
    case copy_flattened:
      return start_flatten(app);
    case share:
      return share_targets(app);
    // PR 10 geometry, crop mode and export (docs/design/16 View + Crop).
    case rotate_ccw: case rotate_cw: case flip_horizontal: case flip_vertical: case crop_mode:
    case crop_commit: case crop_move_left: case crop_move_right: case crop_move_up:
    case crop_move_down: case crop_narrower: case crop_wider: case crop_shorter:
    case crop_taller: case straighten_ccw: case straighten_cw: case export_image:
    case undo_edit: case reset_edits:
    case crop_aspect_cycle: case crop_aspect_swap:  // PR 29: A / X in crop
      return run_edit_command(app, command);
    // PR 29: Y held shows the original pixels; the stack is untouched.
    case show_original:
    case show_original_release:
      if (edit_subject_of(app) != mv::shell::edit_subject::still || !app->edits.has_item()) return false;
      set_show_original(app, command == show_original);
      return true;
    case crop_aspect_set: case crop_straighten_set:
      return false;  // island-only: they carry a value (chrome_on_command)
    // Marks (docs/design/16): a set separate from the selection, keyed by path.
    case toggle_mark: {
      const std::string current = current_item_path(app);
      if (current.empty()) return false;
      (void)app->marks.toggle(current);
      refresh_mark_state(app);
      return set_level(app);
    }
    case mark_all: {
      const std::uint32_t count = folder_count(app);
      if (count == 0) return false;
      std::vector<std::string> paths;
      paths.reserve(count);
      for (std::uint32_t i = 0; i < count; ++i) paths.push_back(item_path_at(app, i));
      app->marks.mark_all(paths);
      refresh_mark_state(app);
      return set_level(app);
    }
    case unmark_all:
      app->marks.clear();
      refresh_mark_state(app);
      return set_level(app);
    case copy_to:
    case copy_to_pick:
      return start_transfer(app, mv::shell::file_job_kind::copy, command == copy_to_pick);
    case move_to:
    case move_to_pick:
      return start_transfer(app, mv::shell::file_job_kind::move, command == move_to_pick);
    case delete_to_recycle_bin:
      return start_recycle(app);

    // Slideshow (docs/design/16): a mode, no transition pass.
    case slideshow_start:
      start_slideshow(app);
      return app->show.active();
    case slideshow_pause:
      app->show.toggle_pause();
      app->show_last_advance = ::GetTickCount64();  // resuming waits a full interval
      publish_slideshow(app);
      return true;
    case slideshow_faster:
      app->show.faster();
      publish_slideshow(app);
      return true;
    case slideshow_slower:
      app->show.slower();
      publish_slideshow(app);
      return true;
    case blackout:
      app->show.toggle_blackout();
      publish_slideshow(app);
      return true;
    case shuffle: {
      std::uint32_t selected = 0;
      (void)mv_folder_selected(app->session, &selected);
      app->show.toggle_shuffle(selected, ::GetTickCount64());
      return true;
    }

    // docs/design/16 `?`, Ctrl+G go-to and `/` find: XAML flyouts on the command bar.
    case help:
    case go_to:
    case typeahead: {
      const bool on_folders = app->gallery_visible && subfolder_count(app) > 0 &&
                              (app->folder_cursor >= 0 || folder_count(app) == 0);
      if (on_folders) {
        app->folder_query.clear();
        touch_folder_find(app);
        if (app->folder_cursor < 0) app->folder_cursor = 0;
        push_browse_state(app);
        return true;
      }
      if (!app->chrome.attached()) return false;
      // `?` toggles. ShowPopup closes then reopens, which flickered as "it
      // does not open".
      if (command == help && app->popup_open) {
        app->chrome.show_popup(mv::shell::chrome_popup::close, 0);
        app->popup_open = false;
        if (app->fullscreen) layout_chrome(app);
        return true;
      }
      const mv::shell::chrome_popup kind = command == help  ? mv::shell::chrome_popup::help
                                           : command == go_to ? mv::shell::chrome_popup::go_to
                                                              : mv::shell::chrome_popup::find;
      // `?` lists the bindings of the mode underneath (as if the canvas had
      // focus), not of the island it was opened from.
      mv::shell::view_state underneath = view_state_of(app);
      underneath.focus = mv::shell::focus_kind::canvas;
      underneath.popup_open = false;
      const mv::shell::mode under_mode = mv::shell::resolve_mode(underneath);
      auto modes = static_cast<std::int32_t>(mv::shell::mask_of(under_mode));
      // Trim layers over video (key_router.cpp), so `?` lists both.
      if (under_mode == mv::shell::mode::trim) modes |= mv::shell::kVideo;
      app->popup_open = true;
      if (app->fullscreen) layout_chrome(app);  // the flyouts hang off the bar
      app->chrome.show_popup(kind, modes);
      // Do not SetFocus the canvas here: a WinUI Flyout light-dismisses, which
      // is why `?` opened and immediately vanished. Letter keys still route
      // while it is up (command-bar focus is not island mode).
      return true;
    }
    // docs/design/12 2026-09-13: the tree island lands in PR 8. Its command, key and
    // chrome_left_px layout are here so the island maths is not retrofitted.
    case folder_tree:
      set_folder_tree(app, !app->tree_visible);
      return true;
    case folder_up:
      return navigate_folder_up(app);
    case folder_prev:
      return navigate_sibling(app, -1);
    case folder_next:
      return navigate_sibling(app, 1);
    case metadata_pane:
      if (!app->meta_pane_visible && app->adjust.visible()) set_adjust_pane(app, false);
      set_meta_pane(app, !app->meta_pane_visible);
      return true;
    // PR 11 (docs/design/16 Pane): Shift+A shows the adjust pane and focuses its
    // first slider; again (or its close button) hides it.
    case adjust_pane:
      set_adjust_pane(app, !app->adjust.visible());
      return true;
    case adjust_exposure: case adjust_contrast: case adjust_saturation:
    case adjust_temperature: case adjust_tint: case adjust_reset:
      return false;  // island-only: they carry a value (chrome_on_command)
    // PR 12 (docs/design/16 Rate): 0-5 write the rating of the item on screen. The
    // pane's stars post the same ids.
    case set_rating_0: case set_rating_1: case set_rating_2: case set_rating_3:
    case set_rating_4: case set_rating_5:
      return rate_current_item(app, mv::shell::rating_of_command(command));
    case edit_comment:
      return focus_comment_field(app);
    // PR 13 / 14 (docs/design/08, docs/design/16 "Video and trim").
    case trim_mode: case trim_in: case trim_out: case trim_clear: case trim_preview:
    case trim_keyframe: case trim_reencode: case trim_remove_middle: case keyframe_prev:
    case keyframe_next: case jobs_pane: case clip_tools: case clip_split:
      return run_clip_command(app, command);

    // Host-side and cheap (docs/design/16): photographers park the viewer on a
    // second monitor.
    case always_on_top:
      if (!app->window) return false;
      app->topmost = !app->topmost;
      ::SetWindowPos(app->window, app->topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
      return true;

    // PR 7 pairs.
    case play_motion:
      return start_motion(app);
    // docs/design/04: the other half of a RAW+JPEG stop is never trapped. Open RAW
    // shows the RAW file on the canvas (same stop, same marks); Open JPEG goes
    // back to the primary, from the LRU. Not a RAW+JPEG stop: not ours.
    case open_raw:
    case open_jpeg: {
      std::uint32_t selected = 0;
      if (!selected_index(app, selected)) return false;
      mv_folder_item rec{};
      if (mv_folder_item_at(app->session, selected, &rec) != MV_OK ||
          rec.pair_kind != MV_PAIR_RAW_JPEG) {
        return false;
      }
      if (command == open_jpeg) {
        folder_select(app, selected);
        return true;
      }
      const std::string raw = item_pair_path_at(app, selected);
      if (raw.empty()) return false;
      uint64_t job = 0;
      (void)mv_image_open(app->session, raw.c_str(), &job);  // bumps the generation
      ++app->input.activity_seq;
      publish(app);
      return true;
    }

    default:
      return false;
  }
}

// Runs before the island's pre-translate, so F3 and friends work with the
// command bar focused. The router decides what a focused island keeps.
bool handle_folder_find(app_state* app, const mv::shell::key_event& e, bool is_down) {
  if (!folder_find_live(app)) return false;
  if (!is_down) return true;
  using mv::shell::key;
  if (e.k == key::escape && e.mods == mv::shell::mod_none) {
    clear_folder_find(app);
    push_browse_state(app);
    return true;
  }
  if (e.k == key::backspace && e.mods == mv::shell::mod_none) {
    while (!app->folder_query.empty() &&
           (static_cast<unsigned char>(app->folder_query.back()) & 0xC0) == 0x80) {
      app->folder_query.pop_back();
    }
    if (!app->folder_query.empty()) app->folder_query.pop_back();
    touch_folder_find(app);
    move_folder_cursor_to_query(app);
    push_browse_state(app);
    return true;
  }
  if (e.k == key::enter && e.mods == mv::shell::mod_none) {
    const int cursor = app->folder_cursor;
    clear_folder_find(app);
    if (cursor >= 0) open_subfolder_at(app, static_cast<std::uint32_t>(cursor));
    else push_browse_state(app);
    return true;
  }
  if (e.mods != mv::shell::mod_none) {
    clear_folder_find(app);
    push_browse_state(app);
    return false;
  }
  const auto code = static_cast<std::uint16_t>(e.k);
  if (code == static_cast<std::uint16_t>(mv::shell::char_key('/'))) return true;
  if (code < 0x21 || code > 0x7E) {
    clear_folder_find(app);
    push_browse_state(app);
    return false;
  }
  app->folder_query.push_back(static_cast<char>(code));
  touch_folder_find(app);
  move_folder_cursor_to_query(app);
  push_browse_state(app);
  return true;
}

bool handle_app_key(app_state* app, const MSG& msg) noexcept {
  // Mid-teardown a key must not open a dialog or touch a detached island.
  if (!app || app->closing) return false;
  const bool is_down = msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN;
  const bool is_up = msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP;
  if (!is_down && !is_up) return false;
  // Milestone H: an add-on's own top-level window (the search panel, the
  // people window) marks itself; its keys are its text and its grid, never
  // viewer commands. Import's window predates the mark and is unchanged.
  if (msg.hwnd) {
    const HWND top = ::GetAncestor(msg.hwnd, GA_ROOT);
    if (top && top != app->window && ::GetPropW(top, L"MediaViewer.AddonWindow")) return false;
  }
  // PR 30: keys aimed at the Video Editor are its own (editor_key). While it
  // is open the viewer's keys raise it instead: the canvas is over there, and
  // A / D here would walk the folder out from under the edit. Alt+ keys still
  // reach the system (Alt+F4, Alt+Tab).
  if (app->editor.open && app->editor.window) {
    const HWND editor = app->editor.window;
    if (msg.hwnd == editor || ::IsChild(editor, msg.hwnd)) {
      try {
        return editor_key(app, msg);
      } catch (...) {
        return true;
      }
    }
    if (msg.message == WM_KEYDOWN) {
      ::SetForegroundWindow(editor);
      return true;
    }
    if (msg.message == WM_KEYUP) return true;
  }
  const auto event = translate_key(msg, is_up);
  if (event.k == mv::shell::key::none) return false;
  if (handle_folder_find(app, event, is_down)) return true;
  const auto routed = app->router.on_key(event, view_state_of(app));
  if (!routed.handled) return false;
  // Issue #38: a transport key also wakes the controls. The key still runs
  // exactly its own command; waking is never a second action.
  if (is_down && mv::shell::is_transport_command(routed.command)) transport_activity(app);
  if (routed.command == mv::shell::command_id::back) {
    walk_back(app, routed.back);
    return true;
  }
  if (routed.command == mv::shell::command_id::none) return true;
  return run_command(app, routed.command);
}

// ---- PR 29: the Edit workspace's verify rig ----------------------------------------
//
// MV_EDIT_SELFTEST=<folder> (docs/design/20 verify; the Mac twin is main_mac.mm's).
// Inert unless set. After launch it walks the workspace through the commands its
// buttons and keys run -- open, a 3:2 crop, apply, the Colour and Info tabs (a
// tag and the date set), Show original, Save copy, Esc, Revert -- or, on a clip,
// the Video Editor (PR 30: split at a third and two thirds, delete the middle,
// export both ways) -- and writes the window (PrintWindow, canvas included) as
// BMPs plus state.txt into <folder>, then closes. The only files it writes
// beside the media are Save copy's and the two exports; the metadata edits are
// reverted byte for byte.
constexpr UINT_PTR kEditSelfTestTimerId = 0x7A01;
constexpr UINT kEditSelfTestStepMs = 1500;
std::wstring g_edit_selftest_dir;
int g_edit_selftest_step = 0;

void edit_selftest_capture(HWND window, const std::wstring& path) noexcept {
  RECT rc{};
  if (!window || !::GetClientRect(window, &rc)) return;
  const int w = rc.right - rc.left, h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) return;
  HDC screen = ::GetDC(window);
  HDC mem = ::CreateCompatibleDC(screen);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = h;  // bottom-up, as a BMP stores it
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib = ::CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (dib && bits) {
    HGDIOBJ old = ::SelectObject(mem, dib);
    // PW_RENDERFULLCONTENT (2): the DirectComposition content too -- the
    // swapchain and the XAML islands, not just the GDI client.
    (void)::PrintWindow(window, mem, PW_CLIENTONLY | 2);
    ::SelectObject(mem, old);
    const DWORD image = static_cast<DWORD>(w) * static_cast<DWORD>(h) * 4;
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;  // "BM"
    fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
    fh.bfSize = fh.bfOffBits + image;
    if (FILE* f = _wfopen(path.c_str(), L"wb")) {
      std::fwrite(&fh, sizeof(fh), 1, f);
      std::fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f);
      std::fwrite(bits, image, 1, f);
      std::fclose(f);
    }
  }
  if (dib) ::DeleteObject(dib);
  ::DeleteDC(mem);
  ::ReleaseDC(window, screen);
}

void edit_selftest_snap(app_state* app, const char* name) noexcept {
  try {
    const std::wstring wide_name = wide_from_utf8(name);
    edit_selftest_capture(app->window, g_edit_selftest_dir + L"\\" + wide_name + L".bmp");
    // PR 30: the Video Editor window too, preview (the canvas) and timeline.
    if (app->editor.window) {
      edit_selftest_capture(app->editor.window, g_edit_selftest_dir + L"\\" + wide_name + L"-editor.bmp");
    }
    const mv::shell::edit_subject subject = edit_subject_of(app);
    std::uint32_t w = 0, h = 0;
    int crop_w = 0, crop_h = 0;
    if (subject == mv::shell::edit_subject::still && app->lab.still_size(app->edit_key, &w, &h)) {
      app->edits.set_size(w, h);
      const mv::edit::placement p = app->edits.preview_placement();
      const mv::edit::rect r = app->edits.crop_active() ? app->edits.crop_overlay() : mv::edit::rect{0, 0, 1, 1};
      crop_w = static_cast<int>(std::lround(r.w * static_cast<float>(p.cropped.w)));
      crop_h = static_cast<int>(std::lround(r.h * static_cast<float>(p.cropped.h)));
    }
    char line[512];
    std::snprintf(line, sizeof(line),
                  "%s open=%d tab=%d subject=%d crop=%d aspect=%d portrait=%d crop_px=%dx%d edits=%d "
                  "original=%d edit_pane=%d adjust=%d meta=%d jobs=%d trim=%d right_px=%u\n",
                  name, app->ws.open ? 1 : 0, static_cast<int>(app->ws.tab), static_cast<int>(subject),
                  app->edits.crop_active() ? 1 : 0, static_cast<int>(app->edits.aspect()),
                  app->edits.aspect_portrait() ? 1 : 0, crop_w, crop_h, static_cast<int>(app->edits.edit_count()),
                  app->show_original ? 1 : 0, app->chrome.edit_pane_visible() ? 1 : 0,
                  app->chrome.adjust_pane_visible() ? 1 : 0, app->chrome.meta_pane_visible() ? 1 : 0,
                  app->chrome.jobs_pane_visible() ? 1 : 0, app->trim.armed() ? 1 : 0, app->input.chrome_right_px);
    std::string text = line;
    if (app->editor.open || app->editor.timeline.loaded()) {
      std::snprintf(line, sizeof(line),
                    "    editor: open=%d pieces=%zu length_ms=%lld edited=%d strip=%zu peaks=%zu canvas_moved=%d\n",
                    app->editor.open ? 1 : 0, app->editor.timeline.pieces().size(),
                    static_cast<long long>(app->editor.timeline.length() / 1'000'000),
                    app->editor.timeline.edited() ? 1 : 0, app->editor.strip.size(), app->editor.peaks.size(),
                    app->lab.canvas_window() != 0 ? 1 : 0);
      text += line;
      for (const auto& piece : app->editor.timeline.pieces()) {
        std::snprintf(line, sizeof(line), "      piece %lld..%lld ms\n", static_cast<long long>(piece.in_ns / 1'000'000),
                      static_cast<long long>(piece.out_ns / 1'000'000));
        text += line;
      }
    }
    // PR 30: the export jobs (no paths: the titles and the outcome).
    if (app->session) {
      std::uint32_t count = 0;
      if (mv_clip_jobs(app->session, nullptr, 0, &count) == MV_OK && count > 0) {
        std::vector<std::uint64_t> ids(count);
        if (mv_clip_jobs(app->session, ids.data(), count, &count) == MV_OK) {
          for (std::uint32_t i = 0; i < count && i < ids.size(); ++i) {
            mv_clip_progress pr{};
            if (mv_clip_job_progress(app->session, ids[i], &pr) != MV_OK) continue;
            std::snprintf(line, sizeof(line), "    job: op=%u state=%u fraction=%.2f error=%u outputs=%u title=%s\n",
                          pr.op, pr.state, pr.fraction, pr.error, pr.output_count, pr.title_utf8);
            text += line;
          }
        }
      }
    }
    if (app->meta_record) {
      std::string artist;
      for (const auto& p : app->meta_record->properties) {
        if (p.raw_tag == "Exif.Image.Artist") artist = p.value;
      }
      text += "    meta: date=" + app->meta_record->s.date_taken + " artist=" + artist +
              " in_file=" + (app->meta_record->writes_in_file ? "1" : "0") + "\n";
    }
    if (FILE* f = _wfopen((g_edit_selftest_dir + L"\\state.txt").c_str(), L"ab")) {
      std::fwrite(text.data(), 1, text.size(), f);
      std::fclose(f);
    }
  } catch (...) {
  }
}

// PR 30: MV_EDIT_SELFTEST_KEYS=1 drives the Video Editor by key instead: each
// step is a WM_KEYDOWN / WM_KEYUP pair through handle_app_key, the path a real
// key takes from the message loop, with Ctrl / Shift in the thread's key state.
// Synthesised, so it proves the routing and the bindings, not the keyboard.
bool g_edit_selftest_keys = false;

void edit_selftest_key(app_state* app, HWND target, UINT vk, bool ctrl = false, bool shift = false) noexcept {
  BYTE state[256]{};
  (void)::GetKeyboardState(state);
  state[VK_CONTROL] = state[VK_LCONTROL] = ctrl ? 0x80 : 0;
  state[VK_SHIFT] = state[VK_LSHIFT] = shift ? 0x80 : 0;
  (void)::SetKeyboardState(state);
  MSG msg{};
  msg.hwnd = target;
  msg.message = WM_KEYDOWN;
  msg.wParam = vk;
  msg.lParam = 1;
  if (!handle_app_key(app, msg)) {
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }
  msg.message = WM_KEYUP;
  msg.lParam = 1 | (1 << 30) | static_cast<LPARAM>(1u << 31);
  (void)handle_app_key(app, msg);
  state[VK_CONTROL] = state[VK_LCONTROL] = 0;
  state[VK_SHIFT] = state[VK_LSHIFT] = 0;
  (void)::SetKeyboardState(state);
}

// Open (Enter); from the end (End), J back five seconds and mark out (O),
// J back five more and mark in (I); delete the marked range (Delete); L plays
// and K stops; export both ways (Ctrl+E, Ctrl+Shift+E); close (Esc, which
// does not ask: the edit was exported). One key per step where a seek has to
// land first; the steps are further apart than J's burst, so each J is 1 s.
bool edit_selftest_keys_tick(app_state* app, int step) {
  const HWND ed = app->editor.window;
  switch (step) {
    case 0: break;  // let the clip load
    case 1:
      edit_selftest_snap(app, "k0-viewer");
      if (app->session) (void)mv_video_pause(app->session);
      focus_canvas(app);
      edit_selftest_key(app, app->window, VK_RETURN);
      break;
    case 2: break;  // the strip is read on a worker
    case 3: edit_selftest_snap(app, "k1-editor"); edit_selftest_key(app, ed, VK_END); break;
    case 4: case 5: case 6: case 7: case 8: edit_selftest_key(app, ed, 'J'); break;
    case 9: edit_selftest_key(app, ed, 'O'); break;
    case 10: case 11: case 12: case 13: case 14: edit_selftest_key(app, ed, 'J'); break;
    case 15: edit_selftest_key(app, ed, 'I'); break;
    case 16: edit_selftest_key(app, ed, VK_DELETE); break;  // the marked range
    case 17: edit_selftest_key(app, ed, 'L'); break;
    case 18: edit_selftest_key(app, ed, 'K'); break;
    case 19:
      edit_selftest_snap(app, "k2-cut");
      edit_selftest_key(app, ed, 'E', true);
      edit_selftest_key(app, ed, 'E', true, true);
      break;
    case 20: case 21: case 22: case 23: break;  // the exports run
    case 24: edit_selftest_snap(app, "k3-exported"); edit_selftest_key(app, ed, VK_ESCAPE); break;
    default: edit_selftest_snap(app, "k4-closed"); return true;
  }
  return false;
}

void edit_selftest_tick(app_state* app) {
  using enum mv::shell::command_id;
  const int step = g_edit_selftest_step++;
  const bool clip = app->editor.open || edit_subject_of(app) == mv::shell::edit_subject::clip;
  bool done = false;
  if (clip && g_edit_selftest_keys) {
    done = edit_selftest_keys_tick(app, step);
  } else if (clip) {
    // PR 30: a clip opens the Video Editor window (the Mac rig's steps).
    const std::int64_t third = app->editor.timeline.length() / 3;
    switch (step) {
      case 0: break;  // let the clip load
      case 1:
        edit_selftest_snap(app, "c0-viewer");
        if (app->session) (void)mv_video_pause(app->session);  // cuts at fixed times, not wherever playback got to
        (void)run_command(app, edit_workspace);
        break;
      case 2: break;  // the strip is read on a worker
      case 3: edit_selftest_snap(app, "c1-editor"); editor_seek(app, third); break;
      case 4: editor_edit(app, 1); editor_seek(app, 2 * third); break;
      case 5: editor_edit(app, 1); editor_select(app, 1); editor_edit(app, 2); break;  // cut the middle third
      case 6: edit_selftest_snap(app, "c2-cut"); editor_export(app, false); editor_export(app, true); break;
      case 7: case 8: case 9: case 10: break;  // the exports run (exact re-encodes on the GPU)
      case 11: edit_selftest_snap(app, "c3-exported"); set_editor_open(app, false); break;
      default: edit_selftest_snap(app, "c4-closed"); done = true; break;
    }
  } else {
    switch (step) {
      case 0: break;  // let the photo decode
      case 1: edit_selftest_snap(app, "s0-viewer"); (void)run_command(app, edit_workspace); break;
      case 2: edit_selftest_snap(app, "s1-workspace"); edit_set_aspect(app, 4); break;  // 3:2
      case 3: edit_selftest_snap(app, "s2-crop-3x2"); (void)run_command(app, crop_commit); break;
      case 4: edit_selftest_snap(app, "s3-applied"); edit_select_tab(app, 1); break;
      case 5: edit_selftest_snap(app, "s4-colour"); edit_select_tab(app, 2); break;
      case 6: {
        edit_selftest_snap(app, "s5-info");
        // Any tag, and the date, from the Info tab (what its Enter sends).
        const std::string path = current_item_path(app);
        mv::meta::write_fields f;
        f.tags.push_back({"Exif.Image.Artist", mv::meta::change<std::string>::to("MV self-test")});
        f.date_taken = mv::meta::change<std::string>::to("2020-02-02 10:00:00");
        app->meta_writer.submit(path, f);
        schedule_meta_write(app, kCommentDebounceMs);
        break;
      }
      case 7:
        edit_selftest_snap(app, "s5b-info-edited");
        edit_select_tab(app, 0);
        run_edit_action(app, static_cast<int>(mv::shell::chrome_edit_action::original_on));
        break;
      case 8:
        edit_selftest_snap(app, "s6-original");
        run_edit_action(app, static_cast<int>(mv::shell::chrome_edit_action::original_off));
        run_edit_action(app, static_cast<int>(mv::shell::chrome_edit_action::save_copy));
        break;
      case 9:
        edit_selftest_snap(app, "s7-save-copy");
        // The dialog's Save with its defaults (what chrome_cmd_export runs).
        app->chrome.show_popup(mv::shell::chrome_popup::close, 0);
        app->popup_open = false;
        start_export(app, mv::shell::unpack_export(app->export_choice));
        break;
      case 10:
        walk_back(app, mv::shell::back_target::pane);
        revert_current_metadata(app);
        break;
      default: edit_selftest_snap(app, "s8-closed-reverted"); done = true; break;
    }
  }
  if (done) {
    ::KillTimer(app->window, kEditSelfTestTimerId);
    ::PostMessageW(app->window, WM_CLOSE, 0, 0);
  }
}

// The right column's width: the metadata, adjust, Jobs and (PR 29) Edit panes.
int right_pane_px(int client_width, std::uint32_t dpi) noexcept {
  return std::min(client_width / 2, ::MulDiv(340, static_cast<int>(dpi), 96));
}

// PR 9. The panes float over the canvas: the metadata pane on the right, the tree
// on the left, both between the command bar and the bottom strips. Native owns the
// maths (the island only moves), and none of it touches the canvas rectangle, so
// opening one never refits the photo or the present path (docs/design/12 2026-09-24).
// PR 29 (docs/design/20, docs/design/12 2026-09-26): the Edit workspace is the exception. It
// docks: its strip heads the right column, the tab's pane hangs under it, and
// the canvas frames the picture beside them (update_client_metrics sets
// chrome_right_px). Still one swapchain, refitted, never resized.
void layout_panels(app_state* app) noexcept {
  if (!app || !app->window || !app->chrome.panels_attached()) return;
  RECT rc{};
  ::GetClientRect(app->window, &rc);
  const auto dpi = ::GetDpiForWindow(app->window);
  const int width = rc.right - rc.left;
  const int height = rc.bottom - rc.top;
  const int bar = chrome_bar_px(app, dpi);
  // The transport floats (issue #38); only the filmstrip is a strip.
  int bottom = 0;
  if (app->chrome.filmstrip_visible()) bottom += mv::shell::chrome_filmstrip_height_px(dpi);
  const int top = bar;
  const int span = std::max(height - bar - bottom, 1);
  // Hidden under the gallery (it covers the client), fullscreen chrome-off and
  // Settings; the wish survives and the pane returns with them.
  const bool chrome_hidden = app->fullscreen && !app->fullscreen_reveal;
  const bool covered = app->gallery_visible || app->settings_open || chrome_hidden;
  const int side = right_pane_px(width, dpi);
  const int tree_w = std::min(width / 2, ::MulDiv(280, static_cast<int>(dpi), 96));
  const bool want_meta = app->meta_pane_visible && !covered;
  const bool want_tree = app->tree_visible && !covered;
  // PR 13 / 14: the Jobs pane takes the right edge over adjust and metadata
  // (one at a time; their wishes survive and they return when it closes).
  const bool want_jobs = app->jobs_pane_visible && !covered;
  // PR 11: the adjust pane takes the metadata pane's edge (one at a time).
  const bool want_adjust = app->adjust.visible() && !covered && !want_jobs;
  // PR 29: the Edit workspace. Crop and Trim are its own island's pane, so it
  // spans the column; on Colour / Info / Jobs it is the strip alone and that
  // pane starts under it.
  const bool want_edit = app->ws.open && !covered;
  int pane_top = top;
  int pane_span = span;
  if (want_edit) {
    const int strip_h = std::min(::MulDiv(kEditStripDip, static_cast<int>(dpi), 96), span);
    const bool own_pane = app->ws.tab == mv::shell::edit_tab::crop || app->ws.tab == mv::shell::edit_tab::trim;
    app->chrome.show_edit_pane(true, width - side, top, side, own_pane ? span : strip_h);
    pane_top = top + strip_h;
    pane_span = std::max(span - strip_h, 1);
  } else {
    app->chrome.show_edit_pane(false, width - side, top, side, span);
  }
  app->chrome.show_meta_pane(want_meta && !want_adjust && !want_jobs, width - side, pane_top, side, pane_span,
                             app->focus_meta_next);
  app->chrome.show_adjust_pane(want_adjust, width - side, pane_top, side, pane_span, app->focus_adjust_next);
  app->chrome.show_jobs_pane(want_jobs, width - side, pane_top, side, pane_span, app->focus_jobs_next);
  if (want_jobs) app->focus_jobs_next = false;
  app->chrome.show_folder_tree(want_tree, 0, top, tree_w, span, app->focus_tree_next);
  if (want_meta && !want_adjust && !want_jobs) app->focus_meta_next = false;
  if (want_adjust) app->focus_adjust_next = false;
  if (want_tree) app->focus_tree_next = false;
  if (want_meta && !want_adjust && !want_jobs) push_meta_pane(app);
  if (want_adjust) push_adjust_pane(app);
  layout_editor(app);  // PR 30: the card beside whatever pane is up
}

void layout_chrome(app_state* app) noexcept {
  if (!app || !app->window || !app->chrome.attached()) return;
  RECT rc{};
  ::GetClientRect(app->window, &rc);
  const auto dpi = ::GetDpiForWindow(app->window);
  const int bar = chrome_bar_px(app, dpi);
  const int width = rc.right - rc.left;
  const int height = rc.bottom - rc.top;
  // Fullscreen parks the bar, except while a flyout hangs off it. Settings
  // expands the bar island to cover the canvas.
  if (app->settings_open) app->chrome.resize(width, height, dpi);
  else if (app->fullscreen && !app->popup_open) app->chrome.park_bar(height);
  else app->chrome.resize(width, bar, dpi);
  if (app->chrome.filmstrip_attached()) app->chrome.resize_filmstrip(width, height, dpi);
  const int strip = app->chrome.filmstrip_visible() ? mv::shell::chrome_filmstrip_height_px(dpi) : 0;
  if (app->chrome.transport_attached()) app->chrome.resize_transport(width, height, strip, dpi);
  if (app->chrome.gallery_attached()) app->chrome.resize_gallery(width, height, dpi);
  layout_panels(app);
}

bool attach_chrome(app_state* app) {
  if (!app || !app->chrome_enabled || !app->window) return false;
  if (auto loaded = app->chrome.load(); !loaded) return false;
  RECT rc{};
  ::GetClientRect(app->window, &rc);
  const auto dpi = ::GetDpiForWindow(app->window);
  const int bar = chrome_bar_px(app, dpi);
  auto attached = app->chrome.attach(app->window, app, &chrome_on_command,
                                     rc.right - rc.left, bar, dpi);
  if (!attached) return false;
  const int height = rc.bottom - rc.top;
  (void)app->chrome.attach_filmstrip(app->window, app, &chrome_on_command, app->session,
                                     rc.right - rc.left, height, dpi);
  // The strip is attached visible; nothing is open yet, so park it until a
  // listing says otherwise.
  app->chrome.show_filmstrip(false, rc.right - rc.left, height, dpi);
  (void)app->chrome.attach_transport(app->window, app, &chrome_on_command, app->session,
                                     rc.right - rc.left, height, dpi);
  (void)app->chrome.attach_gallery(app->window, app, &chrome_on_command, app->session,
                                   rc.right - rc.left, height, dpi);
  (void)app->chrome.attach_panels(app->window, app, &chrome_on_command, app->session,
                                  rc.right - rc.left, height, dpi);
  app->chrome.apply_settings(chrome_flags(app), app->settings.sort);
  // `?` and the palette read the same static table as the router (docs/design/16).
  publish_command_table(app);
  push_recent_folders(app);
  app->chrome.refresh_island_windows();
  return true;
}

void update_client_metrics(app_state* app, HWND hwnd) noexcept {
  // PR 30: the Video Editor has the canvas; its preview is the rectangle.
  if (app->editor.open && app->editor.window) {
    editor_canvas_metrics(app);
    return;
  }
  RECT rc{};
  ::GetClientRect(hwnd, &rc);
  app->input.width = static_cast<std::uint32_t>(rc.right - rc.left);
  app->input.height = static_cast<std::uint32_t>(rc.bottom - rc.top);
  const auto dpi = ::GetDpiForWindow(hwnd);
  app->input.dpi_scale = static_cast<float>(dpi) / 96.0f;
  app->input.chrome_height_px =
      (app->chrome_on_screen && !app->fullscreen)
          ? static_cast<std::uint32_t>(chrome_bar_px(app, dpi))
          : 0;
  // The filmstrip reserves canvas. The transport floats over the video and
  // auto-hides (issue #38, docs/design/12 2026-09-26), so showing, parking or hiding
  // it never refits the canvas.
  int bottom = 0;
  if (app->chrome.filmstrip_visible()) bottom += mv::shell::chrome_filmstrip_height_px(dpi);
  app->input.chrome_bottom_px = static_cast<std::uint32_t>(bottom);
  // PR 29: the docked Edit workspace; the canvas frames the picture left of it.
  app->input.chrome_right_px =
      app->chrome.edit_pane_visible()
          ? static_cast<std::uint32_t>(right_pane_px(static_cast<int>(app->input.width), dpi))
          : 0;
}

// Single place that decides which islands are on screen, so the strip, the
// gallery and the canvas rectangle can never disagree. Cheap and idempotent:
// the show_* calls are no-ops when nothing changed.
void apply_view_state(app_state* app) noexcept {
  if (!app || !app->window || !app->chrome.attached()) return;
  RECT rc{};
  ::GetClientRect(app->window, &rc);
  const auto dpi = ::GetDpiForWindow(app->window);
  const int width = rc.right - rc.left;
  const int height = rc.bottom - rc.top;

  const bool have_media = folder_count(app) > 1;
  if (app->mode == open_mode::none) app->gallery_visible = false;
  sync_video_hold(app);

  // Fullscreen hides chrome (docs/design/16) unless ↓ or the hot-edge revealed it.
  const bool chrome_hidden = app->fullscreen && !app->fullscreen_reveal;
  const bool settings = app->settings_open;
  // PR 30: the Video Editor has the canvas; the viewer's strip and transport
  // step aside (a click on the strip would change the clip under the edit).
  const bool want_filmstrip =
      have_media && !app->gallery_visible && !chrome_hidden && !settings && !app->editor.open &&
      (app->mode == open_mode::image ? app->settings.filmstrip_for_image
       : app->mode == open_mode::folder ? app->settings.filmstrip_for_folder
                                        : false);
  if (want_filmstrip != app->chrome.filmstrip_visible()) {
    app->chrome.show_filmstrip(want_filmstrip, width, height, dpi);
  }
  const bool want_gallery = app->gallery_visible && !settings;
  if (want_gallery != app->chrome.gallery_visible()) {
    app->chrome.show_gallery(want_gallery, width, height, dpi);
  }
  // Auto show/hide: a clip is open, and the grid is not covering everything.
  // Ordered after the filmstrip so the strip height it stacks on is current.
  // A Live Photo stop is a still (PR 7): its motion is a moment, not a clip to
  // scrub. Fullscreen keeps it (issue #38): it floats and auto-hides there as
  // it does windowed, instead of waiting for the ↓ / hot-edge reveal.
  const bool want_transport = app->video_on && !app->gallery_visible && !settings && !app->editor.open &&
                              current_pair_kind(app) != MV_PAIR_LIVE_PHOTO;
  const int strip = app->chrome.filmstrip_visible()
                        ? mv::shell::chrome_filmstrip_height_px(dpi) : 0;
  if (want_transport != app->chrome.transport_visible()) {
    app->chrome.show_transport(want_transport, width, height, strip, dpi);
  } else if (want_transport) {
    app->chrome.resize_transport(width, height, strip, dpi);
  }
  layout_panels(app);
  update_client_metrics(app, app->window);
  apply_transport_autohide(app);
  ++app->input.resize_seq;
  ++app->input.activity_seq;
  publish(app);
}

// Issue #38. The rule is shell/transport_autohide.h, shared with the Mac host;
// this feeds it Win32 facts and applies the answer: park the bar (it keeps its
// content, and a click where it was reaches the canvas, not a hidden button)
// and, in fullscreen over the video, the pointer. Hiding is only visual: the
// canvas rectangle does not move and every key still routes.
void apply_transport_autohide(app_state* app) noexcept {
  if (!app || !app->window) return;
  const HWND hwnd = app->window;
  const ULONGLONG now = ::GetTickCount64();
  mv::shell::transport_view v;
  v.clip = app->chrome.transport_visible();
  v.playing = app->video_playing;
  if (v.clip) {
    const bool over = !app->chrome.transport_parked() && app->chrome.cursor_over_transport();
    const bool focused =
        app->chrome.classify_focus(::GetFocus(), hwnd) == mv::shell::focus_kind::transport;
    BOOL reader = FALSE;
    (void)::SystemParametersInfoW(SPI_GETSCREENREADER, 0, &reader, 0);
    v.held = app->transport_hold || over || focused;
    v.screen_reader = reader != FALSE;
    v.fullscreen = app->fullscreen;
    v.pointer_on_canvas = app->input.mouse_in_client;
  }
  const bool pointer_was_hidden = app->autohide.pointer_hidden();
  (void)app->autohide.update(v, now);
  if (v.clip) {
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    const auto dpi = ::GetDpiForWindow(hwnd);
    const int strip = app->chrome.filmstrip_visible()
                          ? mv::shell::chrome_filmstrip_height_px(dpi) : 0;
    app->chrome.park_transport(!app->autohide.shown(), rc.right - rc.left,
                               rc.bottom - rc.top, strip, dpi);
  }
  // WM_SETCURSOR keeps it hidden; this applies it now rather than on the next move.
  if (app->autohide.pointer_hidden() != pointer_was_hidden) {
    ::SetCursor(app->autohide.pointer_hidden() ? nullptr : ::LoadCursorW(nullptr, IDC_ARROW));
  }
  const std::uint64_t due = app->autohide.due_in(v, now);
  if (due > 0) ::SetTimer(hwnd, kTransportTimerId, static_cast<UINT>(due), nullptr);
  else ::KillTimer(hwnd, kTransportTimerId);
  app->transport_timer = due > 0;
}

// Pointer moved or clicked, a transport key or button, a fullscreen change.
// A bool test and nothing else with no clip; with the bar up and its timer
// running, a stream of mouse-moves costs a stored tick, not a timer each.
void transport_activity(app_state* app) noexcept {
  if (!app || !app->chrome.transport_visible()) return;
  app->autohide.activity(::GetTickCount64());
  if (app->autohide.shown() && (app->transport_timer || !app->video_playing)) return;
  apply_transport_autohide(app);
}

// PR 8: title bar and taskbar icons at the window's own DPI, so a 150 % monitor
// gets the 24/48 px frames rather than a stretched 16/32. WM_SETICON does not
// take ownership, so the previous pair is destroyed after the swap.
void apply_window_icons(HWND hwnd) noexcept {
  static HICON icon_big = nullptr;
  static HICON icon_small = nullptr;
  const int dpi = static_cast<int>(::GetDpiForWindow(hwnd));
  const HINSTANCE instance = ::GetModuleHandleW(nullptr);
  HICON next_big = nullptr;
  HICON next_small = nullptr;
  (void)::LoadIconWithScaleDown(instance, MAKEINTRESOURCEW(MV_IDI_APP),
                                ::GetSystemMetricsForDpi(SM_CXICON, dpi),
                                ::GetSystemMetricsForDpi(SM_CYICON, dpi), &next_big);
  (void)::LoadIconWithScaleDown(instance, MAKEINTRESOURCEW(MV_IDI_APP),
                                ::GetSystemMetricsForDpi(SM_CXSMICON, dpi),
                                ::GetSystemMetricsForDpi(SM_CYSMICON, dpi), &next_small);
  if (next_big) ::SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(next_big));
  if (next_small) ::SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(next_small));
  if (next_big && icon_big) ::DestroyIcon(icon_big);
  if (next_small && icon_small) ::DestroyIcon(icon_small);
  if (next_big) icon_big = next_big;
  if (next_small) icon_small = next_small;
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }

  app_state* app = state_from(hwnd);
  if (!app) return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  if (app->taskbar_created_msg != 0 && msg == app->taskbar_created_msg) {
    on_taskbar_button_created(app);
    return 0;
  }

  switch (msg) {
    case WM_SIZE: {
      if (wparam == SIZE_MINIMIZED) {
        app->input.window_visible = false;
      } else {
        app->input.window_visible = true;
        update_client_metrics(app, hwnd);
        layout_chrome(app);
        ++app->input.resize_seq;
      }
      publish(app);
      return 0;
    }

    case WM_DPICHANGED: {
      // PerMonitorV2: take the suggested rectangle, then let the render thread
      // resize the swapchain. The image itself is resampled on the GPU, so a
      // DPI change costs nothing but a resize (docs/design/03).
      const auto* suggested = reinterpret_cast<const RECT*>(lparam);
      ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
      update_client_metrics(app, hwnd);
      layout_chrome(app);
      ++app->input.resize_seq;
      publish(app);
      apply_window_icons(hwnd);
      return 0;
    }

    case WM_DISPLAYCHANGE:
    case WM_MOVE: {
      // The window may now be on a different output — different refresh rate,
      // possibly a different GPU. The render thread re-checks both.
      ++app->input.display_change_seq;
      publish(app);
      return 0;
    }

    case WM_ACTIVATE: {
      app->main_active = LOWORD(wparam) != WA_INACTIVE;
      app->input.window_active = app->main_active || app->editor.active;
      // A key held across Alt+Tab never sends its key-up here, so fire the
      // releases it owes: a loupe must not stick on, a skim must settle exact.
      if (!app->input.window_active) {
        mv::shell::command_id released[mv::shell::key_router::kHeldSlots]{};
        const std::size_t n = app->router.cancel_holds(released);
        for (std::size_t i = 0; i < n; ++i) (void)run_command(app, released[i]);
      }
      publish(app);
      return 0;
    }

    case WM_MOUSEMOVE: {
      // Fullscreen hot-edge: the bottom few pixels reveal the strips.
      if (app->fullscreen) {
        RECT rc{};
        ::GetClientRect(hwnd, &rc);
        const int edge = ::MulDiv(4, static_cast<int>(::GetDpiForWindow(hwnd)), 96);
        if (GET_Y_LPARAM(lparam) >= rc.bottom - edge) set_fullscreen_reveal(app, true);
      }
      const bool moved = !app->input.mouse_in_client ||
                         app->input.mouse_x != static_cast<float>(GET_X_LPARAM(lparam)) ||
                         app->input.mouse_y != static_cast<float>(GET_Y_LPARAM(lparam));
      if (moved) ++app->input.activity_seq;
      app->input.mouse_x = static_cast<float>(GET_X_LPARAM(lparam));
      app->input.mouse_y = static_cast<float>(GET_Y_LPARAM(lparam));
      app->input.mouse_in_client = true;
      // A recent folder under the pointer: highlighted, with the hand cursor
      // (WM_SETCURSOR). The move above already redraws.
      if (app->input.recents.count != 0) update_welcome_hover(app);
      // Issue #38: movement (and entry) wakes the transport. Only a real move:
      // parking an island can send a synthetic one at the same spot.
      if (moved) transport_activity(app);
      if (!app->tracking_mouse) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&tme);
        app->tracking_mouse = true;
      }
      // At fit, left-drag is not a pan. After the system drag threshold it
      // is a file drag of the current item (same CF_HDROP as the gallery).
      if (app->file_drag_armed && (wparam & MK_LBUTTON) && app->lab.view_fitted()) {
        const int dx = GET_X_LPARAM(lparam) - app->file_drag_x;
        const int dy = GET_Y_LPARAM(lparam) - app->file_drag_y;
        const int slop = ::GetSystemMetrics(SM_CXDRAG);
        if (dx * dx + dy * dy >= slop * slop) {
          app->file_drag_armed = false;
          app->input.mouse_down[0] = false;
          ::ReleaseCapture();
          publish(app);
          // PR 15: Ctrl+Alt+drag drags the edited copy (Ctrl+Alt+C's twin);
          // a plain drag stays the original.
          const bool edited = (::GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
                              (::GetKeyState(VK_MENU) & 0x8000) != 0;
          if (!edited || !start_flatten(app, true)) begin_file_drag(app, hwnd, current_item_path(app));
          return 0;
        }
      }
      publish(app);
      return 0;
    }

    case WM_MOUSELEAVE: {
      ++app->input.activity_seq;
      app->tracking_mouse = false;
      app->input.mouse_in_client = false;
      app->input.recents.hover = -1;
      app->input.recents.hover_remove = false;
      publish(app);
      transport_activity(app);  // onto the bar or out of the window
      return 0;
    }

    case WM_SETCURSOR:
      // Issue #38: fullscreen, playing, idle, over the video. Anything else is
      // the class cursor, so the windowed pointer is never touched.
      if (reinterpret_cast<HWND>(wparam) == hwnd && LOWORD(lparam) == HTCLIENT &&
          app->autohide.pointer_hidden()) {
        ::SetCursor(nullptr);
        return TRUE;
      }
      if (reinterpret_cast<HWND>(wparam) == hwnd && LOWORD(lparam) == HTCLIENT &&
          app->input.recents.hover >= 0) {
        ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
      break;

    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: {
      const bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
      const int index = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)   ? 0
                        : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 1
                                                                         : 2;
      app->input.mouse_down[index] = down;
      ++app->input.activity_seq;
      // Clicking the canvas (this HWND, not an island) must take keyboard
      // focus back. Child XAML islands otherwise keep it, and A/D wait for
      // the window to be deactivated and reactivated.
      if (down) {
        ::SetFocus(hwnd);
        app->island_focus = mv::shell::focus_kind::command_bar;
        ::SetCapture(hwnd);
        transport_activity(app);
        if (msg == WM_LBUTTONDOWN && app->lab.view_fitted()) {
          app->file_drag_armed = true;
          app->file_drag_x = GET_X_LPARAM(lparam);
          app->file_drag_y = GET_Y_LPARAM(lparam);
        }
        if (msg == WM_LBUTTONDOWN) {
          // Hit-test where this click is, not where the last WM_MOUSEMOVE
          // was (the release does the same).
          app->input.mouse_x = static_cast<float>(GET_X_LPARAM(lparam));
          app->input.mouse_y = static_cast<float>(GET_Y_LPARAM(lparam));
          app->welcome_press = welcome_row_at_pointer(app, &app->welcome_press_remove);
        }
      }
      else if (!app->input.mouse_down[0] && !app->input.mouse_down[1] &&
               !app->input.mouse_down[2]) {
        ::ReleaseCapture();
        app->file_drag_armed = false;
      }
      publish(app);
      // A click on a recent folder (or its x): pressed and released on the
      // same row and the same part of it.
      if (msg == WM_LBUTTONUP && app->welcome_press >= 0) {
        const int pressed = std::exchange(app->welcome_press, -1);
        app->input.mouse_x = static_cast<float>(GET_X_LPARAM(lparam));
        app->input.mouse_y = static_cast<float>(GET_Y_LPARAM(lparam));
        bool on_remove = false;
        if (welcome_row_at_pointer(app, &on_remove) == pressed && on_remove == app->welcome_press_remove) {
          if (on_remove) remove_welcome_row(app, pressed);
          else open_welcome_row(app, pressed);
        }
      }
      return 0;
    }

    case WM_MOUSEWHEEL: {
      app->input.wheel_total += GET_WHEEL_DELTA_WPARAM(wparam);
      ++app->input.activity_seq;
      publish(app);
      transport_activity(app);
      return 0;
    }

    case WM_KEYDOWN: {
      if ((lparam & (1 << 30)) != 0) return 0;
      // Tab has nowhere visible to go while fullscreen hides the chrome.
      if (wparam == VK_TAB && app->chrome.attached() && !app->fullscreen) {
        // Unpark the transport first so Tab can land in it (issue #38).
        transport_activity(app);
        const bool reverse = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
        (void)app->chrome.navigate_focus(reverse);
      }
      return 0;
    }

    case WM_COPYDATA: {
      const auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lparam);
      if (!cds || cds->dwData != 0x4D560001ul || !cds->lpData || cds->cbData < 2) return 0;
      const auto* w = static_cast<const wchar_t*>(cds->lpData);
      const std::size_t n = static_cast<std::size_t>(cds->cbData) / sizeof(wchar_t);
      // An island forwarding our own drag back to us: nothing to open.
      if (app->own_drag) return 1;
      std::wstring_view blob(w, n);
      if (!blob.empty() && blob.back() == L'\0') blob.remove_suffix(1);
      open_dropped_wide_list(app, blob);
      return 1;
    }

    case WM_DROPFILES: {
      // Every dropped entry, at any path length; open_paths picks the first
      // that exists (docs/design/16).
      auto drop = reinterpret_cast<HDROP>(wparam);
      // Our own drag let go over our own canvas: refused, not a reopen of the
      // folder it came from (the Mac's in-app drags carry no operation).
      if (app->own_drag) {
        ::DragFinish(drop);
        return 0;
      }
      const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      std::vector<std::wstring> paths;
      for (UINT i = 0; i < count && i < 256; ++i) {
        const UINT len = ::DragQueryFileW(drop, i, nullptr, 0);
        if (len == 0) continue;
        std::wstring path(len, L'\0');
        if (::DragQueryFileW(drop, i, path.data(), len + 1) == len) paths.push_back(std::move(path));
      }
      ::DragFinish(drop);
      open_paths(app, paths);
      return 0;
    }

    case mv::shell::kFileJobDoneMessage:
      on_file_job_done(app, std::unique_ptr<mv::shell::file_job_result>(
                                reinterpret_cast<mv::shell::file_job_result*>(lparam)));
      return 0;

    case kMsgEditJobDone:
      on_edit_job_done(app, std::unique_ptr<edit_job_result>(reinterpret_cast<edit_job_result*>(lparam)));
      return 0;

    case kMsgFlattenDone:
      on_flatten_done(app, std::unique_ptr<flatten_job_result>(reinterpret_cast<flatten_job_result*>(lparam)));
      return 0;

    case kMsgEditorLoaded:  // PR 30: the Video Editor's probe and strip
      on_editor_loaded(app, std::unique_ptr<editor_load_result>(reinterpret_cast<editor_load_result*>(lparam)));
      return 0;

    case kMsgOpenForwarded: {
      // A second start's paths (docs/design/09): opened here, as a drop would be, and
      // the window comes forward. An empty hand-off only brings it forward.
      std::unique_ptr<std::wstring> paths(reinterpret_cast<std::wstring*>(lparam));
      if (paths && !paths->empty()) open_dropped_wide_list(app, *paths);
      if (::IsIconic(hwnd)) ::ShowWindow(hwnd, SW_RESTORE);
      ::SetForegroundWindow(hwnd);
      return 0;
    }

    case kMsgOwnDragEnded:
      app->own_drag = false;
      app->chrome.set_drag_paths({}, false);
      return 0;

    case kMsgJumpListPruned:
      on_jump_list_pruned(app, std::unique_ptr<std::vector<std::string>>(
                                   reinterpret_cast<std::vector<std::string>*>(lparam)));
      return 0;

    // PR 15: the taskbar thumbnail toolbar.
    case WM_COMMAND:
      if (HIWORD(wparam) == THBN_CLICKED) {
        using enum mv::shell::command_id;
        switch (LOWORD(wparam)) {
          case kThumbPrev: (void)run_command(app, prev); break;
          case kThumbPlay: (void)run_command(app, play_pause); break;
          case kThumbNext: (void)run_command(app, next); break;
          default: break;
        }
        update_thumb_bar(app);
        return 0;
      }
      break;

    case kMsgAdjustJobDone:
      on_adjust_job_done(app, std::unique_ptr<adjust_job_result>(reinterpret_cast<adjust_job_result*>(lparam)));
      return 0;

    case kMsgMetaReady:
      metadata_ready(app);
      return 0;

    case kMsgSiblingsReady:
      on_siblings_ready(app, std::unique_ptr<sibling_job_result>(
                                 reinterpret_cast<sibling_job_result*>(lparam)));
      return 0;

    case kMsgMetaWriteDone:
      on_meta_write_done(app, std::unique_ptr<meta_write_result>(
                                  reinterpret_cast<meta_write_result*>(lparam)));
      return 0;

    case WM_TIMER:
      if (wparam == kEditorReleaseTimerId) {
        editor_release_tick(app);
        return 0;
      }
      if (wparam == kRotateTimerId) {
        start_rotation_write(app);
        return 0;
      }
      if (wparam == kMetaWriteTimerId) {
        start_meta_write(app);
        return 0;
      }
      if (wparam == kHistogramTimerId) {
        start_histogram(app);
        return 0;
      }
      if (wparam == kMetaTimerId) {
        request_metadata_now(app);
        return 0;
      }
      if (wparam == kTitleTimerId) {
        if (app->folder_find && ::GetTickCount64() - app->folder_find_tick > 1200) {
          clear_folder_find(app);
          push_browse_state(app);
        }
        update_title(app);
        update_thumb_bar(app);
        // An update restart's zoom goes back once the still is on screen; a
        // preset before the decode lands would be replaced by the fit.
        if (g_restore.zoom_percent > 0 && app->lab.showing_still()) {
          app->input.zoom_preset = static_cast<float>(g_restore.zoom_percent) / 100.0f;
          ++app->input.zoom_preset_seq;
          g_restore.zoom_percent = 0;
          publish(app);
        }
        return 0;
      }
      if (wparam == kUpdateConfirmTimerId) {
        ::KillTimer(hwnd, kUpdateConfirmTimerId);
        confirm_update_start_async();
        return 0;
      }
      if (wparam == kSlideshowTimerId) {
        slideshow_tick(app);
        return 0;
      }
      if (wparam == kBrowseTimerId) {
        browse_tick(app);
        return 0;
      }
      if (wparam == kWorkspaceFollowTimerId) {
        workspace_follow_settled(app);
        return 0;
      }
      if (wparam == kEditSelfTestTimerId) {
        edit_selftest_tick(app);
        return 0;
      }
      if (wparam == kMotionTimerId) {
        motion_tick(app);
        return 0;
      }
      if (wparam == kTransportTimerId) {
        ::KillTimer(hwnd, kTransportTimerId);
        app->transport_timer = false;
        apply_transport_autohide(app);
        return 0;
      }
      if (wparam == kRevealTimerId) {
        // Being used is not settled: the cursor over a strip, or a strip with
        // keyboard focus, keeps it up. The canvas gets no mouse-move while the
        // cursor is over an island, so this is the only place to ask.
        const HWND focus = ::GetFocus();
        if (app->chrome.cursor_over_island() || (focus && focus != hwnd)) {
          ::SetTimer(hwnd, kRevealTimerId, kRevealMs, nullptr);
          return 0;
        }
        set_fullscreen_reveal(app, false);
        return 0;
      }
      break;

    case WM_ERASEBKGND:
      return 1;  // the swapchain owns every pixel; never let GDI flash over it

    case WM_CLOSE: {
      // PR 3-era exit fail-fast (0xC0000602 in CoreUIComponents.dll, about 1
      // exit in 6 on main): the islands were disposed from WM_DESTROY, while
      // DestroyWindow was already tearing their bridge windows down under a
      // live DesktopWindowXamlSource. Every exit path — the window's close
      // button, Ctrl+W, and the soak's own PostMessage(WM_CLOSE) — comes
      // through here, so detach while the parent is still whole, let the
      // dispatcher run the dispose it queued, and only then destroy.
      if (app->closing) return 0;
      if (app->editor.open) set_editor_open(app, false);  // PR 30: the canvas comes home first
      app->closing = true;
      app->chrome.detach();
      app->chrome_on_screen = false;
      MSG pending{};
      for (int i = 0; i < 64 && ::PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE); ++i) {
        if (pending.message == WM_QUIT) {
          ::PostQuitMessage(static_cast<int>(pending.wParam));
          break;
        }
        ::TranslateMessage(&pending);
        ::DispatchMessageW(&pending);
      }
      // Every source is gone and its queued dispose has run; now the XAML
      // runtime itself, still before the parent window goes.
      app->chrome.shutdown_for_exit();
      ::DestroyWindow(hwnd);
      return 0;
    }

    case WM_DESTROY:
      app->instance.stop();
      release_taskbar(app);
      app->chrome.detach();
      ::PostQuitMessage(0);
      return 0;

    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

void enable_dark_titlebar(HWND hwnd) noexcept {
  const BOOL dark = TRUE;
  // Ignored on builds that predate it; a viewer with a white title bar around a
  // dark canvas looks broken, so it is worth the two lines (docs/design/09).
  (void)::DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

bool parse_options(lab_options& options, std::vector<std::wstring>& open_paths, bool& chrome_enabled,
                   std::wstring& error) {
  int argc = 0;
  LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  if (!argv) return true;

  bool ok = true;
  bool static_requested = false;
  for (int i = 1; i < argc && ok; ++i) {
    const std::wstring_view arg{argv[i]};
    const auto next = [&](std::wstring& out) {
      if (i + 1 >= argc) { error = std::wstring(arg) + L" needs a value"; ok = false; return; }
      out = argv[++i];
    };

    if (arg == L"--soak") {
      std::wstring value;
      next(value);
      if (ok) {
        wchar_t* end = nullptr;
        options.soak_seconds = std::wcstod(value.c_str(), &end);
        if (end == value.c_str() || *end != L'\0' || !std::isfinite(options.soak_seconds) ||
            options.soak_seconds <= 0.0 || options.soak_seconds > 86400.0) {
          error = L"--soak must be a number in (0, 86400]";
          ok = false;
        }
      }
    } else if (arg == L"--av-soak") {
      std::wstring value; next(value);
      wchar_t* end = nullptr;
      const auto seconds = std::wcstoul(value.c_str(), &end, 10);
      if (value.empty() || *end || seconds == 0 || seconds > 86400) { error = L"--av-soak requires 1..86400 seconds"; ok = false; }
      else options.av_soak_seconds = static_cast<std::uint32_t>(seconds);
    } else if (arg == L"--csv") {
      next(options.av_csv);
    } else if (arg == L"--json") {
      next(options.json_report_path);
    } else if (arg == L"--gate") {
      options.gate_exit_code = true;
    } else if (arg == L"--no-overlay") {
      options.overlay_visible = false;
    } else if (arg == L"--static") {
      static_requested = true;
    } else if (arg == L"--pan-soak") {
      options.scripted_pan = true;
    } else if (arg == L"--browse-soak") {
      g_browse.enabled = true;
    } else if (arg == L"--open") {
      std::wstring value;
      next(value);
      if (ok) open_paths.push_back(std::move(value));
    } else if (arg == L"--no-chrome") {
      chrome_enabled = false;
    } else if (arg == L"--restore-zoom") {
      // PR 8: written by an update restart (update_guard.h), never by a user.
      std::wstring value;
      next(value);
      if (ok) {
        const unsigned long pct = std::wcstoul(value.c_str(), nullptr, 10);
        g_restore.zoom_percent = pct <= 6400 ? static_cast<unsigned>(pct) : 0;
      }
    } else if (arg == L"--new-instance") {
      g_new_instance = true;
    } else if (arg == L"--restore-fullscreen") {
      g_restore.fullscreen = true;
    } else if (arg == L"--restore-gallery") {
      g_restore.gallery = true;
    } else if (!arg.empty() && arg[0] != L'-') {
      // Every positional path: Explorer's "Open" with several files passes
      // them all. open_paths decides (docs/design/16).
      open_paths.emplace_back(arg);
    } else {
      error = L"unrecognised argument: " + std::wstring(arg);
      ok = false;
    }
  }
  ::LocalFree(argv);
  if (ok && g_browse.enabled) {
    if (options.soak_seconds > 0.0) {
      error = L"--browse-soak cannot be combined with --soak";
      ok = false;
    } else if (options.json_report_path.empty()) {
      error = L"--browse-soak needs --json PATH";
      ok = false;
    } else {
      g_browse.json_path = options.json_report_path;
    }
  }
  // Interactive: drop-target empty view. Soak: the PR 1 sweep unless --static.
  options.start_animating = options.soak_seconds > 0.0 && !static_requested;
  return ok;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show_command) {
  // PR 8 updater, before anything that can fail or show a window. Velopack
  // runs the exe with --veloapp-* during install/update/uninstall and kills it
  // after 15-30 s; the viewer has no work there.
  {
    int argc = 0;
    if (LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc)) {
      const bool hook = argc >= 2 && mv::shell::update::is_velopack_hook(argv[1]);
      ::LocalFree(argv);
      if (hook) return 0;
    }
  }
  // Counts this start of a freshly applied version; after two that never
  // confirmed, hands the kept prior package to Update.exe and exits.
  g_install = mv::shell::update::locate_install();
  if (mv::shell::update::run_start_guard(g_install)) return 0;

  // PerMonitorV2 is also declared in the manifest; this is the belt to that
  // braces, because a manifest can be lost by a repackaging step and the
  // failure mode is a blurry window nobody files a bug about.
  ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  // WinUI islands require an STA. GetOpenFileName wants one too. OLE, not just
  // COM: drag-and-drop out of this thread (the islands' DragStarting file drags
  // from the gallery / filmstrip, and the canvas's SHDoDragDrop) is OLE
  // DoDragDrop, which fails on a thread that only called CoInitializeEx.
  // OleInitialize enters the same STA, so everything above still holds.
  (void)::OleInitialize(nullptr);
  // PR 15: before the first window, so its taskbar button, the Start / pinned
  // shortcuts (mediaviewer.iss) and the jump list are one entry.
  (void)::SetCurrentProcessExplicitAppUserModelID(kAppUserModelId);

  lab_options options;
  std::wstring parse_error;
  std::vector<std::wstring> requested_paths;
  bool chrome_enabled = true;
  if (!parse_options(options, requested_paths, chrome_enabled, parse_error)) {
    ::MessageBoxW(nullptr, parse_error.c_str(), kWindowTitle, MB_ICONERROR | MB_OK);
    return 2;
  }

  // PR 15: one MediaViewer per user. A plain start (Explorer, the jump list,
  // a shortcut) hands its paths to the one already running and exits. Soaks,
  // --no-chrome, an update's restart (the old process may still be closing)
  // and --new-instance always run on their own.
  // MV_EDIT_SELFTEST (PR 29 / 30) is a harness run too: handing its clip to a
  // running viewer would walk someone else's window, and its scratch folder
  // must not reach [recent].
  const bool edit_selftest = ::GetEnvironmentVariableW(L"MV_EDIT_SELFTEST", nullptr, 0) > 0;
  const bool harness_run = options.soak_seconds != 0.0 || options.av_soak_seconds != 0 || g_browse.enabled ||
                           options.scripted_pan || edit_selftest;
  const bool single_instance = chrome_enabled && !g_new_instance && !harness_run && g_restore.zoom_percent == 0 &&
                               !g_restore.fullscreen && !g_restore.gallery;
  // Claimed here, not once the window exists: starts that arrive while this
  // one is still loading queue on the pipe instead of becoming "first" too.
  mv::shell::instance_claim instance_claim;
  if (single_instance) {
    if (mv::shell::forward_to_running_instance(requested_paths)) return 0;
    if (!instance_claim.claim()) {
      // Another start claimed the name between our look and our claim.
      if (mv::shell::forward_to_running_instance(requested_paths)) return 0;
      MV_LOG_WARN("single instance: another MediaViewer owns the pipe; this one runs alone");
    }
  }

  if (options.av_soak_seconds) {
    const auto clip = utf8_from_wide(requested_paths.empty() ? std::wstring_view{}
                                                             : std::wstring_view{requested_paths.front()});
    const auto csv = utf8_from_wide(options.av_csv);
    return mv::shell::run_av_soak({clip.c_str(), options.av_soak_seconds, csv.c_str()});
  }
  mv::trace::provider_register();

  // PR 7 crash reporting (docs/design/13 Part 2): Crashpad out-of-process, armed
  // before the session and its decoders exist. Asynchronous; never blocks.
  (void)mv::shell::crash::start();

  // The ABI round-trip, exercised from the native side as well as from C#: the
  // shell is a client of the core through exactly the same header the managed
  // interop uses. If the shell ever reaches around the ABI, the two-language
  // boundary stops being tested by the thing that matters most.
  app_state app;
  (void)app.jobs.start();
  app.chrome_enabled = chrome_enabled;
  // settings.ini was read once, at startup, by app_settings(). From here every
  // settings save is in memory; the file is written on the store's worker, and
  // a write on this (UI) thread is counted as a rule 1 violation.
  mv::shell::register_settings_ui_thread();
  (void)mv::shell::app_settings().start();
  app.settings = mv::shell::load_view_settings();
  app.input.sticky_zoom = app.settings.sticky_zoom;
  app.input.background = app.settings.background;
  app.destinations = mv::shell::load_destinations();
  app.recent_folders = mv::shell::load_recent_folders();
  app.home_utf8 = profile_folder_utf8();
  app.record_recent = !harness_run;
  // Straight into the snapshot: the render thread is not up yet. A launch
  // with a path to open never shows the rows, not even for its first frame.
  if (requested_paths.empty() && welcome_lists_recents(&app)) {
    mv::shell::fill_welcome_recents(app.recent_folders, app.home_utf8, app.input.recents);
  }
  // The toolbar is added when Explorer reports the button, not before.
  app.taskbar_created_msg = ::RegisterWindowMessageW(L"TaskbarButtonCreated");
  for (const auto& o : mv::shell::load_key_overrides()) {
    (void)mv::shell::rebind_live(o.row, static_cast<mv::shell::key>(o.k), o.mods);
  }
  app.router.rebuild(mv::shell::live_bindings());
  if (!app.files.start()) MV_LOG_WARN("files: I/O worker did not start; F7 / F8 / Delete disabled");
  mv_session_config config{};
  config.worker_count = 0;
  config.enable_etw = 1;
  if (mv_session_create(&config, &app.session) != MV_OK) {
    ::MessageBoxA(nullptr, mv_last_error_message(), "MediaViewer", MB_ICONERROR | MB_OK);
    return 2;
  }
  // PR 9: the saved folder sort applies to every open from here on.
  (void)mv_folder_set_sort(app.session, app.settings.sort);
  (void)mv_folder_set_hidden_kinds(app.session, app.settings.hidden_kinds());
  // PR 13 / 14: every clip job that opens a decoder or an encoder runs in
  // MediaViewerClipJob.exe beside this exe, never in the viewer (docs/design/12
  // 2026-09-25). Set even if the file is missing: those jobs then fail
  // rather than run here.
  {
    std::wstring exe(32768, L'\0');
    const DWORD n = ::GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    exe.resize(n);
    const std::size_t slash = exe.find_last_of(L"\\/");
    if (n > 0 && slash != std::wstring::npos) {
      const std::string helper = utf8_from_wide(exe.substr(0, slash + 1) + L"MediaViewerClipJob.exe");
      (void)mv_clip_set_helper(app.session, helper.c_str());
    }
  }

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = window_proc;
  wc.hInstance = instance;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  // Class icons at system DPI; apply_window_icons() replaces them per monitor.
  const int sys_dpi = static_cast<int>(::GetDpiForSystem());
  (void)::LoadIconWithScaleDown(instance, MAKEINTRESOURCEW(MV_IDI_APP),
                                ::GetSystemMetricsForDpi(SM_CXICON, sys_dpi),
                                ::GetSystemMetricsForDpi(SM_CYICON, sys_dpi), &wc.hIcon);
  (void)::LoadIconWithScaleDown(instance, MAKEINTRESOURCEW(MV_IDI_APP),
                                ::GetSystemMetricsForDpi(SM_CXSMICON, sys_dpi),
                                ::GetSystemMetricsForDpi(SM_CYSMICON, sys_dpi), &wc.hIconSm);
  wc.hbrBackground = nullptr;  // the swapchain paints; GDI must not
  wc.lpszClassName = kWindowClass;
  if (!::RegisterClassExW(&wc)) {
    mv_session_release(app.session);
    return 2;
  }

  HWND hwnd = ::CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP,  // required for DComp content
                                kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, nullptr, nullptr,
                                instance, &app);
  if (!hwnd) {
    mv_session_release(app.session);
    return 2;
  }

  enable_dark_titlebar(hwnd);
  apply_window_icons(hwnd);
  ::DragAcceptFiles(hwnd, TRUE);
  ::SetTimer(hwnd, kTitleTimerId, kTitleTickMs, nullptr);
  app.window = hwnd;
  app.chrome_on_screen = app.chrome_enabled;
  update_client_metrics(&app, hwnd);
  app.lab.bind_session(app.session);
  app.lab.publish(app.input);

  // Overlay is the F3 instrument — off until asked, so launch does not freeze
  // a startup-miss overlay on an idle window. Soak keeps it (and animates
  // unless --static). Interactive empty view is the drop target, not the sweep.
  if (options.soak_seconds == 0.0) options.overlay_visible = false;

  if (g_browse.enabled) options.present_when_inactive = true;
  if (auto started = app.lab.start(hwnd, options); !started) {
    ::MessageBoxA(nullptr, "render thread failed to start", "MediaViewer", MB_ICONERROR | MB_OK);
    mv_session_release(app.session);
    return 2;
  }
  if (instance_claim.claimed() && !app.instance.start(hwnd, kMsgOpenForwarded, instance_claim)) {
    MV_LOG_WARN("single instance: the listener did not start; this one runs alone");
  }
  // PR 15: the Explorer thumbnail handler for this version, copied and
  // registered off the UI thread (shell/shellext_install.h). Installed builds only.
  if (g_install.installed()) {
    app.jobs.submit_at(mv::background_generation,
                       [root = g_install.root, version = g_install.version](const mv::job_context&) {
                         mv::shell::install_thumbnail_handler(root, version);
                         return mv::status::ok;
                       });
  }

  ::ShowWindow(hwnd, show_command);
  ::UpdateWindow(hwnd);
  if (app.chrome_enabled && !requested_paths.empty()) open_paths_early(&app, requested_paths);
  if (app.chrome_enabled && !attach_chrome(&app)) {
    MV_LOG_WARN("chrome: island did not attach; command bar is unavailable");
    app.chrome_on_screen = false;
  }
  if (app.chrome_enabled) {
    update_client_metrics(&app, hwnd);
    ++app.input.resize_seq;
    publish(&app);
  }
  if (!requested_paths.empty()) open_paths(&app, requested_paths);
  if (g_browse.enabled) ::SetTimer(hwnd, kBrowseTimerId, kBrowseTickMs, nullptr);
  // PR 29: MV_EDIT_SELFTEST=<folder> walks the Edit workspace (docs/design/20 verify).
  if (wchar_t dir[MAX_PATH]{}; ::GetEnvironmentVariableW(L"MV_EDIT_SELFTEST", dir, MAX_PATH) > 0) {
    g_edit_selftest_dir = dir;
    g_edit_selftest_keys = ::GetEnvironmentVariableW(L"MV_EDIT_SELFTEST_KEYS", nullptr, 0) > 0;
    (void)::CreateDirectoryW(dir, nullptr);
    MV_LOG_WARN("edit: MV_EDIT_SELFTEST armed; the app will close when it is done");
    ::SetTimer(hwnd, kEditSelfTestTimerId, kEditSelfTestStepMs, nullptr);
  }
  if (g_restore.fullscreen) set_fullscreen(&app, true);
  // Chrome attached (or was not asked for) and the window is up: start the
  // clock on "this version starts". A crash before it fires counts.
  if (g_install.installed() && (!app.chrome_enabled || app.chrome_on_screen)) {
    ::SetTimer(hwnd, kUpdateConfirmTimerId, kUpdateConfirmMs, nullptr);
  }

  MSG msg{};
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    // The router yields every Settings key, including Escape, to XAML.
    if (handle_app_key(&app, msg)) continue;
    if (app.chrome.pre_translate(&msg)) continue;
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);

    // When the island is attached it borrows the session and drains. Two
    // drainers race (docs/design/12 PR 4). --no-chrome keeps the native drain.
    if (!app.chrome.filmstrip_attached()) {
      mv_completion completions[64];
      while (const uint32_t n = mv_completion_drain(app.session, completions, 64)) {
        for (uint32_t i = 0; i < n; ++i) {
          MV_LOG_INFO("completion: kind=%u job=%llu status=%s payload=%lld",
                      completions[i].kind,
                      static_cast<unsigned long long>(completions[i].job_id),
                      mv_status_name(static_cast<mv_status>(completions[i].status)),
                      static_cast<long long>(completions[i].payload));
        }
        if (n < 64) break;
      }
    }
  }

  // Quit never waits on an add-on (addon/host.h "Quit"): Import starts to
  // stop now, alongside the teardown below; the AI pack is left running, and
  // nothing is unloaded. Before 2026-09-27 the loaded set was torn down by
  // static destruction after main returned, joining a pack's model load or
  // inference batch (seconds), and could free it under a chrome read.
  (void)mv_addon_quit();
  const ULONGLONG addons_quit_at = ::GetTickCount64();

  // PR 12: writes the user asked for and has not seen land — a rating inside
  // its 250 ms debounce, a comment queued behind another write, a rotation
  // inside its own debounce. The window is gone, so nothing waits on them now:
  // take them, let the pool finish the write it is running (a second write to
  // the same file must not overlap it), then write them here, in order. The
  // rotation goes first: it refuses bytes a metadata write has changed, while
  // a metadata write applies to whatever orientation it finds. A rotation
  // already in flight is not repeated (a turn is relative; a rating is not).
  {
    const std::optional<mv::shell::rotation_write> turn = app.edits.take_pending_write();
    const std::vector<mv::shell::meta_job> meta_jobs = app.meta_writer.drain_for_exit();
    if (turn || !meta_jobs.empty()) {
      app.jobs.shutdown();
      if (turn && !mv::shell::run_rotation_write(*turn)) MV_LOG_WARN("exit: rotation write failed");
      for (const mv::shell::meta_job& job : meta_jobs) {
        const mv::shell::meta_outcome out = mv::shell::run_meta_job(job);
        if (!out.ok) MV_LOG_WARN("exit: metadata write failed: %s", mv::status_name(out.error));  // never the path
      }
    }
  }

  // The window is gone: a job finishing now posts to nobody and frees its own
  // result. Queued-but-unstarted jobs are dropped with the process.
  // Exit may wait briefly for the last settings snapshot to reach disk (WM_CLOSE
  // has already torn the islands down in its own order); the render loop never
  // waits on it.
  // PR 8 updater, exit path (the window is gone): an orderly exit of a version
  // whose chrome attached is a successful start; then any staged update is
  // handed to Update.exe to apply after this process exits (no restart).
  // Not `!g_start_confirmed`: that flag only says the confirm was STARTED, and
  // the worker that does the write is detached. Re-running it here when the
  // worker has not reported done is one small file write and is idempotent -
  // confirm_started returns early unless [trial] still names this version.
  if (g_install.installed() && !g_start_confirm_done.load(std::memory_order_acquire) &&
      (!app.chrome_enabled || app.chrome.loaded())) {
    g_start_confirmed = true;
    mv::shell::update::confirm_started(g_install);
    g_start_confirm_done.store(true, std::memory_order_release);
  }
  app.chrome.updater_exit();
  if (!mv::shell::app_settings().flush(1000)) {
    MV_LOG_WARN("settings: last change did not reach settings.ini before exit");
  }
  mv::shell::app_settings().stop();
  app.files.stop();
  app.lab.stop();
  app.jobs.shutdown();
  const int code = app.lab.exit_code();
  // Whatever is left of half a second since mv_addon_quit (an idle add-on
  // stops in well under that; a model load or a copy step can take seconds).
  constexpr ULONGLONG kQuitAddonBudgetMs = 500;
  const ULONGLONG quit_spent = ::GetTickCount64() - addons_quit_at;
  const bool addons_stopped =
      mv_addon_quit_wait(quit_spent >= kQuitAddonBudgetMs
                             ? 0u
                             : static_cast<uint32_t>(kQuitAddonBudgetMs - quit_spent)) == MV_OK;

  mv_session_release(app.session);
  mv::trace::provider_unregister();
  if (!addons_stopped) {
    // An add-on thread may still be running (the AI pack always is): static
    // destructors and DLL detach must not run under it. Everything this
    // process had to write has been written above.
    std::fflush(nullptr);
    ::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(code));
  }
  return code;
}
