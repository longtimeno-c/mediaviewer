// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The two things about the driftwire command line's process that differ per
// OS (D9 ports: data_dir_posix.cpp for Linux and macOS, data_dir_win.cpp):
//
//   its own folder   where its prefs, its unfinished receives, its DHT table
//                    and its lock live: never the add-on's
//                    <addons>/driftwire/data, so the command line and the
//                    window never change each other's settings (both write
//                    their prefs whole). Linux ${XDG_STATE_HOME:-~/.local/state}/driftwire,
//                    macOS ~/Library/Application Support/Driftwire, Windows
//                    %LOCALAPPDATA%\Driftwire. Made for this user alone (0700).
//   its arguments    as UTF-8: Windows hands main() the ANSI code page's, so
//                    the command line is read again from GetCommandLineW;
//                    and what it prints outside the session (--help, devices,
//                    an error), which the Windows console takes as UTF-16.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace mv::driftwire::cli {

// The folder, made if it is not there; "" when it cannot be made (nothing is
// then kept across runs, and no second session is kept out).
[[nodiscard]] std::string data_dir();
// The home folder ("" when there is none).
[[nodiscard]] std::string home_dir();
// argv[1..] as UTF-8.
[[nodiscard]] std::vector<std::string> utf8_args(int argc, char** argv);
// `utf8` on standard output (or error), whole.
void print(std::string_view utf8, bool to_stderr = false);

}  // namespace mv::driftwire::cli
