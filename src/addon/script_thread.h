// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A thread with a stack of a size we choose, for a script add-on's worker
// (script_runtime.h). std::thread takes the platform default, which is 512 KB
// for a secondary thread on macOS: Lua's C recursion limit (LUAI_MAXCCALLS,
// 200 levels of pattern matching, parser or resume nesting) wants more
// headroom than that, deterministically on both OSes. _win / _posix halves (D9).
#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace mv::addon {

class script_thread {
 public:
  script_thread() = default;
  ~script_thread();  // joins
  script_thread(const script_thread&) = delete;
  script_thread& operator=(const script_thread&) = delete;

  // Starts `body` on a new thread whose stack is at least `stack_bytes`.
  // False when the OS would not create one (nothing runs).
  [[nodiscard]] bool start(std::size_t stack_bytes, std::function<void()> body);
  void join() noexcept;
  [[nodiscard]] bool joinable() const noexcept { return handle_ != nullptr; }

  struct impl;  // the OS half's

 private:
  impl* handle_ = nullptr;
};

}  // namespace mv::addon
