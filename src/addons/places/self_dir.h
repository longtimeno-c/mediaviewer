// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The folder the Places add-on's library was loaded from: its verified
// install folder, where its data (data/places.mvgeo) sits beside it. A port
// (D9): self_dir_posix.cpp (dladdr) and self_dir_win.cpp
// (GetModuleHandleExW), as the AI pack does (src/addons/ai/platform.h).
#pragma once

#include <string>

namespace mv::places {

// UTF-8, no trailing separator; empty when it cannot be found.
[[nodiscard]] std::string self_dir();

}  // namespace mv::places
