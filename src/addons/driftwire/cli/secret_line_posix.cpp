// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/secret_line.h"

#include <termios.h>
#include <unistd.h>

#include <cstdio>

namespace mv::driftwire::cli {

bool read_secret_line(std::string& out, bool hidden, std::size_t max, bool& echo_was_off) {
  out.clear();
  out.reserve(max + 1);
  termios before{};
  echo_was_off = hidden && ::isatty(STDIN_FILENO) && ::tcgetattr(STDIN_FILENO, &before) == 0;
  if (echo_was_off) {
    termios quiet = before;
    quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
    (void)::tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
  }
  bool got = false;
  for (int c; (c = std::fgetc(stdin)) != EOF;) {
    got = true;
    if (c == '\n') break;
    if (out.size() < max) out.push_back(static_cast<char>(c));
  }
  if (!out.empty() && out.back() == '\r') out.pop_back();
  if (echo_was_off) (void)::tcsetattr(STDIN_FILENO, TCSAFLUSH, &before);
  return got;
}

bool stdin_is_terminal() { return ::isatty(STDIN_FILENO) != 0; }

}  // namespace mv::driftwire::cli
