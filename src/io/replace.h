// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The io replace port (plan/10 PR 10): every write of edited pixels goes
// through here. Portable header; io/replace_win.cpp (ReplaceFileW) and
// io/replace_mac.cpp (same-directory temp, F_FULLFSYNC, rename) implement it
// (D9: no HANDLE or fd in the interface).
//
// I/O pool only — never the UI or render thread (plan/16 speed rule 2).
// Nothing here logs a path (rule 6).
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "core/result.h"

namespace mv::io {

// Export: creates `utf8_path` and fails with status::io if anything is
// already there — an export never overwrites (rule 5). A partial file from a
// failed write is removed. Flushed to stable storage before returning.
[[nodiscard]] expected write_new(std::string_view utf8_path, std::span<const std::uint8_t> bytes);

// The viewer's lossless rotate (plan/10 PR 10 verify: "keyboard-only rotate
// of a JPEG in the viewer writes that file"): the new bytes go to a sibling
// temporary that is flushed to stable storage and then swapped over
// `utf8_path` in one step, so a crash mid-write leaves the old file whole.
// Permissions (and on Windows the ACL and creation time) are kept. The caller
// has already made sure the change is lossless.
[[nodiscard]] expected replace_atomic(std::string_view utf8_path, std::span<const std::uint8_t> bytes);

// PR 12 (a new XMP sidecar): like `write_new` — nothing is overwritten, an
// existing file is `status::io` — but the bytes go to a sibling temporary that
// is flushed and then given the final name in one step, so a crash mid-write
// leaves no partial file *under that name* (a half sidecar would poison every
// later write to the file it sits beside).
[[nodiscard]] expected write_new_atomic(std::string_view utf8_path, std::span<const std::uint8_t> bytes);

}  // namespace mv::io
