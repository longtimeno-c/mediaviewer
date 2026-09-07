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

#include "io/dir.h"

#include <cmath>
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
#include "shell/present_lab.h"
#include "shell/settings.h"

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

struct app_state {
  present_lab lab;
  input_snapshot input;
  mv_session_t session = nullptr;
  bool tracking_mouse = false;
  bool chrome_enabled = true;
  bool chrome_on_screen = false;  // reserved bar height; cleared if attach fails
  open_mode mode = open_mode::none;
  bool gallery_visible = false;
  mv::shell::view_settings settings;
  HWND window = nullptr;
  mv::shell::chrome_host chrome;
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

void open_folder(app_state* app, std::wstring_view wide_dir, std::wstring_view wide_select) {
  if (!app || !app->session || wide_dir.empty()) return;
  const std::string dir = utf8_from_wide(wide_dir);
  if (dir.empty()) return;
  const std::string select = utf8_from_wide(wide_select);
  uint64_t job_id = 0;
  (void)mv_folder_open(app->session, dir.c_str(), select.empty() ? nullptr : select.c_str(),
                       &job_id);
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

void open_image(app_state* app, std::wstring_view wide_path) { open_path(app, wide_path); }

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
  ofn.lpstrFilter = L"Images (JPEG, PNG, BMP)\0*.jpg;*.jpeg;*.png;*.bmp\0All files\0*.*\0";
  ofn.nFilterIndex = 1;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
  if (::GetOpenFileNameW(&ofn)) open_path(app, file);
}

void open_folder_dialog(app_state* app, HWND hwnd) {
  std::wstring folder;
  if (pick_folder(hwnd, folder)) open_folder(app, folder, {});
}

void folder_select(app_state* app, std::uint32_t index) {
  if (!app || !app->session) return;
  uint64_t job = 0;
  if (mv_folder_select(app->session, index, &job) == MV_OK) {
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
  const int next = static_cast<int>(selected) + delta;
  if (next < 0 || next >= static_cast<int>(count)) return;
  folder_select(app, static_cast<std::uint32_t>(next));
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
  if (!app) return;
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
    case mv::shell::chrome_cmd_gallery_activate:
      folder_select(app, static_cast<std::uint32_t>(arg));
      set_gallery(app, false);
      return;
    case mv::shell::chrome_cmd_toggle_filmstrip:
      toggle_filmstrip_setting(app);
      return;
    case mv::shell::chrome_cmd_set_settings:
      app->settings = mv::shell::view_settings::from_flags(static_cast<std::int32_t>(arg));
      mv::shell::save_view_settings(app->settings);
      app->chrome.apply_settings(app->settings.flags());
      apply_view_state(app);
      return;
    case mv::shell::chrome_cmd_folder_ready:
      // The island owns the completion drain (plan/12 2026-09-07), so this is
      // how the native side learns that a listing landed.
      apply_view_state(app);
      return;
    default: return;
  }
  ++app->input.activity_seq;
  publish(app);
}

// App-level keys, even when the XAML island has focus. F3 was landing in the
// command bar and the overlay sat under it — both looked like "F does nothing".
bool handle_app_key(app_state* app, const MSG& msg) noexcept {
  if (!app) return false;
  if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) return false;
  const bool repeat = (msg.lParam & (1 << 30)) != 0;
  const bool arrow = msg.wParam == VK_LEFT || msg.wParam == VK_RIGHT;
  // Ignore typematic repeats except Left/Right, which should walk the folder
  // while the key is held.
  if (repeat && !arrow) return false;

  switch (msg.wParam) {
    case VK_F3:
    case 'F':
      ++app->input.toggle_overlay_seq;
      break;
    case VK_SPACE:
      ++app->input.toggle_animation_seq;
      break;
    case 'R':
      ++app->input.reset_stats_seq;
      break;
    case 'G':
      set_gallery(app, !app->gallery_visible);
      return true;
    case 'T':
      toggle_filmstrip_setting(app);
      return true;
    case '0':
      ++app->input.fit_seq;
      break;
    case '1':
      ++app->input.one_to_one_seq;
      break;
    case VK_OEM_PLUS:
    case VK_ADD:
      ++app->input.zoom_in_seq;
      break;
    case VK_OEM_MINUS:
    case VK_SUBTRACT:
      ++app->input.zoom_out_seq;
      break;
    case 'O':
      if (::GetKeyState(VK_CONTROL) & 0x8000) {
        if (app->window) {
          if (::GetKeyState(VK_SHIFT) & 0x8000) open_folder_dialog(app, app->window);
          else open_file_dialog(app, app->window);
        }
        return true;
      }
      return false;
    case VK_LEFT:
      folder_step(app, -1);
      return true;
    case VK_RIGHT:
      folder_step(app, 1);
      return true;
    case VK_ESCAPE:
      // Esc leaves the gallery before it leaves the app. Closing the window
      // out from under someone who was only backing out of the grid is the
      // kind of thing you do exactly once.
      if (app->gallery_visible) {
        set_gallery(app, false);
        return true;
      }
      if (app->window) ::PostMessageW(app->window, WM_CLOSE, 0, 0);
      return true;
    default:
      return false;
  }
  ++app->input.activity_seq;
  publish(app);
  return true;
}

void layout_chrome(app_state* app) noexcept {
  if (!app || !app->window || !app->chrome.attached()) return;
  RECT rc{};
  ::GetClientRect(app->window, &rc);
  const auto dpi = ::GetDpiForWindow(app->window);
  const int bar = mv::shell::chrome_bar_height_px(dpi);
  const int width = rc.right - rc.left;
  const int height = rc.bottom - rc.top;
  app->chrome.resize(width, bar, dpi);
  if (app->chrome.filmstrip_attached()) app->chrome.resize_filmstrip(width, height, dpi);
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
  (void)app->chrome.attach_gallery(app->window, app, &chrome_on_command, app->session,
                                   rc.right - rc.left, height, dpi);
  app->chrome.apply_settings(app->settings.flags());
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
      app->chrome_on_screen ? static_cast<std::uint32_t>(mv::shell::chrome_bar_height_px(dpi)) : 0;
  app->input.chrome_bottom_px =
      app->chrome.filmstrip_visible()
          ? static_cast<std::uint32_t>(mv::shell::chrome_filmstrip_height_px(dpi))
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

  const bool have_folder = folder_count(app) > 1;
  if (!have_folder) app->gallery_visible = false;

  const bool want_filmstrip =
      have_folder && !app->gallery_visible &&
      (app->mode == open_mode::image ? app->settings.filmstrip_for_image
       : app->mode == open_mode::folder ? app->settings.filmstrip_for_folder
                                        : false);
  if (want_filmstrip != app->chrome.filmstrip_visible()) {
    app->chrome.show_filmstrip(want_filmstrip, width, height, dpi);
  }
  if (app->gallery_visible != app->chrome.gallery_visible()) {
    app->chrome.show_gallery(app->gallery_visible, width, height, dpi);
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
      publish(app);
      return 0;
    }

    case WM_MOUSEMOVE: {
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
      if (down) ::SetCapture(hwnd);
      else if (!app->input.mouse_down[0] && !app->input.mouse_down[1] &&
               !app->input.mouse_down[2]) {
        ::ReleaseCapture();
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
      if (wparam == VK_TAB && app->chrome.attached()) {
        const bool reverse = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
        (void)app->chrome.navigate_focus(reverse);
      }
      return 0;
    }

    case WM_DROPFILES: {
      auto drop = reinterpret_cast<HDROP>(wparam);
      wchar_t path[MAX_PATH]{};
      if (::DragQueryFileW(drop, 0, path, MAX_PATH) > 0) open_image(app, path);
      ::DragFinish(drop);
      return 0;
    }

    case WM_ERASEBKGND:
      return 1;  // the swapchain owns every pixel; never let GDI flash over it

    case WM_CLOSE:
      ::DestroyWindow(hwnd);
      return 0;

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

bool parse_options(lab_options& options, std::wstring& open_path, bool& chrome_enabled,
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
    } else if (arg == L"--json") {
      next(options.json_report_path);
    } else if (arg == L"--gate") {
      options.gate_exit_code = true;
    } else if (arg == L"--no-overlay") {
      options.overlay_visible = false;
    } else if (arg == L"--static") {
      static_requested = true;
    } else if (arg == L"--open") {
      next(open_path);
    } else if (arg == L"--no-chrome") {
      chrome_enabled = false;
    } else if (!arg.empty() && arg[0] != L'-') {
      open_path = std::wstring(arg);
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
  std::wstring open_path;
  bool chrome_enabled = true;
  if (!parse_options(options, open_path, chrome_enabled, parse_error)) {
    ::MessageBoxW(nullptr, parse_error.c_str(), kWindowTitle, MB_ICONERROR | MB_OK);
    return 2;
  }

  mv::trace::provider_register();

  // The ABI round-trip, exercised from the native side as well as from C#: the
  // shell is a client of the core through exactly the same header the managed
  // interop uses. If the shell ever reaches around the ABI, the two-language
  // boundary stops being tested by the thing that matters most.
  app_state app;
  app.chrome_enabled = chrome_enabled;
  app.settings = mv::shell::load_view_settings();
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
  if (!open_path.empty()) open_image(&app, open_path);

  MSG msg{};
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
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

  app.lab.stop();
  const int code = app.lab.exit_code();

  mv_session_release(app.session);
  mv::trace::provider_unregister();
  return code;
}
