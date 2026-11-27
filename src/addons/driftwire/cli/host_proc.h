// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// `driftwire host`'s supervisor runs the services in a process of their own
// (the same program, `host child`), so a crash in them is a restart 30 s
// later, not the end of hosting (docs/plans/self-hosted.md §3). This is the
// little it needs of the OS: start a child whose stdin is a pipe it holds
// (closing it asks the child to stop, and a supervisor that dies closes it
// too), wait for it, and kill it. A port (D9): host_proc_posix.cpp
// (posix_spawn), host_proc_win.cpp (CreateProcessW, no window).
//
// It also holds the two other things the host asks of the OS: whether stdin
// is at its end (the child's stop signal), and on Windows giving up a
// console nobody started it from (Task Scheduler's at login).
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace mv::driftwire::cli {

class child_process {
 public:
  struct ended {
    bool crashed = false;  // a signal (POSIX) or an exception code (Windows), not an exit
    int code = 0;          // the exit code, or the signal / exception
  };

  // `program` with `args` (UTF-8, not including the program); its stdout and
  // stderr are this process's. nullptr when it could not be started.
  [[nodiscard]] static std::unique_ptr<child_process> start(const std::string& program,
                                                            const std::vector<std::string>& args);
  virtual ~child_process() = default;  // kills it if it is still running

  // Waits up to `ms` (0: just looks); true once it has ended (then how()).
  [[nodiscard]] virtual bool wait(int ms) = 0;
  [[nodiscard]] virtual ended how() const = 0;
  [[nodiscard]] virtual long pid() const = 0;
  // Closes its stdin: the child stops by itself.
  virtual void ask_to_stop() = 0;
  virtual void kill() = 0;
};

// Blocks until stdin is at its end (or fails); for the child's watcher thread.
void wait_for_stdin_end();

// Windows: a console made for this process alone (Task Scheduler's, at login)
// is let go, so no window stays; a terminal someone typed in is kept.
// Elsewhere nothing.
void leave_own_console();

}  // namespace mv::driftwire::cli
