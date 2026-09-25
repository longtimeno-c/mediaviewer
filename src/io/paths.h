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

// Where add-ons install (plan/18 "Location"): %LocalAppData%\MediaViewer\addons
// on Windows, ~/Library/Application Support/MediaViewer/Add-ons on Mac.
// Per-user; created if missing.
[[nodiscard]] result<std::string> addons_dir();
void set_addons_dir_override(std::string_view utf8_dir);  // tests; empty restores

// Import's default destination (plan/18 "Painless by default"):
// Pictures\MediaViewer / ~/Pictures/MediaViewer. Not created here; the
// first import creates it.
[[nodiscard]] result<std::string> default_library_dir();

}  // namespace mv::io
