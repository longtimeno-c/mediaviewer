// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// io::tree_watcher where the OS half is not written yet (Linux: inotify is per
// folder and capped by max_user_watches; docs/plans/linux.md decides when its
// halves land). Nothing is watched, so Local search keeps its 15-minute walk.
#include "io/tree_watch.h"

namespace mv::io {

struct tree_watcher::impl {};

tree_watcher::tree_watcher() = default;
tree_watcher::~tree_watcher() = default;

expected tree_watcher::start(const std::vector<std::string>& roots, callback, void*, std::vector<bool>& out_ok) {
  out_ok.assign(roots.size(), false);
  return err(status::unsupported_format);
}

void tree_watcher::stop() noexcept {}

}  // namespace mv::io
