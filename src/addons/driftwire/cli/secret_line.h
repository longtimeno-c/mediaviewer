// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// One line of a secret from stdin, for `driftwire serve init` and `serve
// passphrase` (docs/plans/nas-owner-and-guests.md §3.6): echo off while it is
// typed on a terminal. One file per platform, as term_*: the OS's console calls
// stay in secret_line_posix.cpp and secret_line_win.cpp (D9).
#pragma once

#include <cstddef>
#include <string>

namespace mv::driftwire::cli {

// Reads up to a newline into `out` (no newline, no trailing CR), keeping at most
// `max` bytes; `out` is reserved once so no copy is left behind unwiped. With
// `hidden` and a terminal on stdin, echo is off while it is read, and
// `echo_was_off` says so (the caller prints the newline it did not show).
// False at the end of input with nothing read.
[[nodiscard]] bool read_secret_line(std::string& out, bool hidden, std::size_t max, bool& echo_was_off);

// Whether stdin is a terminal (a console on Windows).
[[nodiscard]] bool stdin_is_terminal();

}  // namespace mv::driftwire::cli
