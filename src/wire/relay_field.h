// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The relay server field, "<address or name>:<port>#<key>" (#472 relays):
// read here for Driftwire's relayed transfers and the VPN add-on's Tunnel,
// which keep a field each (docs/plans/vpn-addon.md).
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/result.h"
#include "wire/link.h"
#include "wire/net.h"

namespace mv::driftwire {

// Each TCP connect of a round (a code's places, a relay server).
inline constexpr int kReachConnectMs = 5000;
// How a connect is made (net::connect; a stand-in in the tests).
using connect_fn = std::function<result<std::unique_ptr<link>>(const net::endpoint&, int timeout_ms)>;

// "<address>:<port>", or "<name>:<port>" resolved (the relay server only:
// the person typed it). nullopt: neither.
[[nodiscard]] std::optional<net::endpoint> relay_endpoint(const std::string& text);
// Every address "<address or name>:<port>" stands for, IPv4 and IPv6 (a
// name resolved once, here): what the VPN leaves out of its routes, since the
// dialer may reach the relay at any of them. Empty: neither.
[[nodiscard]] std::vector<net::address> relay_addresses(const std::string& text);
// The relay field, "<address or name>:<port>#<key>", split (not resolved);
// nullopt with `why` (the window's words) when it is not one.
struct relay_server_field {
  std::string at;   // "<address or name>:<port>"
  std::string key;  // what only a waiter there proves (splice.h)
};
[[nodiscard]] std::optional<relay_server_field> relay_server_of(const std::string& text, std::string& why);

}  // namespace mv::driftwire
