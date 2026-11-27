// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #452: whether the window's WM_CLOSE hides it into the background (an add-on, or
// the shell's own entry while `driftwire host` runs, keeps the process) or quits.
#pragma once

namespace mv::shell {

struct close_state {
  bool closing = false;        // the close is already under way
  bool quit_for_real = false;  // the icon's Quit, an update's restart
  bool harness_run = false;    // --soak, --browse-soak, a self-test, --boot-report
  bool kept_alive = false;     // something is on the keep-alive list
};

// A harness run closes itself when its report is written, and frametime, regenerate.py
// and boot.ps1 wait for the process to exit: before this, every measured run on a PC
// that was hosting (`driftwire host`) hid instead of quitting and never exited.
[[nodiscard]] constexpr bool close_goes_to_background(const close_state& s) noexcept {
  return !s.closing && !s.quit_for_real && !s.harness_run && s.kept_alive;
}

}  // namespace mv::shell
