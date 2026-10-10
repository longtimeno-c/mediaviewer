// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/write_guard.h"

#include "io/memory_file.h"
#include "shell/clipboard_image.h"

#include <mutex>
#include <unordered_set>

namespace mv::shell {
namespace {

std::mutex g_m;
std::unordered_set<std::string> g_paths;
std::string g_prefix;  // with a trailing separator

constexpr std::string_view kPhotosKey = "photos:";

bool inside_photos_library(std::string_view path) {
  constexpr std::string_view kBundle = ".photoslibrary";
  for (std::size_t i = 0; i + kBundle.size() <= path.size(); ++i) {
    bool match = true;
    for (std::size_t j = 0; j < kBundle.size() && match; ++j) {
      char c = path[i + j];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      match = c == kBundle[j];
    }
    if (!match) continue;
    const std::size_t end = i + kBundle.size();
    // The bundle itself or anything in it (APFS is case-insensitive by default).
    if (end == path.size() || path[end] == '/' || path[end] == '\\') return true;
  }
  return false;
}

}  // namespace

bool write_protected(std::string_view path) {
  if (path.empty()) return false;
  if (inside_photos_library(path)) return true;
  // A Photos library item itself (docs/design/26): no file, and never a write.
  if (path.size() > kPhotosKey.size() && path.substr(0, kPhotosKey.size()) == kPhotosKey) return true;
  // An unsaved item (New from Clipboard): no file until Save Copy writes one.
  if (io::is_memory_path(path)) return true;
  std::lock_guard lock(g_m);
  if (!g_prefix.empty() && path.size() > g_prefix.size() && path.substr(0, g_prefix.size()) == g_prefix) {
    return true;
  }
  return g_paths.count(std::string(path)) != 0;
}

bool any_write_protected(std::span<const std::string> paths) {
  for (const std::string& p : paths) {
    if (write_protected(p)) return true;
  }
  return false;
}

const char* write_protected_notice(std::string_view path) noexcept {
  return io::is_memory_path(path) ? kUnsavedItemNotice : kWriteProtectedNotice;
}

void set_read_only_paths(std::span<const std::string> paths) {
  std::lock_guard lock(g_m);
  g_paths.clear();
  g_paths.insert(paths.begin(), paths.end());
}

void set_read_only_prefix(std::string_view dir) {
  std::lock_guard lock(g_m);
  g_prefix.assign(dir);
  if (!g_prefix.empty() && g_prefix.back() != '/' && g_prefix.back() != '\\') g_prefix.push_back('/');
}

}  // namespace mv::shell
