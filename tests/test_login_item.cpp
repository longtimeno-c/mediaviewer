// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #452: start at sign-in (shell/login_item_win.h).
#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <optional>
#include <string>

#include "shell/login_item_win.h"

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

std::optional<std::wstring> run_value() {
  wchar_t buf[4096]{};
  DWORD bytes = sizeof buf;
  if (::RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"MediaViewer", RRF_RT_REG_SZ, nullptr, buf, &bytes) !=
      ERROR_SUCCESS) {
    return std::nullopt;
  }
  return std::wstring(buf);
}

}  // namespace

TEST_CASE("login item: the Run value quotes the exe and starts in the background", "[login-item]") {
  CHECK(mv::shell::login_item::command_for(L"C:\\Users\\a b\\AppData\\Local\\MediaViewer\\MediaViewer.exe") ==
        L"\"C:\\Users\\a b\\AppData\\Local\\MediaViewer\\MediaViewer.exe\" --background");
}

// Hidden: writes this user's real Run key, and puts back what was there.
TEST_CASE("login item: on writes the value, off removes it", "[.login-item]") {
  const std::optional<std::wstring> before = run_value();
  const std::wstring exe = L"C:\\mv-login-item-test\\MediaViewer.exe";
  REQUIRE(mv::shell::login_item::set(true, exe));
  CHECK(run_value() == mv::shell::login_item::command_for(exe));
  REQUIRE(mv::shell::login_item::set(true, exe));  // already there: unchanged
  REQUIRE(mv::shell::login_item::set(false, {}));
  CHECK_FALSE(run_value().has_value());
  CHECK(mv::shell::login_item::set(false, {}));  // nothing there is not a failure
  if (before) {
    (void)::RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, L"MediaViewer", REG_SZ, before->c_str(),
                            static_cast<DWORD>((before->size() + 1) * sizeof(wchar_t)));
  }
}
