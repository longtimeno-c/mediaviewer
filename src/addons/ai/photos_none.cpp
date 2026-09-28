// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// No Photos library source on this platform (photos_source.h): Windows has no
// PhotoKit, and iCloud for Windows syncs to a folder an ordinary root covers.
#include "addons/ai/photos_source.h"

namespace mv::ai {

std::unique_ptr<photos_source> make_photos_source() { return nullptr; }

}  // namespace mv::ai
