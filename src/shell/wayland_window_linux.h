// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Linux present lab's window (Linux PR 1): a raw Wayland xdg toplevel on
// the UI thread, with no toolkit. Pointer and keyboard (xkbcommon) go into the
// shared input_snapshot the render thread consumes, as the Win32 window proc
// and the AppKit view do; the window's state (activated, suspended, size,
// fractional scale, the outputs it is on) does too, read on the UI thread so
// the render thread never touches the window (docs/plans/linux.md §2).
//
// Linux PR 3 replaces this with Qt's window and a child surface; the lab only
// ever sees the wl_display and wl_surface it is given, and before_commit().
// No Wayland type in this header.
#pragma once

#include <cstdint>
#include <memory>

#include "core/result.h"
#include "shell/input_state.h"

namespace mv::shell {

class wayland_window_linux {
 public:
  wayland_window_linux();
  ~wayland_window_linux();
  wayland_window_linux(const wayland_window_linux&) = delete;
  wayland_window_linux& operator=(const wayland_window_linux&) = delete;

  // [UI thread] Connects to $WAYLAND_DISPLAY, maps an xdg toplevel of
  // `logical_width` x `logical_height` and waits (bounded) for its first
  // configure. unsupported_format when there is no Wayland compositor or it
  // lacks xdg-shell.
  [[nodiscard]] expected open(const char* title, std::uint32_t logical_width,
                              std::uint32_t logical_height) noexcept;
  void close() noexcept;

  // wl_display* / wl_surface*, borrowed by the lab. Valid until close().
  [[nodiscard]] void* display() const noexcept;
  [[nodiscard]] void* surface() const noexcept;

  // [UI thread] One pass of the UI loop: flush, wait up to `timeout_ms` (< 0:
  // no limit) for the connection or `extra_fd`, dispatch the default queue.
  // False when the connection is gone. `*extra_ready` says extra_fd woke it.
  [[nodiscard]] bool dispatch(int timeout_ms, int extra_fd, bool* extra_ready) noexcept;
  [[nodiscard]] bool close_requested() const noexcept;

  // [UI thread] The input and window state as of the last dispatch, and
  // whether it changed since the last take_changed().
  [[nodiscard]] const input_snapshot& snapshot() const noexcept;
  [[nodiscard]] bool take_changed() noexcept;

  // [any thread] The refresh of the outputs the surface is on, in mHz, or 0
  // when they disagree or none has said (the cloned-output rule, docs/design/03).
  [[nodiscard]] std::uint32_t refresh_mhz() const noexcept;

  // [render thread] Right before the commit of a `width` x `height` buffer:
  // acks the newest configure of that size and sets the viewport (or buffer
  // scale) that goes with it, so the size, the scale and the ack land in the
  // same commit.
  void before_commit(std::uint32_t width, std::uint32_t height) noexcept;

  struct impl;

 private:
  std::unique_ptr<impl> impl_;
};

}  // namespace mv::shell
