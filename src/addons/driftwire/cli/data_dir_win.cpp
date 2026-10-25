// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// data_dir.h on Windows: %LOCALAPPDATA%\Driftwire (per user, its ACL from the
// profile: this user and the system only), and the command line from
// GetCommandLineW.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>

#include "addons/driftwire/cli/data_dir.h"

#include <cstdio>
#include <cwchar>

namespace mv::driftwire::cli {

namespace {

std::string to_utf8(const wchar_t* w, std::size_t n) {
  if (!w || n == 0) return {};
  const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(bytes > 0 ? bytes : 0), '\0');
  if (bytes > 0) ::WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(n), out.data(), bytes, nullptr, nullptr);
  return out;
}

std::wstring known_folder(REFKNOWNFOLDERID id) {
  PWSTR wide = nullptr;
  std::wstring out;
  if (SUCCEEDED(::SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &wide)) && wide) out = wide;
  ::CoTaskMemFree(wide);
  return out;
}

}  // namespace

std::string home_dir() {
  const std::wstring w = known_folder(FOLDERID_Profile);
  return to_utf8(w.c_str(), w.size());
}

std::string data_dir() {
  std::wstring dir = known_folder(FOLDERID_LocalAppData);
  if (dir.empty()) return {};
  dir += L"\\Driftwire";
  if (!::CreateDirectoryW(dir.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) return {};
  const DWORD attrs = ::GetFileAttributesW(dir.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) return {};
  return to_utf8(dir.c_str(), dir.size());
}

std::vector<std::string> utf8_args(int argc, char** argv) {
  std::vector<std::string> out;
  int n = 0;
  LPWSTR* w = ::CommandLineToArgvW(::GetCommandLineW(), &n);
  if (!w) {
    for (int i = 1; i < argc; ++i) out.emplace_back(argv[i]);
    return out;
  }
  for (int i = 1; i < n; ++i) out.push_back(to_utf8(w[i], std::wcslen(w[i])));
  ::LocalFree(w);
  return out;
}


void print(std::string_view utf8, bool to_stderr) {
  const HANDLE h = ::GetStdHandle(to_stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  if (h && h != INVALID_HANDLE_VALUE && ::GetConsoleMode(h, &mode)) {
    // The console: UTF-16, whatever its code page.
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) return;
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
    DWORD done = 0;
    ::WriteConsoleW(h, w.data(), static_cast<DWORD>(w.size()), &done, nullptr);
    return;
  }
  // A pipe or a file: the bytes as they are.
  std::FILE* f = to_stderr ? stderr : stdout;
  std::fwrite(utf8.data(), 1, utf8.size(), f);
  std::fflush(f);
}

}  // namespace mv::driftwire::cli
