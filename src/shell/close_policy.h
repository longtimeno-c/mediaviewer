// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #452: whether the window's WM_CLOSE hides it into the background (an add-on, or
// the shell's own entry while `driftwire host` runs, keeps the process) or quits.
#pragma once

namespace mv::shell {

struct close_state {
  bool closing = false;        // the close is already under way
  bool quit_for_real = false;  // the icon's Quit, an update's restart
  bool lab_finished = false;   // the render thread has ended: a soak's or a harness's own close
  bool kept_alive = false;     // something is on the keep-alive list
};

// A finished lab has no canvas left to come back to, and the harnesses (--soak,
// --browse-soak, frametime, boot.ps1) wait for the process to exit: before this,
// every measured run on a PC that was hosting hid instead of quitting and hung.
[[nodiscard]] constexpr bool close_goes_to_background(const close_state& s) noexcept {
  return !s.closing && !s.quit_for_real && !s.lab_finished && s.kept_alive;
}

}  // namespace mv::shell
