// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The terminal, behind a port (D9: term_posix.cpp for Linux and macOS,
// term_win.cpp for the Windows console). The driftwire command line reads
// bytes and writes bytes; everything it draws and decodes is above this.
//
//   raw mode   no echo, no line editing, Ctrl+C and Ctrl+Z arrive as keys;
//              bracketed paste on. On Windows also VT sequences both ways
//              and UTF-8 (code page 65001). Restored by leave_raw(), by the
//              destructor and at exit, whatever the way out.
//   wait       blocks until input, a wake() from another thread (an engine
//              callback), a signal or a resize, or the timeout; nothing
//              spins while nothing happens (the idle cost is the engine's).
//   signals    POSIX: SIGTERM / SIGHUP quit, SIGWINCH resizes, SIGTSTP from
//              outside suspends, SIGCONT resumes (through a self-pipe), SIGPIPE
//              is ignored. Windows: the console closing, logoff, shutdown and
//              Ctrl+Break quit; the handler holds the process for the quit
//              (the OS's grace is 5 s) until the session has restored the
//              console and the engine has stopped.
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "addons/driftwire/cli/inline_view.h"

namespace mv::driftwire::cli {

struct term_size {
  int columns = 80;
  int rows = 24;
};

// What one wait brought (several at once is possible).
struct term_input {
  std::string bytes;       // typed or pasted, as the terminal sent them
  bool woken = false;      // wake()
  bool resized = false;
  bool quit = false;       // SIGTERM, SIGHUP; the console closing, logoff, shutdown, Ctrl+Break
  bool interrupt = false;  // SIGINT / Ctrl+C outside raw mode (in raw mode Ctrl+C is a key)
  bool suspend = false;    // SIGTSTP from outside
  bool resumed = false;    // SIGCONT after a suspend
  bool eof = false;        // the input is closed
  bool timed_out = false;
};

// The terminal as the session sees it: the console, or a fake in the tests.
class terminal : public output {
 public:
  [[nodiscard]] virtual term_size size() const = 0;
  // Waits up to `timeout_ms` (< 0: no limit) and fills `in` (cleared first).
  virtual void wait(term_input& in, int timeout_ms) = 0;
  // Any thread, any time, also from inside an engine callback: the wait
  // returns with `woken`. Never blocks.
  virtual void wake() noexcept = 0;
};

class console final : public terminal {
 public:
  struct options {
    bool signals = true;  // install the signal hooks (one console at a time does)
  };

  // Standard input and output are both a terminal.
  [[nodiscard]] static bool is_terminal() noexcept;
  // The terminal takes UTF-8: the locale's character set (POSIX); the
  // console's output code page, which enter_raw() sets to 65001 (Windows).
  [[nodiscard]] static bool utf8() noexcept;

  console();
  explicit console(options o);
#if !defined(_WIN32)
  // A terminal on other descriptors (a pty in the tests); not closed here.
  console(int in_fd, int out_fd, options o);
#endif
  ~console() override;
  console(const console&) = delete;
  console& operator=(const console&) = delete;

  bool enter_raw();
  void leave_raw() noexcept;
  [[nodiscard]] bool raw() const noexcept;
  // Ctrl+Z, or SIGTSTP from outside: hands the terminal back and stops the
  // process; returns once it is continued, in raw mode again. The caller
  // erases its live region before and draws it again after. Windows has no
  // job control: nothing happens.
  void suspend_self();

  [[nodiscard]] term_size size() const override;
  void wait(term_input& in, int timeout_ms) override;
  void wake() noexcept override;
  void write(std::string_view bytes) override;

 private:
  struct impl;
  std::unique_ptr<impl> p_;
};

}  // namespace mv::driftwire::cli
