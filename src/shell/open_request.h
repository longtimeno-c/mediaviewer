// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// What to open from a command line or a drop (plan/16, PR 6 "drag-and-drop in,
// argv handling"). Pure: the caller probes the paths, this decides.
//
// Rule: the first entry that exists wins. A folder opens that folder; a file
// opens its folder with that file selected. So one folder dropped opens it,
// several files from one folder open the folder on the first of them, and a
// mixed drop opens the first file's folder. Entries that do not exist are
// skipped; if none exists the request is `missing` and nothing is opened.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace mv::shell {

struct path_probe {
  std::wstring path;
  bool exists = false;
  bool is_directory = false;
};

enum class open_kind : std::uint8_t { none, folder, file, missing };

struct open_request {
  open_kind kind = open_kind::none;
  std::wstring path;  // the folder, or the file to select in its folder
};

// Trims whitespace and one pair of surrounding quotes, turns '/' into '\', and
// drops a trailing separator unless the path is a root ("C:\", "\\?\C:\").
// `\\?\` long-path prefixes are kept: the Windows APIs take them as they are.
[[nodiscard]] std::wstring normalize_open_path(std::wstring_view raw);

[[nodiscard]] open_request resolve_open(std::span<const path_probe> candidates);

}  // namespace mv::shell
