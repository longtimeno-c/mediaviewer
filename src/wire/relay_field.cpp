// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "wire/relay_field.h"

#include <algorithm>

#include "wire/splice.h"

namespace mv::driftwire {

namespace {

std::optional<net::endpoint> parse_hostport(const std::string& text) {
  // "203.0.113.5:41000" or "[2001:db8::5]:41000".
  std::string host;
  std::string port;
  if (!text.empty() && text.front() == '[') {
    const auto close = text.find(']');
    if (close == std::string::npos || close + 1 >= text.size() || text[close + 1] != ':') return std::nullopt;
    host = text.substr(1, close - 1);
    port = text.substr(close + 2);
  } else {
    const auto colon = text.rfind(':');
    if (colon == std::string::npos) return std::nullopt;
    host = text.substr(0, colon);
    port = text.substr(colon + 1);
  }
  if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    return std::nullopt;
  }
  const unsigned long p = std::stoul(port);
  const auto ip = net::parse_address(host);
  if (!ip || p == 0 || p > 65535) return std::nullopt;
  return net::endpoint{net::address_text(*ip), static_cast<std::uint16_t>(p)};
}

}  // namespace

std::optional<net::endpoint> relay_endpoint(const std::string& text) {
  if (auto e = parse_hostport(text)) return e;
  const auto colon = text.rfind(':');
  if (colon == std::string::npos || colon == 0 || text.front() == '[') return std::nullopt;
  const std::string host = text.substr(0, colon);
  const auto port = parse_hostport("0.0.0.0" + text.substr(colon));
  if (!port || host.size() > 253) return std::nullopt;
  const auto all = net::resolve(host);
  if (all.empty()) return std::nullopt;
  // IPv4 first: a sender's network has it more often than IPv6.
  const auto v4 = std::find_if(all.begin(), all.end(), [](const net::address& a) { return !a.v6; });
  return net::endpoint{net::address_text(v4 != all.end() ? *v4 : all.front()), port->port};
}

std::vector<net::address> relay_addresses(const std::string& text) {
  if (const auto e = parse_hostport(text)) {
    if (const auto a = net::parse_address(e->address)) return {*a};
    return {};
  }
  const auto colon = text.rfind(':');
  if (colon == std::string::npos || colon == 0 || text.front() == '[') return {};
  const std::string host = text.substr(0, colon);
  if (!parse_hostport("0.0.0.0" + text.substr(colon)) || host.size() > 253) return {};
  return net::resolve(host);
}

std::optional<relay_server_field> relay_server_of(const std::string& text, std::string& why) {
  const auto hash = text.rfind('#');
  if (hash == std::string::npos || text.size() - hash - 1 < relay::kMinKeyChars) {
    why = "Your relay server needs its key after the address: <address>:<port>#<key>, as the server prints it.";
    return std::nullopt;
  }
  relay_server_field f{text.substr(0, hash), text.substr(hash + 1)};
  const auto colon = f.at.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= f.at.size()) {
    why = "Your relay server's address is not one (address or name, a colon, the port, then #key).";
    return std::nullopt;
  }
  return f;
}

}  // namespace mv::driftwire
