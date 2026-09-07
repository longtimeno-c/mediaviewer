// SPDX-License-Identifier: GPL-2.0-or-later
// Whole-file read. Worker threads only — never the UI or render thread
// (plan/02-architecture.md).
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace mv::io {

// Reads the entire file at `utf8_path` into memory. The path is UTF-8 and is
// converted to UTF-16 for the Windows API; it is never logged (rule 6).
[[nodiscard]] result<std::vector<std::uint8_t>> read_all(std::string_view utf8_path);

}  // namespace mv::io
