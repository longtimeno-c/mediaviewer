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
#include <shellapi.h>
#include <windowsx.h>
#include <dwmapi.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "abi/guard.h"
#include "core/trace.h"
#include "mediaviewer/mediaviewer.h"
#include "shell/present_lab.h"

namespace {

using mv::shell::input_snapshot;
using mv::shell::lab_options;
using mv::shell::present_lab;

constexpr wchar_t kWindowClass[] = L"MediaViewer.PresentLab";
constexpr wchar_t kWindowTitle[] = L"MediaViewer — present lab";

struct app_state {
  present_lab lab;
  input_snapshot input;
  mv_session_t session = nullptr;
  bool tracking_mouse = false;
};

app_state* state_from(HWND hwnd) noexcept {
  return reinterpret_cast<app_state*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void publish(app_state* app) noexcept {
  app->lab.publish(app->input);
  app->lab.wake();
  app->input.wheel = 0.0f;  // wheel is a delta; consume it once published
}

void update_client_metrics(app_state* app, HWND hwnd) noexcept {
  RECT rc{};
  ::GetClientRect(hwnd, &rc);
  app->input.width = static_cast<std::uint32_t>(rc.right - rc.left);
  app->input.height = static_cast<std::uint32_t>(rc.bottom - rc.top);
  app->input.dpi_scale = static_cast<float>(::GetDpiForWindow(hwnd)) / 96.0f;
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
      if (down) ::SetCapture(hwnd);
      else if (!app->input.mouse_down[0] && !app->input.mouse_down[1] &&
               !app->input.mouse_down[2]) {
        ::ReleaseCapture();
      }
      publish(app);
      return 0;
    }

    case WM_MOUSEWHEEL: {
      app->input.wheel +=
          static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / static_cast<float>(WHEEL_DELTA);
      publish(app);
      return 0;
    }

    case WM_KEYDOWN: {
      if ((lparam & (1 << 30)) != 0) return 0;  // ignore auto-repeat
      switch (wparam) {
        case VK_F3:     ++app->input.toggle_overlay_seq; break;
        case VK_SPACE:  ++app->input.toggle_animation_seq; break;
        case 'R':       ++app->input.reset_stats_seq; break;
        case VK_ESCAPE: ::PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
        default: return 0;
      }
      publish(app);
      return 0;
    }

    case WM_ERASEBKGND:
      return 1;  // the swapchain owns every pixel; never let GDI flash over it

    case WM_CLOSE:
      ::DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY:
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

bool parse_options(lab_options& options, std::wstring& error) {
  int argc = 0;
  LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  if (!argv) return true;

  bool ok = true;
  for (int i = 1; i < argc && ok; ++i) {
    const std::wstring_view arg{argv[i]};
    const auto next = [&](std::wstring& out) {
      if (i + 1 >= argc) { error = std::wstring(arg) + L" needs a value"; ok = false; return; }
      out = argv[++i];
    };

    if (arg == L"--soak") {
      std::wstring value;
      next(value);
      if (ok) options.soak_seconds = ::_wtof(value.c_str());
    } else if (arg == L"--json") {
      next(options.json_report_path);
    } else if (arg == L"--gate") {
      options.gate_exit_code = true;
    } else if (arg == L"--no-overlay") {
      options.overlay_visible = false;
    } else if (arg == L"--static") {
      options.start_animating = false;
    } else {
      error = L"unrecognised argument: " + std::wstring(arg);
      ok = false;
    }
  }
  ::LocalFree(argv);
  return ok;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show_command) {
  // PerMonitorV2 is also declared in the manifest; this is the belt to that
  // braces, because a manifest can be lost by a repackaging step and the
  // failure mode is a blurry window nobody files a bug about.
  ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  lab_options options;
  std::wstring parse_error;
  if (!parse_options(options, parse_error)) {
    ::MessageBoxW(nullptr, parse_error.c_str(), kWindowTitle, MB_ICONERROR | MB_OK);
    return 2;
  }

  mv::trace::provider_register();

  // The ABI round-trip, exercised from the native side as well as from C#: the
  // shell is a client of the core through exactly the same header the managed
  // interop uses. If the shell ever reaches around the ABI, the two-language
  // boundary stops being tested by the thing that matters most.
  app_state app;
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
  update_client_metrics(&app, hwnd);
  app.lab.publish(app.input);

  if (auto started = app.lab.start(hwnd, options); !started) {
    ::MessageBoxA(nullptr, "render thread failed to start", "MediaViewer", MB_ICONERROR | MB_OK);
    mv_session_release(app.session);
    return 2;
  }

  ::ShowWindow(hwnd, show_command);
  ::UpdateWindow(hwnd);

  MSG msg{};
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);

    // Drain the core's completion queue on the UI thread — the shape the C#
    // shell uses in PR 3. C++ never marshals to a dispatcher (plan/14).
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

  app.lab.stop();
  const int code = app.lab.exit_code();

  mv_session_release(app.session);
  mv::trace::provider_unregister();
  return code;
}
