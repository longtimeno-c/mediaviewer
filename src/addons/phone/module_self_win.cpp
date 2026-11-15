// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// module_self.h on Windows.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "addons/phone/module_self.h"

namespace mv::phone::module_self {

void pin() noexcept {
  HMODULE self = nullptr;
  (void)::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                             reinterpret_cast<LPCWSTR>(&pin), &self);
}

std::string dir() {
  HMODULE self = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&dir), &self)) {
    return {};
  }
  std::wstring buf(32768, L'\0');
  const DWORD n = ::GetModuleFileNameW(self, buf.data(), static_cast<DWORD>(buf.size()));
  if (n == 0 || n >= buf.size()) return {};
  buf.resize(n);
  const auto slash = buf.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return {};
  buf.resize(slash);
  const int len = ::WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(len > 0 ? len : 0), '\0');
  if (len > 0) (void)::WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), out.data(), len, nullptr, nullptr);
  return out;
}

std::string join(const std::string& dir, const char* name) { return dir + "\\" + name; }

std::filesystem::path fs_path(const std::string& utf8) {
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) (void)::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
  return std::filesystem::path(w);
}

std::string app_clip_helper() {
  std::wstring buf(32768, L'\0');
  const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
  if (n == 0 || n >= buf.size()) return {};
  buf.resize(n);
  const auto slash = buf.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return {};
  buf.resize(slash + 1);
  buf += L"MediaViewerClipJob.exe";
  const DWORD attrs = ::GetFileAttributesW(buf.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) return {};
  const int len = ::WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(len > 0 ? len : 0), '\0');
  if (len > 0) (void)::WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), out.data(), len, nullptr, nullptr);
  return out;
}

}  // namespace mv::phone::module_self
