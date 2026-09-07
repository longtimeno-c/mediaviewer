// SPDX-License-Identifier: GPL-2.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <windows.h>
#include <objbase.h>

#include "shell/chrome_host.h"
#include "shell/settings.h"

TEST_CASE("chrome attach args stay 40 bytes") {
  REQUIRE(sizeof(mv::shell::chrome_attach_args) == 40);
  REQUIRE(sizeof(mv::shell::chrome_resize_args) == 16);
  REQUIRE(sizeof(mv::shell::chrome_filmstrip_args) == 48);
  REQUIRE(sizeof(mv::shell::chrome_show_args) == 16);
  REQUIRE(sizeof(mv::shell::chrome_flags_args) == 8);
}

TEST_CASE("view settings round-trip through the flag word") {
  mv::shell::view_settings settings;
  REQUIRE(settings.filmstrip_for_folder);
  REQUIRE_FALSE(settings.filmstrip_for_image);
  REQUIRE(settings.flags() == mv::shell::kSettingFilmstripFolder);

  settings.filmstrip_for_image = true;
  const auto round = mv::shell::view_settings::from_flags(settings.flags());
  REQUIRE(round.filmstrip_for_folder);
  REQUIRE(round.filmstrip_for_image);
  REQUIRE(mv::shell::view_settings::from_flags(0).flags() == 0);
}

TEST_CASE("chrome bar height is 48 DIP") {
  REQUIRE(mv::shell::chrome_bar_height_px(96) == 48);
  REQUIRE(mv::shell::chrome_bar_height_px(120) == 60);
  REQUIRE(mv::shell::chrome_bar_height_px(144) == 72);
  REQUIRE(mv::shell::chrome_bar_height_px(0) == 48);
}

TEST_CASE("chrome host loads hostfxr and the blittable size") {
  mv::shell::chrome_host host;
  auto loaded = host.load();
  REQUIRE(loaded);
  REQUIRE(host.loaded());
  REQUIRE(host.probe() == 40);
  REQUIRE_FALSE(host.attached());
}

TEST_CASE("chrome host attaches and detaches an island on an hwnd") {
  mv::shell::chrome_host host;
  auto loaded = host.load();
  REQUIRE(loaded);

  (void)::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.lpszClassName = L"MediaViewer.ChromeHostTest";
  (void)::RegisterClassExW(&wc);

  HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                0, 0, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
  REQUIRE(hwnd != nullptr);
  ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);

  auto attached = host.attach(hwnd, nullptr, nullptr, 640, 48, 96);
  REQUIRE(attached);
  REQUIRE(host.attached());
  host.resize(640, 48, 96);

  MSG msg{};
  while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }

  host.detach();
  REQUIRE_FALSE(host.attached());

  ::DestroyWindow(hwnd);
  ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
}
