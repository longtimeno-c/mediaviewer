// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// power.h on iOS: there is no assertion against sleep below UIKit. Keeping a
// transfer alive is the host's job (a background task, the idle timer), so the
// port answers as an OS that refused: power.h's "the transfer runs anyway".
#include "addons/driftwire/power.h"

namespace mv::driftwire::power {

std::unique_ptr<stay_awake> hold(bool) noexcept { return nullptr; }

}  // namespace mv::driftwire::power
