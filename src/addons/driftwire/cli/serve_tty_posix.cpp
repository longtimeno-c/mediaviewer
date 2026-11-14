// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/serve_tty.h"

#include <termios.h>
#include <unistd.h>

#include <cstring>

namespace mv::driftwire::cli {

static_assert(sizeof(termios) <= 128, "serve_echo_off keeps the termios in 128 bytes");

bool serve_stdin_is_terminal() { return ::isatty(STDIN_FILENO) != 0; }

serve_echo_off::serve_echo_off(bool hide) {
  termios before{};
  if (!hide || !::isatty(STDIN_FILENO) || ::tcgetattr(STDIN_FILENO, &before) != 0) return;
  std::memcpy(saved_, &before, sizeof before);
  termios quiet = before;
  quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
  (void)::tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
  active_ = true;
}

serve_echo_off::~serve_echo_off() {
  if (!active_) return;
  termios before{};
  std::memcpy(&before, saved_, sizeof before);
  (void)::tcsetattr(STDIN_FILENO, TCSAFLUSH, &before);
}

}  // namespace mv::driftwire::cli
