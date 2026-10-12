// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// File > Duplicate (issue #289, docs/design/16 "Marks, copy, move"): a copy of the
// item on screen beside itself, under the name the platform's own file manager
// would give it. Finder (and Preview): "IMG_1234 copy.JPG", then
// "IMG_1234 copy 2.JPG". Explorer: "IMG_1234 - Copy.JPG", then
// "IMG_1234 - Copy (2).JPG".
//
// A stop is more than one file: a RAW+JPEG or Live Photo pair, and an XMP
// sidecar (meta/write.h sidecar_path_for: "IMG_1234.xmp" beside it). They are
// duplicated together under one number, so the copies pair and keep their
// sidecar the way the originals do. Each file is a verified copy
// (io/verified_copy.h), never an overwrite: a name taken between the check and
// the rename moves the whole group on to the next number. Portable over
// io/file_port.h (D9); I/O pool only (rule 1); no path is logged (rule 6).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace mv::io {

enum class duplicate_style : std::uint8_t {
  finder,    // "name copy.ext", "name copy 2.ext"
  explorer,  // "name - Copy.ext", "name - Copy (2).ext"
};

// The style of the platform this build runs on.
[[nodiscard]] constexpr duplicate_style native_duplicate_style() noexcept {
#if defined(_WIN32)
  return duplicate_style::explorer;
#else
  return duplicate_style::finder;
#endif
}

// `file_name` (no directory) as the n-th duplicate, n >= 1. The suffix goes
// before the last extension; a leading dot is part of the name, as in
// collision_name.h (".profile" -> ".profile copy").
[[nodiscard]] std::string duplicate_name(std::string_view file_name, int n, duplicate_style style);

// The first n (first..max_attempts) at which every name of the group is free
// by `exists`, or 0 when none is. Pure: the existence check is the caller's.
template <class Exists>
[[nodiscard]] int first_free_duplicate(std::span<const std::string> file_names, duplicate_style style,
                                       Exists&& exists, int first = 1, int max_attempts = 9999) {
  if (file_names.empty() || first < 1) return 0;
  for (int n = first; n <= max_attempts; ++n) {
    bool all_free = true;
    for (const std::string& name : file_names) {
      if (exists(std::string_view{duplicate_name(name, n, style)})) {
        all_free = false;
        break;
      }
    }
    if (all_free) return n;
  }
  return 0;
}

// The files a duplicate of the stop `primary` (+ `secondary`, the other half
// of a pair, or empty) carries: those two, then the XMP sidecar of either stem
// if one exists. Stats the sidecar candidates; worker thread.
[[nodiscard]] std::vector<std::string> duplicate_group(std::string_view primary_utf8,
                                                       std::string_view secondary_utf8);

// Duplicates every file of `group` (full paths, one folder) beside itself
// under one shared number. Returns the new paths in `group`'s order; the
// first is the copy to select. On a failure nothing of the group is left
// behind and the originals are untouched (rule 5).
[[nodiscard]] result<std::vector<std::string>> duplicate_files(std::span<const std::string> group,
                                                               duplicate_style style);

}  // namespace mv::io
