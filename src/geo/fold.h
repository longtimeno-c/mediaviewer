// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Case- and diacritic-insensitive key for a name or a typed word.
//
// The place names fold with the one rule the app's searches share, core/fold.h
// (mv::text::fold: decompose, drop non-spacing marks, lowercase; ł, ø, ß, æ
// stay themselves). The rule began here (#344) and moved to core with #291, so
// "portugal" means the same in file search, Local search and Places.
#pragma once

#include <string>
#include <string_view>

#include "core/fold.h"

namespace mv::geo {

[[nodiscard]] inline std::string fold(std::string_view utf8) { return mv::text::fold(utf8); }

}  // namespace mv::geo
