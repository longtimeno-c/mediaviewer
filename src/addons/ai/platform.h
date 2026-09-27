// SPDX-License-Identifier: GPL-2.0-or-later
// The few OS facts the AI pack needs that the host table does not carry:
// where its own library lives (the Core pack's folder: ORT and the models sit
// beside it), whether the machine is on battery (plan/17 "Yield policy"), and
// the lowest scheduling class for its workers (plan/17 "inference workers,
// lowest priority, THREAD_MODE_BACKGROUND_BEGIN for I/O as well"). One TU per
// OS (D9): platform_win.cpp, platform_posix.cpp.
#pragma once

#include <cstdint>
#include <string>

namespace mv::ai::platform {

// The folder holding this add-on's shared library, UTF-8, no trailing slash.
[[nodiscard]] std::string self_dir();

struct power {
  bool on_battery = false;
  int percent = 100;  // 0..100; 100 when unknown or on AC
};
[[nodiscard]] power power_state() noexcept;

// Called once at the top of each worker thread: lowest CPU and I/O priority.
void enter_background() noexcept;

[[nodiscard]] unsigned hardware_threads() noexcept;

}  // namespace mv::ai::platform
