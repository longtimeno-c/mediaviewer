// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Phone access module's view of itself and of the OS's paths, one
// implementation per OS (module_self_win.cpp, module_self_posix.cpp) so
// addon_entry.cpp stays free of platform headers (tools/check-hostable-core.ps1).
#pragma once

#include <filesystem>
#include <string>

namespace mv::phone::module_self {

// The link's thread is detached at stop (it never waits on the NAS), so this
// library must never be unmapped under it: it holds a reference to itself
// that is never let go. A remove takes effect at the next launch.
void pin() noexcept;

// The folder this library was loaded from (the add-on's verified version
// folder); "" when the OS does not say.
[[nodiscard]] std::string dir();

// `name` in `dir`, with the OS's separator.
[[nodiscard]] std::string join(const std::string& dir, const char* name);

// The app's clip helper (MediaViewerClipJob, in Contents/Helpers on the Mac, beside the app's
// program on Windows and Linux), which the server runs to convert clips for phones
// (docs/plans/phone-clip-convert.md). "" when it is not there.
[[nodiscard]] std::string app_clip_helper();

// A UTF-8 path as the OS's (wide on Windows).
[[nodiscard]] std::filesystem::path fs_path(const std::string& utf8);

}  // namespace mv::phone::module_self
