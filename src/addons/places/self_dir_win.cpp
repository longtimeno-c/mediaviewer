// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>

#include "addons/places/self_dir.h"

namespace mv::places {

std::string self_dir() {
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&self_dir), &self)) {
    return {};
  }
  std::wstring path(32768, L'\0');
  const DWORD n = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
  if (n == 0 || n >= path.size()) return {};
  path.resize(n);
  const std::size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return {};
  path.resize(slash);
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0,
                                        nullptr, nullptr);
  if (bytes <= 0) return {};
  std::string out(static_cast<std::size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), out.data(), bytes, nullptr,
                      nullptr);
  return out;
}

}  // namespace mv::places
