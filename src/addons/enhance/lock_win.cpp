// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>

#include "addons/enhance/lock.h"

namespace mv::enhance {

file_lock::~file_lock() { ::CloseHandle(reinterpret_cast<HANDLE>(handle_)); }

std::unique_ptr<file_lock> try_lock(const std::string& path) {
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0);
  if (n <= 0) return nullptr;
  std::wstring wide(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), wide.data(), n);
  // No sharing: a second open fails while this handle is open, in this
  // process or another.
  HANDLE h = ::CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return nullptr;
  return std::make_unique<file_lock>(reinterpret_cast<std::intptr_t>(h));
}

}  // namespace mv::enhance
