// SPDX-License-Identifier: GPL-2.0-or-later
// Process-wide cache locations. UTF-8. Windows impl is paths_win.cpp (D9).
#pragma once

#include <string>
#include <string_view>

#include "core/result.h"

namespace mv::io {

// `%LocalAppData%/MediaViewer/thumbs` on Windows. Created if missing.
[[nodiscard]] result<std::string> thumb_cache_dir();

// Tests only. Empty path restores the default.
void set_thumb_cache_dir_override(std::string_view utf8_dir);

}  // namespace mv::io
