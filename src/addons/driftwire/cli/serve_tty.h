// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The two things `driftwire serve` asks of the terminal when it reads a
// passphrase: whether stdin is one, and its echo off while a line is typed.
// A port (D9): serve_tty_posix.cpp, serve_tty_win.cpp.
#pragma once

namespace mv::driftwire::cli {

[[nodiscard]] bool serve_stdin_is_terminal();

// Whether this process runs with more than an ordinary user's rights: root on
// POSIX, an elevated token on Windows. The service refuses to run so (its keys
// would be the administrator's, and it faces the internet).
[[nodiscard]] bool serve_is_privileged();

// Echo off on stdin for its lifetime when `hide` and stdin is a terminal;
// active() says whether it was turned off (and a newline is owed).
class serve_echo_off {
 public:
  explicit serve_echo_off(bool hide);
  ~serve_echo_off();
  serve_echo_off(const serve_echo_off&) = delete;
  serve_echo_off& operator=(const serve_echo_off&) = delete;
  [[nodiscard]] bool active() const noexcept { return active_; }

 private:
  bool active_ = false;
  unsigned char saved_[128] = {};  // what to put back: the termios, or the console mode (sizes checked there)
};

}  // namespace mv::driftwire::cli
