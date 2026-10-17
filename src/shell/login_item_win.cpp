// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #452: start at sign-in (login_item_win.h).
#include "shell/login_item_win.h"

#include <windows.h>

#include <vector>

namespace mv::shell::login_item {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValue[] = L"MediaViewer";

// The value as it is now; empty when there is none.
std::wstring read_value() {
  DWORD bytes = 0;
  if (::RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValue, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) !=
          ERROR_SUCCESS ||
      bytes < sizeof(wchar_t)) {
    return {};
  }
  std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 1, L'\0');
  DWORD got = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
  if (::RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValue, RRF_RT_REG_SZ, nullptr, buf.data(), &got) !=
      ERROR_SUCCESS) {
    return {};
  }
  return std::wstring(buf.data());
}

}  // namespace

std::wstring command_for(std::wstring_view exe) {
  std::wstring out;
  out.reserve(exe.size() + 16);
  out += L'"';
  out += exe;
  out += L"\" --background";
  return out;
}

bool set(bool on, std::wstring_view exe) noexcept {
  try {
    if (!on) {
      const LSTATUS s = ::RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kValue);
      return s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND;
    }
    if (exe.empty()) return false;
    const std::wstring want = command_for(exe);
    if (read_value() == want) return true;
    return ::RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kValue, REG_SZ, want.c_str(),
                             static_cast<DWORD>((want.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
  } catch (...) {
    return false;
  }
}

}  // namespace mv::shell::login_item
