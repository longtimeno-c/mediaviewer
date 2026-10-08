// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// LoadLibraryExW half of the add-on loader. The add-on's own folder is on the
// search path for its dependencies and the current directory is not: a DLL
// planted beside a photo can never be picked up.
#include <windows.h>

#include <string>

#include "addon/host.h"

namespace mv::addon {

namespace {

thread_local std::string t_load_error;

// LoadLibrary's error, as a person can act on it. No path: rule 6.
std::string load_reason(DWORD code) {
  switch (code) {
    case ERROR_MOD_NOT_FOUND:
      return "Windows could not load it: a file it needs is missing (error 126). "
             "Installing the latest Microsoft Visual C++ Redistributable (x64) usually fixes this.";
    case ERROR_PROC_NOT_FOUND:
      return "Windows could not load it: this version of Windows lacks a function it needs (error 127). "
             "Update Windows, then try again.";
    case ERROR_BAD_EXE_FORMAT:
      return "Windows could not load it: it was built for another kind of processor (error 193).";
    case ERROR_DLL_INIT_FAILED:
      return "Windows could not start it (error 1114). Restart the PC, then try again.";
    case ERROR_ACCESS_DENIED:
      return "Windows refused to load it (access denied). Security software may be blocking it.";
    case ERROR_VIRUS_INFECTED:
    case ERROR_VIRUS_DELETED:
      return "Security software blocked it.";
    default:
      return "Windows could not load it (error " + std::to_string(code) + ").";
  }
}

}  // namespace

const std::string& shared_library::last_error() noexcept { return t_load_error; }

result<shared_library> shared_library::open(const std::string& utf8_path) {
  t_load_error.clear();
  if (utf8_path.empty()) return err(status::invalid_arg);
  const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path.data(),
                                      static_cast<int>(utf8_path.size()), nullptr, 0);
  if (n <= 0) return err(status::invalid_arg);
  std::wstring path(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path.data(),
                        static_cast<int>(utf8_path.size()), path.data(), n);
  HMODULE h = ::LoadLibraryExW(path.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!h) {
    t_load_error = load_reason(::GetLastError());
    return err(status::io);
  }
  shared_library lib;
  lib.handle_ = h;
  return lib;
}

void* shared_library::symbol(const char* name) const noexcept {
  return handle_ ? reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(handle_), name))
                 : nullptr;
}

void shared_library::close() noexcept {
  if (handle_) ::FreeLibrary(static_cast<HMODULE>(handle_));
  handle_ = nullptr;
}

}  // namespace mv::addon
