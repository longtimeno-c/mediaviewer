// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The folder the Enhance add-on's library was loaded from: its verified
// version folder, where its catalogue sits beside it (models/enhance.json,
// models/<id>/model.json, manifest.json; catalog.h). A port (D9):
// self_dir_posix.cpp (dladdr) and self_dir_win.cpp (GetModuleHandleExW), as
// Places does (src/addons/places/self_dir.h).
#pragma once

#include <string>

namespace mv::enhance {

// UTF-8, no trailing separator; empty when it cannot be found.
[[nodiscard]] std::string self_dir();

}  // namespace mv::enhance
