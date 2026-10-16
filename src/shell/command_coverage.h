// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Which host runs which command (docs/design/16, issue #239).
//
// command_coverage.inc is the only list. The Mac supported-command check and
// both hosts' command dispatchers read it, so a command cannot be handled and
// missing from Settings, or listed and not dispatched. A new command_id fails
// the static_assert until it has a row.
#pragma once

#include "shell/commands.h"

namespace mv::shell {

// Where runCommand / run_command sends the id. `excluded` means that function
// has no case: the Mac returns no, Windows run_command returns false.
enum class command_area : std::uint8_t {
  canvas,
  transport,
  marks,
  edit,
  slideshow,
  session,
  excluded
};

// `handles`: this host accounts for the id (its dispatcher, or, on Windows, the
// island switch / the Esc key path). `excludes`: it deliberately does not.
enum class command_take : std::uint8_t { handles, excludes };

// The five wire values that are not enumerators (commands.h).
inline constexpr int kCommandIdGaps = 5;

inline constexpr int command_coverage_rows() noexcept {
#define MV_COMMAND(name, mac_area, win_area, mac_take, win_take, mac_why, win_why) +1
  return 0
#include "shell/command_coverage.inc"
      ;
#undef MV_COMMAND
}

static_assert(command_coverage_rows() + kCommandIdGaps == static_cast<int>(command_id::count),
              "command_coverage.inc must name every command_id except count");

[[nodiscard]] inline constexpr command_area mac_command_area(command_id id) noexcept {
  switch (id) {
#define MV_COMMAND(name, mac_area, win_area, mac_take, win_take, mac_why, win_why) \
    case command_id::name:                                                         \
      return command_area::mac_area;
#include "shell/command_coverage.inc"
#undef MV_COMMAND
    case command_id::count:
      break;
  }
  return command_area::excluded;
}

[[nodiscard]] inline constexpr command_area win_command_area(command_id id) noexcept {
  switch (id) {
#define MV_COMMAND(name, mac_area, win_area, mac_take, win_take, mac_why, win_why) \
    case command_id::name:                                                         \
      return command_area::win_area;
#include "shell/command_coverage.inc"
#undef MV_COMMAND
    case command_id::count:
      break;
  }
  return command_area::excluded;
}

[[nodiscard]] inline constexpr bool mac_dispatcher_handles(command_id id) noexcept {
  switch (id) {
#define MV_COMMAND(name, mac_area, win_area, mac_take, win_take, mac_why, win_why) \
    case command_id::name:                                                         \
      return command_take::mac_take == command_take::handles;
#include "shell/command_coverage.inc"
#undef MV_COMMAND
    case command_id::count:
      break;
  }
  return false;
}

[[nodiscard]] inline constexpr bool win_dispatcher_handles(command_id id) noexcept {
  switch (id) {
#define MV_COMMAND(name, mac_area, win_area, mac_take, win_take, mac_why, win_why) \
    case command_id::name:                                                         \
      return command_take::win_take == command_take::handles;
#include "shell/command_coverage.inc"
#undef MV_COMMAND
    case command_id::count:
      break;
  }
  return false;
}

// Non-null when that host excludes the id. Null when it handles it.
[[nodiscard]] inline constexpr const char* mac_command_exclusion(command_id id) noexcept {
  switch (id) {
#define MV_COMMAND(name, mac_area, win_area, mac_take, win_take, mac_why, win_why) \
    case command_id::name:                                                         \
      return mac_why;
#include "shell/command_coverage.inc"
#undef MV_COMMAND
    case command_id::count:
      break;
  }
  return nullptr;
}

[[nodiscard]] inline constexpr const char* win_command_exclusion(command_id id) noexcept {
  switch (id) {
#define MV_COMMAND(name, mac_area, win_area, mac_take, win_take, mac_why, win_why) \
    case command_id::name:                                                         \
      return win_why;
#include "shell/command_coverage.inc"
#undef MV_COMMAND
    case command_id::count:
      break;
  }
  return nullptr;
}

}  // namespace mv::shell
