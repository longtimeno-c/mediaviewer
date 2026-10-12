// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Per-file video resume points (docs/design/05, issue #235).
//
// One small file per clip in a `resume` folder beside the thumbnail cache. The
// name is a hash of the path and the body holds a second, independent hash of
// it plus the file's size and mtime, so a renamed or rewritten clip never
// resumes and the folder holds no path or filename in plain text. An entry is
// written only for a position worth resuming; closing a clip anywhere else
// removes its entry. The folder is capped at `resume_store_cap` entries, oldest
// written first out. Plain portable C++: no OS calls beyond std::filesystem.
//
// [blocking] all of it: it is file I/O. The player runs it on open (the decode
// side, before the first frame) and on the ABI's cleanup worker as a source
// retires, never on the UI or render thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

#include "player/video_source.h"  // time_ns

namespace mv::player {

// Enough for every clip of a long camera dump to keep its place; the oldest
// entries go first. One entry is ~60 bytes, so the cap is tens of KB on disk.
inline constexpr std::size_t resume_store_cap = 1000;

// The content identity a stored position is valid for.
struct resume_key {
  std::string_view path;        // UTF-8; hashed, never written
  std::uint64_t size = 0;
  std::int64_t mtime = 0;       // filesystem clock ticks, as last_write_time reports
};

// The stored position for `key`, or 0 when there is none, it belongs to another
// version of the file, or it is no longer worth resuming against `duration_ns`.
[[nodiscard]] time_ns resume_load(const std::filesystem::path& dir, const resume_key& key,
                                  time_ns duration_ns) noexcept;

// Stores `position_ns` when should_store_resume says it is worth it; otherwise
// removes any entry the clip had. Creates `dir` only when it writes, and trims
// the folder to `resume_store_cap` now and then (once per process, then every
// few dozen writes).
void resume_save(const std::filesystem::path& dir, const resume_key& key,
                 time_ns position_ns, time_ns duration_ns) noexcept;

// Keeps the newest `cap` entries and deletes the rest, plus any entry in the
// pre-#235 format (`*.txt`, which held the clip's path in plain text).
void resume_prune(const std::filesystem::path& dir, std::size_t cap) noexcept;

}  // namespace mv::player
