// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The one place `driftwire` asks the OS for an administrator: the LAN share's
// elevated step (docs/plans/self-host-users-smb.md §5). The front end
// (`host users …`, never elevated) starts `driftwire host smb-apply` elevated,
// hands it one line (the request, its passwords included) over a channel only
// the two hold, and reads one line back (the answer: no password, no path).
// Never a command-line argument, never a file.
//
//   macOS     at a terminal: sudo (it asks on the tty; the line goes on its
//             stdin); from the app: the authorization dialog
//             (AuthorizationExecuteWithPrivileges, its pipe)
//   Windows   UAC ("runas") with the name of a named pipe this user alone can
//             open, made first; the helper's process id checked against the one started
//   Linux     not offered yet (host_smb_none.cpp)
//
// A port (D9): elevate_mac.cpp, elevate_win.cpp, elevate_none.cpp.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace mv::driftwire::cli {

struct elevated_run {
  bool started = false;   // the OS ran it (the person approved)
  bool declined = false;  // the person said no, or no administrator was given
  std::string answer;     // its one line
  std::string why;        // when it did not run, in words
};

// Runs `program host smb-apply` elevated with `request` as its one line.
// `terminal`: a person is at this terminal (sudo); else the OS's dialog.
// Blocks until it ends (the person's answer to the OS first).
[[nodiscard]] elevated_run run_elevated(const std::string& program, std::string_view request, bool terminal);

// In the helper: the front end's line, from where this OS put it (stdin, or
// the pipe named by `channel`). False when there is none.
[[nodiscard]] bool helper_read(const std::string& channel, std::string& line);
// The answer, one line, the same way back.
[[nodiscard]] bool helper_write(const std::string& channel, const std::string& line);

}  // namespace mv::driftwire::cli
