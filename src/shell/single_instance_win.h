// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 15 (docs/design/09 "Single-instance ... named pipe hands the path to the running
// instance, overridable"; docs/design/12 2026-09-25): a second MediaViewer started
// by the same user (Explorer, the jump list, a shortcut) hands its paths to
// the one already running, which opens them in its window, and exits.
// Multi-window (tabs) is its own later PR; this is the single instance.
//
// One pipe per user and session: \\.\pipe\MediaViewer.Viewer.<session>.<SID>.
// Local clients only, created FIRST_PIPE_INSTANCE with a DACL that admits
// this user only. The name is computable by any account, so a second start
// sends its paths only to a server process running as this user; anything
// else holding the name gets nothing and this start runs alone.
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
// exit now. False: none is running (or it did not answer in time, or the
// pipe's server is not this user); carry on as the first instance.
[[nodiscard]] bool forward_to_running_instance(const std::vector<std::wstring>& paths) noexcept;

// The first instance claims the name as soon as it knows it is one, long
// before its window exists. A second start in that gap (Explorer starts one
// process per selected file) then connects, and its paths wait in the pipe's
// buffer until the listener starts, instead of opening a second window.
class instance_claim {
 public:
  instance_claim() = default;
  instance_claim(const instance_claim&) = delete;
  instance_claim& operator=(const instance_claim&) = delete;
  ~instance_claim();

  // False when another process already owns the name: forward to it instead.
  [[nodiscard]] bool claim() noexcept;
  [[nodiscard]] bool claimed() const noexcept { return pipe_ != INVALID_HANDLE_VALUE; }

 private:
  friend class instance_listener;
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
};

// The first instance. Listens on its own thread; each hand-off is posted to
// `window` as `message` with a heap std::wstring* in LPARAM (paths separated
// by '\n', possibly empty: "just come to the front"), which the receiver owns.
class instance_listener {
 public:
  instance_listener() = default;
  instance_listener(const instance_listener&) = delete;
  instance_listener& operator=(const instance_listener&) = delete;
  ~instance_listener() { stop(); }

  // Takes over the claimed pipe. False when nothing was claimed.
  bool start(HWND window, UINT message, instance_claim& claim) noexcept;
  void stop() noexcept;

 private:
  void run(HANDLE first_pipe) noexcept;

  HWND window_ = nullptr;
  UINT message_ = 0;
  HANDLE stop_event_ = nullptr;
  std::thread thread_;
};

}  // namespace mv::shell
