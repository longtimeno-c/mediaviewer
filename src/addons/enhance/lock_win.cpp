// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>

#include <string>

#include "addons/enhance/lock.h"

namespace mv::enhance {

file_lock::~file_lock() { ::CloseHandle(reinterpret_cast<HANDLE>(handle_)); }

namespace {

std::wstring wide_of(const std::string& path) {
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0);
  if (n <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), wide.data(), n);
  return wide;
}

}  // namespace

std::unique_ptr<file_lock> try_lock(const std::string& path) {
  const std::wstring wide = wide_of(path);
  if (wide.empty()) return nullptr;
  // No sharing: a second open fails while this handle is open, in this
  // process or another.
  HANDLE h = ::CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return nullptr;
  return std::make_unique<file_lock>(reinterpret_cast<std::intptr_t>(h));
}

bool is_held(const std::string& path) {
  const std::wstring wide = wide_of(path);
  if (wide.empty()) return false;
  // The holder shares nothing, so any open asking to read fails while it
  // has the file; this one shares everything, delete included.
  HANDLE h = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return ::GetLastError() == ERROR_SHARING_VIOLATION;
  ::CloseHandle(h);
  return false;
}

}  // namespace mv::enhance
