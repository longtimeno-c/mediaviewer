// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The nearby service on the local network (docs/plans/mobile.md §7.5): one
// Bonjour registration of _mediaviewer._tcp, held for as long as this lives.
// Never a broadcast of ours: iOS would need the multicast entitlement.
//   advert_mac.cpp   dns_sd (mDNSResponder)
//   advert_win.cpp   DnsServiceRegister (Windows' own mDNS responder)
//   advert_none.cpp  no advert yet (Linux, iOS): the service still runs
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mv::driftwire::advert {

inline constexpr const char* kServiceType = "_mediaviewer._tcp";

class registration {
 public:
  virtual ~registration() = default;
};

// nullptr where there is no advert or the registration failed: not an error
// to the caller, a phone then simply does not find this desktop.
[[nodiscard]] std::unique_ptr<registration> publish(std::uint16_t port,
                                                    const std::vector<std::pair<std::string, std::string>>& txt);

}  // namespace mv::driftwire::advert
