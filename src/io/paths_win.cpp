// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/paths.h"

#include <windows.h>
#include <shlobj.h>

#include <initializer_list>
#include <mutex>
#include <string>

namespace mv::io {
namespace {

std::mutex g_mu;
std::string g_override;
std::string g_addons_override;
std::string g_snapshot_override;

std::string utf8_from_wide(const wchar_t* wide) {
  if (!wide || !wide[0]) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string out(static_cast<std::size_t>(n - 1), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), n, nullptr, nullptr);
  return out;
}

}  // namespace

void set_thumb_cache_dir_override(std::string_view utf8_dir) {
  std::lock_guard lock(g_mu);
  g_override.assign(utf8_dir);
}

result<std::string> thumb_cache_dir() {
  {
    std::lock_guard lock(g_mu);
    if (!g_override.empty()) return g_override;
  }

  wchar_t local[MAX_PATH]{};
  const HRESULT hr =
      ::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local);
  if (FAILED(hr)) return err(status::io);

  std::wstring dir = local;
  dir += L"\\MediaViewer\\thumbs";
  if (!::CreateDirectoryW((std::wstring(local) + L"\\MediaViewer").c_str(), nullptr)) {
    const DWORD errn = ::GetLastError();
    if (errn != ERROR_ALREADY_EXISTS) return err(status::io);
  }
  if (!::CreateDirectoryW(dir.c_str(), nullptr)) {
    const DWORD errn = ::GetLastError();
    if (errn != ERROR_ALREADY_EXISTS) return err(status::io);
  }
  return utf8_from_wide(dir.c_str());
}

void set_addons_dir_override(std::string_view utf8_dir) {
  std::lock_guard lock(g_mu);
  g_addons_override.assign(utf8_dir);
}

result<std::string> addons_dir() {
  {
    std::lock_guard lock(g_mu);
    if (!g_addons_override.empty()) return g_addons_override;
  }
  wchar_t local[MAX_PATH]{};
  if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local))) {
    return err(status::io);
  }
  const std::wstring app = std::wstring(local) + L"\\MediaViewer";
  const std::wstring dir = app + L"\\addons";
  for (const std::wstring* d : {&app, &dir}) {
    if (!::CreateDirectoryW(d->c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) {
      return err(status::io);
    }
  }
  return utf8_from_wide(dir.c_str());
}

void set_metadata_snapshot_dir_override(std::string_view utf8_dir) {
  std::lock_guard lock(g_mu);
  g_snapshot_override.assign(utf8_dir);
}

result<std::string> metadata_snapshot_dir() {
  {
    std::lock_guard lock(g_mu);
    if (!g_snapshot_override.empty()) return g_snapshot_override;
  }
  wchar_t local[MAX_PATH]{};
  if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local))) {
    return err(status::io);
  }
  const std::wstring app = std::wstring(local) + L"\\MediaViewer";
  const std::wstring dir = app + L"\\metadata-snapshots";
  for (const std::wstring* d : {&app, &dir}) {
    if (!::CreateDirectoryW(d->c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) {
      return err(status::io);
    }
  }
  return utf8_from_wide(dir.c_str());
}

result<std::string> default_library_dir() {
  wchar_t pictures[MAX_PATH]{};
  if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_MYPICTURES, nullptr, SHGFP_TYPE_CURRENT, pictures))) {
    return err(status::io);
  }
  return utf8_from_wide((std::wstring(pictures) + L"\\MediaViewer").c_str());
}

}  // namespace mv::io
