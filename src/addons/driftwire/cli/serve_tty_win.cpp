// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/serve_tty.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>

namespace mv::driftwire::cli {

bool serve_stdin_is_terminal() {
  DWORD mode = 0;
  return ::GetConsoleMode(::GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
}

serve_echo_off::serve_echo_off(bool hide) {
  DWORD mode = 0;
  HANDLE in = ::GetStdHandle(STD_INPUT_HANDLE);
  if (!hide || !::GetConsoleMode(in, &mode)) return;
  static_assert(sizeof(DWORD) <= sizeof saved_);
  std::memcpy(saved_, &mode, sizeof mode);
  ::SetConsoleMode(in, mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT));
  active_ = true;
}

serve_echo_off::~serve_echo_off() {
  if (!active_) return;
  DWORD mode = 0;
  std::memcpy(&mode, saved_, sizeof mode);
  ::SetConsoleMode(::GetStdHandle(STD_INPUT_HANDLE), mode);
}

}  // namespace mv::driftwire::cli
