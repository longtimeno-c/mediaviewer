// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// volume.h on Windows: a folder is kept by its volume's GUID path
// ("\\?\Volume{...}\", which a drive keeps whatever its letter) as well as by
// its path. Only a folder under a drive letter is moved to a new letter; one
// under a folder mount point keeps its path (and reads offline while that
// mount point names another volume).
#include "addons/driftwire/cli/volume.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <cwchar>
#include <iterator>
#include <vector>

namespace mv::driftwire::cli {

namespace {

std::wstring wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return {};
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  (void)::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::string narrow(const wchar_t* s) {
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string out(static_cast<std::size_t>(n), '\0');
  (void)::WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
  out.resize(static_cast<std::size_t>(n - 1));
  return out;
}

// The volume a path is on now, as its GUID path; "" when it has none (no
// drive at that letter, or a network share).
std::string volume_now(const std::string& path, std::string* mount_root = nullptr) {
  const std::wstring w = wide(path);
  if (w.empty()) return {};
  // No "insert a disk" box for an empty card reader: this runs unattended.
  // Per thread: sessions ask this at once.
  DWORD old_mode = 0;
  const bool set = ::SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &old_mode) != 0;
  wchar_t mount[MAX_PATH + 1] = {};
  wchar_t guid[64] = {};
  const bool ok = ::GetVolumePathNameW(w.c_str(), mount, MAX_PATH) &&
                  ::GetVolumeNameForVolumeMountPointW(mount, guid, static_cast<DWORD>(std::size(guid)));
  if (set) (void)::SetThreadErrorMode(old_mode, nullptr);
  if (mount_root) *mount_root = ok ? narrow(mount) : std::string();
  return ok ? narrow(guid) : std::string();
}

bool drive_letter_path(const std::string& p) {
  return p.size() >= 3 && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' &&
         (p[2] == '\\' || p[2] == '/');
}

}  // namespace

volume_facts volume_of(const std::string& path) {
  volume_facts f;
  std::string root;
  const std::string guid = volume_now(path, &root);
  // Kept only for a volume at a drive letter's root ("E:"): a folder under a
  // folder mount point keeps its path and only its folder decides.
  if (drive_letter_path(root) && root.size() == 3) f.volume = guid;
  return f;
}

bool volume_present(const serve::root_config& root) {
  if (root.volume.empty()) return true;
  const std::string now = volume_now(root.path);
  return !now.empty() && _stricmp(now.c_str(), root.volume.c_str()) == 0;
}

std::optional<std::string> volume_locate(const serve::root_config& root) {
  if (volume_present(root)) return root.path;
  if (!drive_letter_path(root.path)) return std::nullopt;
  const std::wstring guid = wide(root.volume);
  if (guid.empty()) return std::nullopt;
  // Every place the volume is mounted, as a list of strings ending in an empty one.
  std::vector<wchar_t> names(512);
  DWORD needed = 0;
  if (!::GetVolumePathNamesForVolumeNameW(guid.c_str(), names.data(), static_cast<DWORD>(names.size()), &needed)) {
    if (::GetLastError() != ERROR_MORE_DATA || needed == 0 || needed > 32768) return std::nullopt;
    names.assign(needed, L'\0');
    if (!::GetVolumePathNamesForVolumeNameW(guid.c_str(), names.data(), needed, &needed)) return std::nullopt;
  }
  for (const wchar_t* at = names.data(); *at; at += wcslen(at) + 1) {
    const std::string mount = narrow(at);  // "F:\"
    if (!drive_letter_path(mount) || mount.size() != 3) continue;
    std::string moved = root.path;
    moved[0] = mount[0];
    return moved;
  }
  return std::nullopt;
}

}  // namespace mv::driftwire::cli
