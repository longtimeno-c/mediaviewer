// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// LoadLibraryExW half of the add-on loader. The add-on's own folder is on the
// search path for its dependencies and the current directory is not: a DLL
// planted beside a photo can never be picked up.
#include <windows.h>

#include "addon/host.h"

namespace mv::addon {

result<shared_library> shared_library::open(const std::string& utf8_path) {
  if (utf8_path.empty()) return err(status::invalid_arg);
  const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path.data(),
                                      static_cast<int>(utf8_path.size()), nullptr, 0);
  if (n <= 0) return err(status::invalid_arg);
  std::wstring path(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path.data(),
                        static_cast<int>(utf8_path.size()), path.data(), n);
  HMODULE h = ::LoadLibraryExW(path.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!h) return err(status::corrupt);
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
