// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// volume.h on Linux and macOS: a mount point is a folder on another device
// than its parent (st_dev), which is what an unmounted one stops being.
#include "addons/driftwire/cli/volume.h"

#include <sys/stat.h>

namespace mv::driftwire::cli {

namespace {

// The folder is on another device than its parent; false when either cannot
// be looked at, or the folder is "/" (its own parent).
bool is_mount_point(const std::string& path) {
  struct stat here {}, parent {};
  if (path.empty() || ::stat(path.c_str(), &here) != 0 || !S_ISDIR(here.st_mode)) return false;
  std::string up = path;
  while (up.size() > 1 && up.back() == '/') up.pop_back();
  up += "/..";
  if (::stat(up.c_str(), &parent) != 0) return false;
  return here.st_dev != parent.st_dev;
}

}  // namespace

volume_facts volume_of(const std::string& path) {
  volume_facts f;
  f.mount = is_mount_point(path);
  return f;
}

bool volume_present(const serve::root_config& root) { return !root.mount || is_mount_point(root.path); }

std::optional<std::string> volume_locate(const serve::root_config& root) {
  if (!volume_present(root)) return std::nullopt;
  return root.path;
}

}  // namespace mv::driftwire::cli
