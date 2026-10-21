// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Linux core test build: no sleep to hold off (power.h).
#include "addons/driftwire/power.h"

namespace mv::driftwire::power {

std::unique_ptr<stay_awake> hold(bool /*receiving*/) noexcept { return nullptr; }

}  // namespace mv::driftwire::power
