// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// ort.h's platform half on Windows: LoadLibraryExW with the library's own
// folder first in the search, so onnxruntime_providers_shared.dll (and a
// vendor piece's provider DLL beside it) resolve from the pack, not PATH.
#include <windows.h>

#include <cstring>
#include <string>

#include "infer/ort.h"

namespace mv::infer::dylib {
namespace {

std::wstring wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                                    nullptr, 0);
  if (n <= 0) return {};
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), w.data(), n);
  for (wchar_t& c : w) {
    if (c == L'/') c = L'\\';
  }
  return w;
}

}  // namespace

void* open(const std::string& path_utf8) noexcept {
  try {
    const std::wstring w = wide(path_utf8);
    if (w.empty()) return nullptr;
    return LoadLibraryExW(w.c_str(), nullptr,
                          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  } catch (...) {
    return nullptr;
  }
}

void* symbol(void* handle, const char* name) noexcept {
  return handle ? reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name)) : nullptr;
}

void close(void* handle) noexcept {
  if (handle) FreeLibrary(static_cast<HMODULE>(handle));
}

std::vector<char> native_path(const std::string& path_utf8) {
  const std::wstring w = wide(path_utf8);
  std::vector<char> out((w.size() + 1) * sizeof(wchar_t), 0);
  std::memcpy(out.data(), w.c_str(), w.size() * sizeof(wchar_t));
  return out;
}

const char* library_name() noexcept { return "onnxruntime.dll"; }

}  // namespace mv::infer::dylib
