// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Tab at the driftwire prompt: paths (and folders only, for /save-to and
// "save somewhere else"), and the slash palette's filter over the commands.
// Pure apart from listing the folder being completed; tested with temp dirs.
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "addons/driftwire/cli/paths_arg.h"

namespace mv::driftwire::cli {

// ---- the commands (palette order) ------------------------------------------------------------

struct command_info {
  std::string_view name;   // "/code"
  std::string_view args;   // "N on|off", "" for none
  std::string_view description;
  std::string_view alias;  // "/exit" for /quit, else ""
};

[[nodiscard]] std::span<const command_info> commands() noexcept;

// The commands for what is typed after "/" (up to the first space): those
// whose name starts with it, then those that contain it, each group in
// palette order; every command for "". Indexes into commands().
[[nodiscard]] std::vector<std::size_t> filter_commands(std::string_view typed);
// The command `name` ("/quit", or its alias "/exit") names; -1 for none.
[[nodiscard]] int find_command(std::string_view name);

// ---- paths -------------------------------------------------------------------------------------

struct completion {
  bool changed = false;               // `line` / `cursor` differ from what was typed
  std::string line;
  std::size_t cursor = 0;
  std::vector<std::string> choices;   // more than one match: their names (folders end in a separator)
};

// Completes the word before `cursor` in `line` as a path from `o.cwd`:
// one match is written out whole (a folder with its separator, a file with
// the quote closed and a space after); several are written as far as they
// agree and listed in `choices`. The word keeps its quoting: inside '...' or
// "..." the name goes in as it is, unquoted it is escaped (POSIX) or the word
// is put in double quotes (Windows). Hidden names only when the word starts
// with a dot; `folders_only` lists folders alone.
[[nodiscard]] completion complete_path(std::string_view line, std::size_t cursor, const path_options& o,
                                       bool folders_only);

}  // namespace mv::driftwire::cli
