// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/secret_line.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>

namespace mv::driftwire::cli {

bool read_secret_line(std::string& out, bool hidden, std::size_t max, bool& echo_was_off) {
  out.clear();
  out.reserve(max + 1);
  HANDLE in = ::GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode = 0;
  echo_was_off = hidden && ::GetConsoleMode(in, &mode) != 0;
  if (echo_was_off) ::SetConsoleMode(in, mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT));
  bool got = false;
  for (int c; (c = std::fgetc(stdin)) != EOF;) {
    got = true;
    if (c == '\n') break;
    if (out.size() < max) out.push_back(static_cast<char>(c));
  }
  if (!out.empty() && out.back() == '\r') out.pop_back();
  if (echo_was_off) ::SetConsoleMode(in, mode);
  return got;
}

bool stdin_is_terminal() {
  DWORD mode = 0;
  return ::GetConsoleMode(::GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
}

}  // namespace mv::driftwire::cli
