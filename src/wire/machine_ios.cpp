// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// net::machine_name on iOS. UIDevice.name has answered only "iPhone" since
// iOS 16 without a special entitlement, and the host's name is "localhost":
// the kind of device is all there is to say here.
#include <sys/sysctl.h>

#include <string>
#include <string_view>

#include "wire/net.h"
#include "wire/wire.h"

namespace mv::driftwire::net {

std::string machine_name() {
  char model[64] = {};
  std::size_t n = sizeof model - 1;
  const bool ipad = sysctlbyname("hw.machine", model, &n, nullptr, 0) == 0 && std::string_view(model).starts_with("iPad");
  return clean_name(ipad ? "iPad" : "iPhone");
}

}  // namespace mv::driftwire::net
