// SPDX-License-Identifier: GPL-2.0-or-later
// Whole-file read. Worker threads only — never the UI or render thread
// (plan/02-architecture.md).
#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace mv::io {

// Reads the entire file at `utf8_path` into memory. The path is UTF-8 and is
// converted to UTF-16 for the Windows API; it is never logged (rule 6).
[[nodiscard]] result<std::vector<std::uint8_t>> read_all(std::string_view utf8_path);

[[nodiscard]] result<std::vector<std::uint8_t>> read_prefix(std::string_view utf8_path, std::size_t max_bytes);

[[nodiscard]] expected write_all(std::string_view utf8_path, std::span<const std::uint8_t> bytes);

// True when `utf8_path` names an existing file (not a directory). Used to
// validate a cache row before trusting it; never reports why it failed.
[[nodiscard]] bool file_exists(std::string_view utf8_path) noexcept;

}  // namespace mv::io
