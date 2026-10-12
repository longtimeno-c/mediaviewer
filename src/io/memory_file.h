// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// An unsaved image addressed like a file (docs/design/16 "New from Clipboard"):
// bytes the user pasted, held in memory under a reserved key,
// "clipboard:<n>/<name>", that no file system path can take. read_all,
// read_prefix, stat_path and file_exists answer for the key from memory, so the
// viewer, the edit stack and the export read it as they read a file; every write
// primitive (write_all, write_new, write_new_atomic, replace_atomic) refuses
// it with permission_denied, so nothing is written for it until the user saves
// a copy somewhere real. Nothing here touches the disk.
//
// One memory file at a time: a second put releases the first. `n` grows with
// every put, so caches keyed by path (the viewer's, an edit stack's) never take
// one paste for another. Any thread.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace mv::io {

inline constexpr std::string_view kMemoryScheme = "clipboard:";

[[nodiscard]] constexpr bool is_memory_path(std::string_view path) noexcept {
  return path.size() > kMemoryScheme.size() && path.substr(0, kMemoryScheme.size()) == kMemoryScheme;
}

// Holds `bytes` as the memory file, named `name` (a file name, no separator),
// releasing the one before. Returns its key.
[[nodiscard]] std::string put_memory_file(std::vector<std::uint8_t> bytes, std::string_view name);

// The bytes under `key`, or null once another put (or clear) released them.
[[nodiscard]] std::shared_ptr<const std::vector<std::uint8_t>> memory_file(std::string_view key) noexcept;

struct memory_file_stamp {
  std::uint64_t size = 0;
  std::int64_t mtime_ns = 0;  // when it was put, ns since the Unix epoch
};
[[nodiscard]] bool memory_file_stat(std::string_view key, memory_file_stamp& out) noexcept;

// read_prefix's answer for a memory key: not_found once released, `corrupt`
// for no bytes (as for an empty file). `max_bytes` 0 is the whole file.
[[nodiscard]] result<std::vector<std::uint8_t>> read_memory_prefix(std::string_view key,
                                                                   std::size_t max_bytes);

// Releases the memory file (tests; nothing else needs to).
void clear_memory_files() noexcept;

}  // namespace mv::io
