// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// make_bluetooth where there is no Bluetooth bearer (the portable build): the
// window says so and Wi-Fi works alone.
#include "addons/sharepoint/presence.h"

namespace mv::sharepoint {

std::unique_ptr<bluetooth> make_bluetooth() { return nullptr; }

}  // namespace mv::sharepoint
