// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// No OS text recogniser on this platform (text_reader.h).
#include "addons/ai/text_reader.h"

namespace mv::ai {

std::unique_ptr<text_reader> make_text_reader() { return nullptr; }

}  // namespace mv::ai
