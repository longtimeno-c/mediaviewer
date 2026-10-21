// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #530: while bytes move, neither machine goes to sleep from idleness: a
// transfer holds a request for the system to stay awake (the display may
// still turn off and the screen lock). The port behind it is per OS:
// PowerCreateRequest / PowerSetRequest(PowerRequestSystemRequired) on
// Windows, an IOKit assertion (PreventUserIdleSystemSleep) on the Mac,
// nothing in the Linux test build. It cannot stop a closed lid, a shutdown
// or a flat battery: a receive that stops then is resumed (resume.h).
#pragma once

#include <memory>

namespace mv::driftwire::power {

class stay_awake {
 public:
  virtual ~stay_awake() = default;  // released
};

// Held until the result is destroyed; nullptr when the OS refused (the
// transfer runs anyway). `receiving`: the reason the OS shows (powercfg
// /requests, pmset -g assertions) says which way.
[[nodiscard]] std::unique_ptr<stay_awake> hold(bool receiving) noexcept;

}  // namespace mv::driftwire::power
