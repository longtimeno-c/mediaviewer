// SPDX-License-Identifier: GPL-2.0-or-later
// MediaViewer present lab — the Win32 entry point.
//
// This is the top-level window described in plan/02-architecture.md's shell/
// module. It owns the HWND and the window procedure, publishes an input
// snapshot, and does nothing else: no file I/O, no decode, no GPU waits, and no
// blocking on the core. The render thread lives in present_lab.
//
// Under the D1 amendment this window is the app, not a scaffold. PR 3 hosts
// WinUI 3 chrome inside it as XAML content islands.

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <shlobj.h>

#include "io/dir.h"

#include <cmath>
#include <iterator>
#include <cwchar>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "abi/guard.h"
#include "core/trace.h"
#include "mediaviewer/mediaviewer.h"
#include "shell/chrome_host.h"
#include "shell/file_jobs.h"
#include "shell/key_router.h"
#include "shell/marks.h"
#include "shell/open_request.h"
#include "shell/navigation.h"
#include "shell/slideshow.h"
#include "shell/present_lab.h"
#include "shell/settings.h"
#include "shell/av_soak.h"

namespace {

using mv::shell::input_snapshot;
using mv::shell::lab_options;
using mv::shell::present_lab;

constexpr wchar_t kWindowClass[] = L"MediaViewer.PresentLab";
constexpr wchar_t kWindowTitle[] = L"MediaViewer — present lab";

// What the user asked for, which is not the same as what is on screen. A
// folder open is "browse this folder"; an image open is "show me this file",
// and the folder behind it is still listed so arrows and the gallery work
// (plan/10 PR 4 — one folder navigation model) without the strip taking a
// slice of the canvas the user did not ask to give up.
enum class open_mode { none, folder, image };

// plan/16 §Focus: in fullscreen, ↓ at fit (or the bottom hot-edge) shows the
// strips until navigation settles — this long after the last navigation.
constexpr UINT_PTR kRevealTimerId = 0x6B01;
constexpr UINT kRevealMs = 3000;
// view_fitted is the render thread's last pass; a `1` then ↓ inside one frame
// would read the old fit. Zoom commands open this window so the first ↓ after
// them pans instead of falling through.
constexpr ULONGLONG kZoomIntentMs = 250;
// plan/16 slideshow: a UI-thread tick that only decides whether to advance. It
// wakes the UI thread, never the render thread, so a still between advances is
// zero presents.
constexpr UINT_PTR kSlideshowTimerId = 0x6D01;
constexpr UINT kSlideshowTickMs = 100;
// plan/16 status line in the title bar. The tick only compares strings; the
// window text is written when it changes.
constexpr UINT_PTR kTitleTimerId = 0x6F01;
constexpr UINT kTitleTickMs = 250;

struct app_state {
  present_lab lab;
  input_snapshot input;
  mv_session_t session = nullptr;
  bool tracking_mouse = false;
  bool chrome_enabled = true;
  bool chrome_on_screen = false;  // reserved bar height; cleared if attach fails
  open_mode mode = open_mode::none;
  bool gallery_visible = false;
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
  mv::shell::view_settings settings;
  HWND window = nullptr;
  mv::shell::chrome_host chrome;
  mv::shell::key_router router;
  // Which island last reported focus (chrome_cmd_focus_changed). Only read
  // when GetFocus() is not the canvas window, so it cannot go stale there.
  mv::shell::focus_kind island_focus = mv::shell::focus_kind::command_bar;
  // plan/16 `F`: borderless on the window's monitor, chrome hidden. The
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
  bool file_drag_armed = false;
  int file_drag_x = 0;
  int file_drag_y = 0;
  std::wstring last_title;       // the status line last written to the title bar
  bool fullscreen_reveal = false;  // strips shown over a fullscreen canvas for a while
  ULONGLONG zoom_intent_tick = 0;  // GetTickCount64 of the last zoom-in style command
  // plan/16 marks, copy, move. Marks are UI-thread state keyed by path; the
  // file work runs on files' own I/O worker and reports back by message.
  mv::shell::mark_set marks;
  mv::shell::file_jobs files;
  std::vector<std::string> destinations;  // F7 / F8, most recent first
  std::uint64_t folder_token = 0;         // bumped per folder open
  // plan/16 slideshow, a mode: order and interval in `show`, advancing through
  // the same folder_select as browse.
  mv::shell::slideshow show;
  ULONGLONG show_last_advance = 0;
  bool show_entered_fullscreen = false;  // leave fullscreen again on stop
};

app_state* state_from(HWND hwnd) noexcept {
  return reinterpret_cast<app_state*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void publish(app_state* app) noexcept {
  app->lab.publish(app->input);
  app->lab.wake();
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

void apply_view_state(app_state* app) noexcept;
void layout_chrome(app_state* app) noexcept;
bool run_command(app_state* app, mv::shell::command_id command) noexcept;
void focus_canvas(app_state* app) noexcept;
void reveal_current_in_explorer(app_state* app) noexcept;
void begin_file_drag(HWND hwnd, const std::string& utf8) noexcept;
void open_dropped_wide_list(app_state* app, std::wstring_view blob) noexcept;
void persist_live_keys() noexcept;
void publish_command_table(app_state* app) noexcept;
void set_settings_open(app_state* app, bool on) noexcept;

void open_folder(app_state* app, std::wstring_view wide_dir, std::wstring_view wide_select) {
  if (!app || !app->session || wide_dir.empty()) return;
  const std::string dir = utf8_from_wide(wide_dir);
  if (dir.empty()) return;
  const std::string select = utf8_from_wide(wide_select);
  uint64_t job_id = 0;
  (void)mv_folder_open(app->session, dir.c_str(), select.empty() ? nullptr : select.c_str(),
                       &job_id);
  ++app->folder_token;
  ++app->input.activity_seq;
  publish(app);
  apply_view_state(app);
}

void open_path(app_state* app, std::wstring_view wide_path) {
  if (!app || wide_path.empty()) return;
  const std::string utf8 = utf8_from_wide(wide_path);
  if (utf8.empty()) return;
  auto dir = mv::io::is_directory(utf8);
  if (dir && dir.value()) {
    app->mode = open_mode::folder;
    app->gallery_visible = false;
    open_folder(app, wide_path, {});
    return;
  }
  app->mode = open_mode::image;
  app->gallery_visible = false;
  const auto slash = wide_path.find_last_of(L"\\/");
  if (slash == std::wstring_view::npos) {
    mv_session_bump_generation(app->session, nullptr);
    uint64_t job_id = 0;
    (void)mv_image_open(app->session, utf8.c_str(), &job_id);
    ++app->input.activity_seq;
    publish(app);
    return;
  }
  open_folder(app, wide_path.substr(0, slash), wide_path);
}

void open_media(app_state* app, std::wstring_view wide_path) { open_path(app, wide_path); }

// argv and drag-and-drop (plan/16): the first entry that exists wins — a folder
// opens, a file opens its folder with that file selected (open_request.h).
// The attribute probe is the same one-stat-per-path open_path already makes.
void open_paths(app_state* app, const std::vector<std::wstring>& raw) {
  if (!app) return;
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
  const auto request = mv::shell::resolve_open(probes);
  switch (request.kind) {
    case mv::shell::open_kind::folder:
    case mv::shell::open_kind::file:
      open_path(app, request.path);
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
  open_folder(app, folder, {});
  focus_canvas(app);
}

// The folder item's full path, UTF-8, or empty. UI thread; a copy out of the
// folder model, no I/O.
std::string item_path_at(app_state* app, std::uint32_t index) {
  if (!app || !app->session) return {};
  char stack_buf[1024];
  std::uint32_t bytes = 0;
  if (mv_folder_item_path(app->session, index, stack_buf, sizeof(stack_buf), &bytes) != MV_OK) {
    return {};
  }
  if (bytes < sizeof(stack_buf)) {
    stack_buf[sizeof(stack_buf) - 1] = '\0';
    return std::string(stack_buf);
  }
  // A long path: ask again with room for it.
  std::string out(static_cast<std::size_t>(bytes) + 1, '\0');
  if (mv_folder_item_path(app->session, index, out.data(), bytes + 1, &bytes) != MV_OK) return {};
  out.resize(std::char_traits<char>::length(out.c_str()));
  return out;
}

std::string current_item_path(app_state* app) {
  if (!app || !app->session) return {};
  std::uint32_t count = 0;
  std::uint32_t selected = 0;
  if (mv_folder_count(app->session, &count) != MV_OK || count == 0) return {};
  if (mv_folder_selected(app->session, &selected) != MV_OK || selected >= count) return {};
  return item_path_at(app, selected);
}

// plan/16 Ctrl+E: open the containing folder with this file selected, so a
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

// Shell IDataObject for the file, so Explorer / other apps receive a real
// CF_HDROP. Modal; the UI thread is inside OLE's drag loop until drop or Esc.
void begin_file_drag(HWND hwnd, const std::string& utf8) noexcept {
  if (!hwnd || utf8.empty()) return;
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
  DWORD effect = DROPEFFECT_COPY;
  (void)::SHDoDragDrop(hwnd, data, nullptr, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
  data->Release();
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

void folder_select(app_state* app, std::uint32_t index) {
  if (!app || !app->session) return;
  uint64_t job = 0;
  if (mv_folder_select(app->session, index, &job) == MV_OK) {
    refresh_item_info(app);
    refresh_mark_state(app);
    // Navigation keeps a fullscreen reveal up; it hides once this settles.
    if (app->fullscreen_reveal && app->window) {
      ::SetTimer(app->window, kRevealTimerId, kRevealMs, nullptr);
    }
    ++app->input.activity_seq;
    publish(app);
  }
}

void folder_step(app_state* app, int delta) {
  if (!app || !app->session) return;
  uint32_t count = 0;
  uint32_t selected = 0;
  if (mv_folder_count(app->session, &count) != MV_OK || count == 0) return;
  if (mv_folder_selected(app->session, &selected) != MV_OK) return;
  // plan/16: wrap at the ends when the setting is on (the default).
  const auto next = mv::shell::step_index(selected, delta, count, app->settings.wrap);
  if (!next) return;
  folder_select(app, *next);
}

// Skim, not transport: J/L are the +/-10 s jumps, Q/E are the shuttle you hold
// down to find a moment. 2 s per repeat lands about where a scrubber drag does.
constexpr std::int64_t kSkimStepNs = 2'000'000'000;
// plan/16: J / L are the +/-10 s transport jumps.
constexpr std::int64_t kTransportStepNs = 10'000'000'000;

// plan/16's Video mode: "current item is a clip, playing or paused". Stopped
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

// The gallery is only a view of a folder. One file in the directory is the
// image already on screen, so there is nothing to lay out in a grid.
bool gallery_available(app_state* app) noexcept {
  return app && app->chrome.gallery_attached() && folder_count(app) > 1;
}

void set_gallery(app_state* app, bool visible) {
  if (!app) return;
  if (visible && !gallery_available(app)) return;
  if (app->gallery_visible == visible) return;
  app->gallery_visible = visible;
  apply_view_state(app);
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
  app->chrome.apply_settings(app->settings.flags());
  apply_view_state(app);
}

void chrome_on_command(void* ctx, int command, float arg) {
  auto* app = static_cast<app_state*>(ctx);
  // WM_CLOSE pumps messages after detaching; nothing the island queued before
  // it went away may act on the app now.
  if (!app || app->closing) return;
  switch (command) {
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
      folder_select(app, index);
      // Closing the grid would otherwise reveal the previous still until the
      // new decode lands. Drop it when the click is a jump.
      if (index != cur) {
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
      app->settings = mv::shell::view_settings::from_flags(static_cast<std::int32_t>(arg));
      mv::shell::save_view_settings(app->settings);
      app->input.sticky_zoom = app->settings.sticky_zoom;
      app->input.background = app->settings.background;
      app->chrome.apply_settings(app->settings.flags());
      apply_view_state(app);
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
      const bool on = arg != 0.0f;
      if (app->video_on == on) return;
      app->video_on = on;
      // A freshly opened media_source starts at 1.00x, so the ladder and the
      // dropdown have to start there too rather than inheriting the last clip.
      if (on) apply_rate(app, kRateDefaultIndex);
      apply_view_state(app);
      return;
    }
    case mv::shell::chrome_cmd_folder_ready:
      // The island owns the completion drain (plan/12 2026-09-07), so this is
      // how the native side learns that a listing landed.
      refresh_item_info(app);
      refresh_mark_state(app);
      apply_view_state(app);
      return;
    case mv::shell::chrome_cmd_focus_changed: {
      const int kind = static_cast<int>(arg);
      if (kind >= static_cast<int>(mv::shell::focus_kind::command_bar) &&
          kind <= static_cast<int>(mv::shell::focus_kind::text)) {
        app->island_focus = static_cast<mv::shell::focus_kind>(kind);
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
          !mv::shell::is_reserved_notification(command)) {
        (void)run_command(app, static_cast<mv::shell::command_id>(command));
      }
      return;
  }
  ++app->input.activity_seq;
  publish(app);
}

// plan/16: one router. Symbol keys are resolved through the active layout so
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
      } else if (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE) {
        k = key::none;  // numpad digits are ratings (PR 11)
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
}

void publish_slideshow(app_state* app) noexcept {
  app->input.blackout = app->show.active() && app->show.blackout();
  ++app->input.activity_seq;
  publish(app);
}

// F5 (plan/16). Fullscreen unless it already is; leaving puts it back.
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
// whichever is later (plan/16). A still, or a paused clip, goes on the
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
  const auto next = app->show.next(selected, app->settings.wrap);  // plan/16 wrap setting
  app->show_last_advance = now;
  if (!next) {
    stop_slideshow(app);
    return;
  }
  folder_select(app, *next);
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
    app->gallery_visible = false;
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
    case back_target::canvas_focus:
      focus_canvas(app);
      return;
    case back_target::gallery:
      set_gallery(app, false);
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
    case back_target::slideshow:
      stop_slideshow(app);
      return;
    case back_target::fullscreen:
      set_fullscreen(app, false);
      return;
    // Slideshow, pane and crop land with their slices (6d, PR 8, PR 9);
    // resolve_back cannot name them until their state exists.
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

// plan/16 "Status / title": name — i/N — W×H — zoom %. From the folder model
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
  auto targets = app->marks.targets(current_item_path(app));
  if (targets.empty()) return false;
  std::string dest;
  if (!pick && !app->destinations.empty()) dest = app->destinations.front();
  if (dest.empty()) {
    std::wstring folder;
    if (!pick_folder(app->window, folder)) return true;  // cancelled: nothing to do
    dest = utf8_from_wide(folder);
    if (dest.empty()) return true;
  }
  // Written only when it changes: it is an INI write on the UI thread (plan/12
  // "Settings writes on the UI thread").
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

// Delete: always the Recycle Bin, always asked first (plan/16). A location
// with no bin is refused on the worker, never deleted permanently.
bool start_recycle(app_state* app) {
  if (!app || !app->window) return false;
  auto targets = app->marks.targets(current_item_path(app));
  if (targets.empty()) return false;
  // One item: show its name (never the folder). This is the user's own screen;
  // confirming a delete without seeing what goes is the trap. Several: a count.
  std::wstring text;
  if (targets.size() == 1) {
    const std::string& path = targets.front();
    const auto slash = path.find_last_of("\\/");
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    std::wstring wide(name.size(), L'\0');
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()),
                                        wide.data(), static_cast<int>(wide.size()));
    wide.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
    text = L"Move “" + wide + L"” to the Recycle Bin?";
  } else {
    wchar_t count[96]{};
    (void)::swprintf_s(count, L"Move %zu marked items to the Recycle Bin?", targets.size());
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

// Command effects. A switch over a dense enum is the jump table plan/16 asks
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
    case close_window:
      if (app->window) ::PostMessageW(app->window, WM_CLOSE, 0, 0);
      return true;
    case prev: folder_step(app, -1); return true;
    case next: folder_step(app, 1); return true;
    case first: folder_jump(app, -(1LL << 32)); return true;
    case last: folder_jump(app, 1LL << 32); return true;
    case skip_back: folder_jump(app, -10); return true;
    case skip_forward: folder_jump(app, 10); return true;
    case toggle_gallery: set_gallery(app, !app->gallery_visible); return true;
    case gallery_open_selected:
      if (!app->gallery_visible) return false;
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
      std::uint32_t selected = 0;
      if (mv_folder_selected(app->session, &selected) == MV_OK) {
        app->chrome.navigate_gallery(command == gallery_up ? -1 : 1,
                                     static_cast<std::int32_t>(selected));
      }
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
    case overlay: return bump(app->input.toggle_overlay_seq);
    case reset_stats: return bump(app->input.reset_stats_seq);

    case play_pause: {
      // plan/16: on an animation Space plays and pauses it, like a clip.
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
    // plan/16: J / L are -10 s / +10 s, and a jump is not part of a skim burst.
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
    // it on tap; they skip instead (plan/12 2026-09-13).
    case rate_down:
    case rate_up:
      if (!video_mode(app)) return false;
      apply_rate(app, app->rate_index + (command == rate_up ? 1 : -1));
      return true;
    // Q/E: tap is one exact ±2 s skip; hold shuttles on the non-exact seek
    // (nearest keyframe) so a held key cannot queue a decode-forward per
    // repeat (plan/16 speed rule 1). A new burst re-reads the position.
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
    // plan/16: Ctrl+0 resets pan/zoom, which is the opening view — fit.
    case reset_view:
      app->zoom_intent_tick = 0;
      return bump(app->input.fit_seq);
    // plan/16: pan only when zoomed. At fit the view is locked, so the key is
    // not ours and falls through to whatever else wants it.
    case pan_up:
    case pan_down:
    case pan_left:
    case pan_right: {
      const bool zooming = app->zoom_intent_tick != 0 &&
                           ::GetTickCount64() - app->zoom_intent_tick < kZoomIntentMs;
      if (app->lab.view_fitted() && !zooming) {
        // plan/16 §Focus: fullscreen hides the strips, and ↓ at fit is how a
        // keyboard user gets them (and a clip's transport) back.
        if (command == pan_down && app->fullscreen) {
          set_fullscreen_reveal(app, true);
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
      app->input.background = static_cast<std::uint8_t>((app->input.background + 1) % 4);
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
      return set_level(app);
    // Marks (plan/16): a set separate from the selection, keyed by path.
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

    // Slideshow (plan/16): a mode, no transition pass.
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

    // plan/16 `?`, Ctrl+G go-to and `/` find: XAML flyouts on the command bar.
    case help:
    case go_to:
    case typeahead: {
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
      const auto modes =
          static_cast<std::int32_t>(mv::shell::mask_of(mv::shell::resolve_mode(underneath)));
      app->popup_open = true;
      if (app->fullscreen) layout_chrome(app);  // the flyouts hang off the bar
      app->chrome.show_popup(kind, modes);
      // Do not SetFocus the canvas here: a WinUI Flyout light-dismisses, which
      // is why `?` opened and immediately vanished. Letter keys still route
      // while it is up (command-bar focus is not island mode).
      return true;
    }
    // plan/12 2026-09-13: the tree island lands in PR 8. Its command, key and
    // chrome_left_px layout are here so the island maths is not retrofitted.
    case folder_tree:
      MV_LOG_INFO("folder tree: lands in PR 8 (plan/12 2026-09-13)");
      ::MessageBeep(MB_OK);
      return true;

    // Host-side and cheap (plan/16): photographers park the viewer on a
    // second monitor.
    case always_on_top:
      if (!app->window) return false;
      app->topmost = !app->topmost;
      ::SetWindowPos(app->window, app->topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
      return true;

    default:
      return false;
  }
}

// Runs before the island's pre-translate, so F3 and friends work with the
// command bar focused. The router decides what a focused island keeps.
bool handle_app_key(app_state* app, const MSG& msg) noexcept {
  // Mid-teardown a key must not open a dialog or touch a detached island.
  if (!app || app->closing) return false;
  const bool is_down = msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN;
  const bool is_up = msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP;
  if (!is_down && !is_up) return false;
  const auto event = translate_key(msg, is_up);
  if (event.k == mv::shell::key::none) return false;
  const auto routed = app->router.on_key(event, view_state_of(app));
  if (!routed.handled) return false;
  if (routed.command == mv::shell::command_id::back) {
    walk_back(app, routed.back);
    return true;
  }
  if (routed.command == mv::shell::command_id::none) return true;
  return run_command(app, routed.command);
}

void layout_chrome(app_state* app) noexcept {
  if (!app || !app->window || !app->chrome.attached()) return;
  RECT rc{};
  ::GetClientRect(app->window, &rc);
  const auto dpi = ::GetDpiForWindow(app->window);
  const int bar = mv::shell::chrome_bar_height_px(dpi);
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
}

bool attach_chrome(app_state* app) {
  if (!app || !app->chrome_enabled || !app->window) return false;
  if (auto loaded = app->chrome.load(); !loaded) return false;
  RECT rc{};
  ::GetClientRect(app->window, &rc);
  const auto dpi = ::GetDpiForWindow(app->window);
  const int bar = mv::shell::chrome_bar_height_px(dpi);
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
  app->chrome.apply_settings(app->settings.flags());
  // `?` and the palette read the same static table as the router (plan/16).
  publish_command_table(app);
  app->chrome.refresh_island_windows();
  return true;
}

void update_client_metrics(app_state* app, HWND hwnd) noexcept {
  RECT rc{};
  ::GetClientRect(hwnd, &rc);
  app->input.width = static_cast<std::uint32_t>(rc.right - rc.left);
  app->input.height = static_cast<std::uint32_t>(rc.bottom - rc.top);
  const auto dpi = ::GetDpiForWindow(hwnd);
  app->input.dpi_scale = static_cast<float>(dpi) / 96.0f;
  app->input.chrome_height_px =
      (app->chrome_on_screen && !app->fullscreen)
          ? static_cast<std::uint32_t>(mv::shell::chrome_bar_height_px(dpi))
          : 0;
  // Both bottom strips reserve canvas. The transport is only ever up while a
  // clip is playing or paused, and reserving is what keeps it off the video.
  int bottom = 0;
  if (app->chrome.filmstrip_visible()) bottom += mv::shell::chrome_filmstrip_height_px(dpi);
  if (app->chrome.transport_visible()) bottom += mv::shell::chrome_transport_height_px(dpi);
  app->input.chrome_bottom_px = static_cast<std::uint32_t>(bottom);
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

  const bool have_folder = folder_count(app) > 1;
  if (!have_folder) app->gallery_visible = false;

  // Fullscreen hides chrome (plan/16) unless ↓ or the hot-edge revealed it.
  const bool chrome_hidden = app->fullscreen && !app->fullscreen_reveal;
  const bool settings = app->settings_open;
  const bool want_filmstrip =
      have_folder && !app->gallery_visible && !chrome_hidden && !settings &&
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
  const bool want_transport =
      app->video_on && !app->gallery_visible && !chrome_hidden && !settings;
  const int strip = app->chrome.filmstrip_visible()
                        ? mv::shell::chrome_filmstrip_height_px(dpi) : 0;
  if (want_transport != app->chrome.transport_visible()) {
    app->chrome.show_transport(want_transport, width, height, strip, dpi);
  } else if (want_transport) {
    app->chrome.resize_transport(width, height, strip, dpi);
  }
  update_client_metrics(app, app->window);
  ++app->input.resize_seq;
  ++app->input.activity_seq;
  publish(app);
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
      // DPI change costs nothing but a resize (plan/03).
      const auto* suggested = reinterpret_cast<const RECT*>(lparam);
      ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
      update_client_metrics(app, hwnd);
      layout_chrome(app);
      ++app->input.resize_seq;
      publish(app);
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
      app->input.window_active = LOWORD(wparam) != WA_INACTIVE;
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
      if (!app->input.mouse_in_client ||
          app->input.mouse_x != static_cast<float>(GET_X_LPARAM(lparam)) ||
          app->input.mouse_y != static_cast<float>(GET_Y_LPARAM(lparam))) {
        ++app->input.activity_seq;
      }
      app->input.mouse_x = static_cast<float>(GET_X_LPARAM(lparam));
      app->input.mouse_y = static_cast<float>(GET_Y_LPARAM(lparam));
      app->input.mouse_in_client = true;
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
          begin_file_drag(hwnd, current_item_path(app));
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
      publish(app);
      return 0;
    }

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
        if (msg == WM_LBUTTONDOWN && app->lab.view_fitted()) {
          app->file_drag_armed = true;
          app->file_drag_x = GET_X_LPARAM(lparam);
          app->file_drag_y = GET_Y_LPARAM(lparam);
        }
      }
      else if (!app->input.mouse_down[0] && !app->input.mouse_down[1] &&
               !app->input.mouse_down[2]) {
        ::ReleaseCapture();
        app->file_drag_armed = false;
      }
      publish(app);
      return 0;
    }

    case WM_MOUSEWHEEL: {
      app->input.wheel_total += GET_WHEEL_DELTA_WPARAM(wparam);
      ++app->input.activity_seq;
      publish(app);
      return 0;
    }

    case WM_KEYDOWN: {
      if ((lparam & (1 << 30)) != 0) return 0;
      // Tab has nowhere visible to go while fullscreen hides the chrome.
      if (wparam == VK_TAB && app->chrome.attached() && !app->fullscreen) {
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
      std::wstring_view blob(w, n);
      if (!blob.empty() && blob.back() == L'\0') blob.remove_suffix(1);
      open_dropped_wide_list(app, blob);
      return 1;
    }

    case WM_DROPFILES: {
      // Every dropped entry, at any path length; open_paths picks the first
      // that exists (plan/16).
      auto drop = reinterpret_cast<HDROP>(wparam);
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

    case WM_TIMER:
      if (wparam == kTitleTimerId) {
        update_title(app);
        return 0;
      }
      if (wparam == kSlideshowTimerId) {
        slideshow_tick(app);
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
  // dark canvas looks broken, so it is worth the two lines (plan/09).
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
    } else if (arg == L"--open") {
      std::wstring value;
      next(value);
      if (ok) open_paths.push_back(std::move(value));
    } else if (arg == L"--no-chrome") {
      chrome_enabled = false;
    } else if (!arg.empty() && arg[0] != L'-') {
      // Every positional path: Explorer's "Open" with several files passes
      // them all. open_paths decides (plan/16).
      open_paths.emplace_back(arg);
    } else {
      error = L"unrecognised argument: " + std::wstring(arg);
      ok = false;
    }
  }
  ::LocalFree(argv);
  // Interactive: drop-target empty view. Soak: the PR 1 sweep unless --static.
  options.start_animating = options.soak_seconds > 0.0 && !static_requested;
  return ok;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show_command) {
  // PerMonitorV2 is also declared in the manifest; this is the belt to that
  // braces, because a manifest can be lost by a repackaging step and the
  // failure mode is a blurry window nobody files a bug about.
  ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  // WinUI islands require an STA. GetOpenFileName wants one too.
  (void)::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  lab_options options;
  std::wstring parse_error;
  std::vector<std::wstring> requested_paths;
  bool chrome_enabled = true;
  if (!parse_options(options, requested_paths, chrome_enabled, parse_error)) {
    ::MessageBoxW(nullptr, parse_error.c_str(), kWindowTitle, MB_ICONERROR | MB_OK);
    return 2;
  }

  if (options.av_soak_seconds) {
    const auto clip = utf8_from_wide(requested_paths.empty() ? std::wstring_view{}
                                                             : std::wstring_view{requested_paths.front()});
    const auto csv = utf8_from_wide(options.av_csv);
    return mv::shell::run_av_soak({clip.c_str(), options.av_soak_seconds, csv.c_str()});
  }
  mv::trace::provider_register();

  // The ABI round-trip, exercised from the native side as well as from C#: the
  // shell is a client of the core through exactly the same header the managed
  // interop uses. If the shell ever reaches around the ABI, the two-language
  // boundary stops being tested by the thing that matters most.
  app_state app;
  app.chrome_enabled = chrome_enabled;
  app.settings = mv::shell::load_view_settings();
  app.input.sticky_zoom = app.settings.sticky_zoom;
  app.input.background = app.settings.background;
  app.destinations = mv::shell::load_destinations();
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

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = window_proc;
  wc.hInstance = instance;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
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

  if (auto started = app.lab.start(hwnd, options); !started) {
    ::MessageBoxA(nullptr, "render thread failed to start", "MediaViewer", MB_ICONERROR | MB_OK);
    mv_session_release(app.session);
    return 2;
  }

  ::ShowWindow(hwnd, show_command);
  ::UpdateWindow(hwnd);
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

  MSG msg{};
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    // The router yields every Settings key, including Escape, to XAML.
    if (handle_app_key(&app, msg)) continue;
    if (app.chrome.pre_translate(&msg)) continue;
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);

    // When the island is attached it borrows the session and drains. Two
    // drainers race (plan/12 PR 4). --no-chrome keeps the native drain.
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

  // The window is gone: a job finishing now posts to nobody and frees its own
  // result. Queued-but-unstarted jobs are dropped with the process.
  app.files.stop();
  app.lab.stop();
  const int code = app.lab.exit_code();

  mv_session_release(app.session);
  mv::trace::provider_unregister();
  return code;
}
