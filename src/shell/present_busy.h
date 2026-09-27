// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Whether the present loop is presenting frames (panning, zooming, playing,
// loading). The Mac host's add-on table reads it so a background import
// waits between buffers (plan/18 "Priority"); the Windows host reaches the
// same rule through mv_present_set_busy. One relaxed atomic, never a lock.
#pragma once

#include <atomic>

namespace mv::shell {
inline std::atomic<bool> g_present_busy{false};
}  // namespace mv::shell
