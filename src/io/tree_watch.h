// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Watches whole folder trees and says which directory changed
// (docs/plans/index-watcher.md). Local search's remembered roots use it
// through the add-on host table (watch_tree); the viewer's open folder keeps
// directory_watcher (dir.h), which only says "something changed".
// Windows: tree_watch_win.cpp (ReadDirectoryChangesW, subtree, one completion
// port); Mac: tree_watch_mac.cpp (one FSEvents stream, file events) (D9).

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace mv::io {

class tree_watcher {
 public:
  // `dir` is everything below it, not only its own files: a folder moved in,
  // renamed or removed, or the OS asking for a rescan below it. A path that
  // is no longer a directory (removed, or a file) is reported this way too.
  static constexpr std::uint32_t subtree = 1;
  // Events were lost: rescan the whole root (`dir` is the root).
  static constexpr std::uint32_t overflow = 2;

  // On the watch thread; must not block or re-enter the watcher. `root` is
  // the index into start()'s list; `dir` is under roots[root] as given there
  // (its spelling, then the names the OS reports), with no trailing separator.
  using callback = void (*)(void* user, std::uint32_t root, std::string_view dir_utf8, std::uint32_t flags);

  tree_watcher();
  ~tree_watcher();
  tree_watcher(const tree_watcher&) = delete;
  tree_watcher& operator=(const tree_watcher&) = delete;

  // Replaces whatever was watched with `roots`. out_ok[i] is false for a root
  // that could not be watched (offline, gone, no access); the others are.
  // Fails (and watches nothing) only when none could be watched or the watch
  // itself could not start. Worker threads only: it opens each root. Once
  // stop() or the next start() has returned, no callback of the old set is
  // running or to come.
  [[nodiscard]] expected start(const std::vector<std::string>& roots, callback cb, void* user,
                               std::vector<bool>& out_ok);
  void stop() noexcept;

 private:
  struct impl;
  std::mutex mutex_;
  std::unique_ptr<impl> impl_;
};

}  // namespace mv::io
