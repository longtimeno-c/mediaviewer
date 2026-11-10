// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Seams for src/mobile's tests only. Nothing in the app sets them.
#pragma once

#include <functional>

namespace mv::mobile::testing {

// Runs inside mv_mobile_search_start while it holds its start lock, so a test can keep a start
// in progress. Empty: nothing runs. Set it before the start begins and clear it after.
void set_start_hook(std::function<void()> hook);

}  // namespace mv::mobile::testing
