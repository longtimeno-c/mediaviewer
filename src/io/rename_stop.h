// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #210: F2 renames the file on screen (docs/design/16 "Marks, copy, move",
// docs/design/24 "The file port"). One stop, never a batch: no filename templates.
//
// A stop is more than one file. The rename carries, under the new name:
//   * the file itself, keeping its extension;
//   * its pair's other half (RAW+JPEG, a Live Photo's MOV), so the pair stays
//     a pair;
//   * each one's XMP sidecar, ours and Lightroom's (`IMG_1234.xmp`, shared by
//     every file of the stem, docs/design/06) and darktable's
//     (`IMG_1234.CR2.xmp`), so a rating or a label is not left behind.
//
// Never overwrites, the copy / move rule (io/collision_name.h): when any of
// the group's new names is taken, the whole group takes `name (2)`, then
// `(3)`, so its members keep one stem. Every move is a same-folder
// rename_no_replace (io/file_port.h); a race that takes a name mid-way puts
// back what had moved and reports it. Portable (D9). I/O pool only: it stats
// and renames (rule 1). Nothing here logs a path (rule 6).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace mv::io {

enum class stem_problem : std::uint8_t {
  ok,
  empty,          // nothing typed, or only spaces
  separator,      // a / or \: a name, not a path
  reserved,       // < > : " | ? * or a control character (Windows refuses them)
  dots,           // "." or "..", or a trailing dot or space (Windows drops them)
  device,         // CON, PRN, AUX, NUL, COM1-9, LPT1-9 (Windows)
  too_long,       // more than kMaxStemBytes
};

inline constexpr std::size_t kMaxStemBytes = 200;

// Whether `stem` (the new name without its extension) can be a file name on
// both platforms. Leading / trailing spaces are the caller's to trim.
[[nodiscard]] stem_problem check_stem(std::string_view stem) noexcept;
// "A name cannot contain / or \", for the host's notice. "" for ok.
[[nodiscard]] const char* stem_problem_text(stem_problem p) noexcept;

// "IMG_1234" from "/a/IMG_1234.CR2" (a leading dot is part of the name).
[[nodiscard]] std::string_view stem_of(std::string_view utf8_path) noexcept;

struct rename_move {
  std::string from;
  std::string to;
};

// What a rename of the stop `primary` (+ `secondary`, empty when unpaired) to
// `new_stem` would do, given `exists` (a path is taken). Pure: the caller's
// `exists` decides, so tests need no filesystem. The members that exist move;
// the first stem of `new_stem`, `new_stem (2)`, ... that leaves every target
// free (a target that is one of the group's own sources is free: a case-only
// rename) is chosen. Empty when nothing would change or every candidate is
// taken (the caller reports which, from `new_stem`).
[[nodiscard]] std::vector<rename_move> plan_rename(
    std::string_view primary, std::string_view secondary, std::string_view new_stem,
    const std::function<bool(std::string_view)>& exists);

struct rename_result {
  std::string primary;    // the stop's file, renamed
  std::string secondary;  // its pair half, renamed (empty when unpaired)
  std::size_t moved = 0;  // files renamed, sidecars included
};

// Plans against the disk and renames. `status::invalid_arg` for a stem
// check_stem refuses or no change, `status::io` when a file would not move
// (the group is put back as it was), `status::not_found` when the stop's file
// is gone. `before`, when set, is told the stop's file's old and new path
// once the plan is made and before anything moves, so a listing can be told
// to follow it before a watcher sees the change.
using rename_before_fn = std::function<void(const std::string& from, const std::string& to)>;
[[nodiscard]] result<rename_result> rename_stop(std::string_view primary, std::string_view secondary,
                                                std::string_view new_stem,
                                                const rename_before_fn& before = {}) noexcept;

}  // namespace mv::io
