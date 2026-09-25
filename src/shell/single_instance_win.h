// SPDX-License-Identifier: GPL-2.0-or-later
// PR 15 (plan/09 "Single-instance ... named pipe hands the path to the running
// instance, overridable"; plan/12 2026-09-25): a second MediaViewer started
// by the same user (Explorer, the jump list, a shortcut) hands its paths to
// the one already running, which opens them in its window, and exits.
// Multi-window (tabs) is its own later PR; this is the single instance.
//
// One pipe per user and session: \\.\pipe\MediaViewer.Viewer.<session>.<SID>.
// Local clients only, created FIRST_PIPE_INSTANCE so nothing can squat it.
// Windows host only.
#pragma once

#include <windows.h>

#include <string>
#include <thread>
#include <vector>

namespace mv::shell {

// The second instance: when a running MediaViewer accepted `paths` (made
// absolute here, so a relative argument means the caller's folder, not the
// running app's), it is allowed to take the foreground and true is returned:
// exit now. False: none is running (or it did not answer in time); carry on
// as the first instance.
[[nodiscard]] bool forward_to_running_instance(const std::vector<std::wstring>& paths) noexcept;

// The first instance. Listens on its own thread; each hand-off is posted to
// `window` as `message` with a heap std::wstring* in LPARAM (paths separated
// by '\n', possibly empty: "just come to the front"), which the receiver owns.
class instance_listener {
 public:
  instance_listener() = default;
  instance_listener(const instance_listener&) = delete;
  instance_listener& operator=(const instance_listener&) = delete;
  ~instance_listener() { stop(); }

  // False when another instance already owns the pipe.
  bool start(HWND window, UINT message) noexcept;
  void stop() noexcept;

 private:
  void run(HANDLE first_pipe) noexcept;

  HWND window_ = nullptr;
  UINT message_ = 0;
  HANDLE stop_event_ = nullptr;
  std::thread thread_;
};

}  // namespace mv::shell
