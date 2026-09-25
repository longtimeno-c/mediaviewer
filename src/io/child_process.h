// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The child-process port: a helper program with its stdin and stdout piped to
// the caller (plan/08 "Execution & UX": encode jobs run in a child process, so
// a crash in an encoder cannot take the viewer down, and cancelling is a clean
// kill).
//
// Portable header; io/child_process_win.cpp (CreateProcessW, a kill-on-close
// job object so the helper dies with the app) and io/child_process_posix.cpp
// (posix_spawn; macOS and the Linux core tests) implement it. No HANDLE or pid
// in the interface (D9).
//
// Worker threads only: start, read_line and wait block (rule 1). write_line,
// close_stdin and kill may be called from another thread while one is reading.
// Nothing here logs a path or an argument (rule 6).
#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "core/result.h"

namespace mv::io {

class child_process {
 public:
  child_process();
  ~child_process();  // kills a child still running and reaps it
  child_process(const child_process&) = delete;
  child_process& operator=(const child_process&) = delete;

  // Starts `exe_utf8` with `args` (not including argv[0]). status::io if it
  // cannot be started. The child inherits the environment, nothing else.
  [[nodiscard]] expected start(std::string_view exe_utf8, std::span<const std::string> args);

  // One line to the child's stdin ('\n' appended). False once the pipe is gone.
  bool write_line(std::string_view line) noexcept;
  void close_stdin() noexcept;

  // The next line of the child's stdout, without its '\n' (a trailing '\r' is
  // dropped too). False at end of stream.
  [[nodiscard]] bool read_line(std::string& out);

  // Ends the child now (SIGKILL / TerminateProcess).
  void kill() noexcept;

  // Waits for the child. Its exit code when it exited; -1 when it was killed
  // or crashed (a signal, an unhandled exception) or never started.
  [[nodiscard]] int wait() noexcept;

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

}  // namespace mv::io
