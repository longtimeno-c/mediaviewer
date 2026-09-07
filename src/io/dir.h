// SPDX-License-Identifier: GPL-2.0-or-later
// Portable directory listing and watch. Windows impl is dir_win.cpp (D9).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace mv::io {

struct dir_entry {
  std::string name_utf8;
  std::string path_utf8;
  std::uint64_t size = 0;
  std::int64_t mtime_unix = 0;
};

// JPEG / PNG / BMP only (the PR 2 decode set). Sorted by name, case-insensitive.
[[nodiscard]] result<std::vector<dir_entry>> list_still_files(std::string_view utf8_dir);

[[nodiscard]] result<bool> is_directory(std::string_view utf8_path);

// If `utf8_path` is a directory, returns it. If it is a file, returns the parent.
[[nodiscard]] result<std::string> containing_dir(std::string_view utf8_path);

class directory_watcher {
 public:
  using callback = void (*)(void* user);

  directory_watcher();
  ~directory_watcher();

  directory_watcher(const directory_watcher&) = delete;
  directory_watcher& operator=(const directory_watcher&) = delete;

  // `cb` runs on the watch thread. It must not block and must not re-enter
  // the watcher. Submit a job from it.
  [[nodiscard]] expected start(std::string_view utf8_dir, callback cb, void* user);
  void stop() noexcept;

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

}  // namespace mv::io
