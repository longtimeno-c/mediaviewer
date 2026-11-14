// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Where the host's keepers plug in (docs/plans/self-hosted.md §3–§5): the
// DuckDNS keeper (slice 3, `ddns::keeper` in src/addons/driftwire/ddns.*)
// keeps a name pointed at the home's address, and the router keeper (slice 4,
// host_ports.*) asks the router for the host's ports. Each runs on a thread of
// its own inside the host's service process, started after the service and
// the relay are listening (it needs their ports) and stopped before them (a
// router mapping goes before the port it opened stops answering).
//
// Slice 2 left the interface; slice 5 builds the two keepers behind it
// (host_keepers.cpp): make_ddns_keeper around ddns::keeper and
// make_router_keeper around host_ports::keeper, each rebuilding on a worker of
// its own when the ports, the VPN or the name change, so neither start() nor
// ports_changed() waits on the network. host_keepers() returns them for the
// real host; with loopback_only (the test build) they stay idle unless built
// for a stand-in (`on_loopback`). A keeper never logs an address, token or name
// of a file (rule 6), and never runs in the viewer or on a phone.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "addons/driftwire/ddns.h"
#include "addons/driftwire/host_ports.h"

namespace mv::driftwire::secrets {
class store;
}

namespace mv::driftwire::cli {

struct keeper_context {
  std::string folder;                // <data dir>/host: host.json is here; a keeper may keep a file of its own
  secrets::store* secrets = nullptr; // the host's own store (<folder>/secrets): ddns_token goes here
  std::uint16_t service_port = 0;    // serve's TCP port (0: not serving now: no folder shared)
  std::uint16_t relay_port = 0;      // the relay's TCP port
  std::uint16_t relay_tls_port = 0;  // the relay's web-site look (0: none; only with the VPN on)
  bool vpn = false;                  // forwarding on: the relay's port must be reachable from outside too
  std::string ddns_subdomain;        // host.json's DuckDNS name ("": none); the token is in `secrets`
  bool loopback_only = false;        // the tests: never a real router, never duckdns.org
  // A keeper's news that the public address may have moved (the router keeper
  // saw a new outside address): the host passes it to every other keeper.
  // Any thread; never blocks.
  std::function<void()> address_moved;
};

class host_keeper {
 public:
  virtual ~host_keeper() = default;  // stopped first if it was not
  // Starts its thread and returns at once.
  virtual void start(const keeper_context& ctx) = 0;
  // Ends its thread (undoing what it holds, a router mapping), within seconds.
  virtual void stop() noexcept = 0;
  // The ports changed (serve started or stopped, the VPN turned on): the
  // context again, with the rest as it was. Never blocks.
  virtual void ports_changed(const keeper_context& ctx) noexcept = 0;
  // The address may have moved (keeper_context::address_moved). Never blocks.
  virtual void address_may_have_moved() noexcept {}
  // One line for `host status` ("DuckDNS  name.duckdns.org, updated 3 min ago"):
  // a name the person chose may be in it, never an address or a token.
  [[nodiscard]] virtual std::string status_line() const = 0;
  // Numbers for state.json (the router keeper: the outside ports a phone and
  // the laptop dial, for `host setup`, `host pair` and `host vpn on`). Never
  // an address.
  [[nodiscard]] virtual std::vector<std::pair<std::string, std::int64_t>> state_numbers() const { return {}; }
};

struct keeper_factories {
  std::function<std::unique_ptr<host_keeper>()> ddns;    // slice 3
  std::function<std::unique_ptr<host_keeper>()> router;  // slice 4
};

// The keepers this build has (host_keepers.cpp): DuckDNS over cli::https_get,
// and the router on the default gateway.
[[nodiscard]] keeper_factories host_keepers();

// ---- the two keepers (slice 5) -------------------------------------------------------------

struct ddns_keeper_config {
  ddns::fetch_fn fetch;                 // empty: cli::https_get, its error text dropped
  std::string base = ddns::kDuckDnsBase;
  ddns::timing when{};
  bool on_loopback = false;             // the tests' stand-in DuckDNS: runs with loopback_only too
  std::function<std::int64_t()> now_unix;  // status_line's "updated N min ago" (empty: the clock)
};
[[nodiscard]] std::unique_ptr<host_keeper> make_ddns_keeper(ddns_keeper_config c = {});

struct router_keeper_config {
  host_ports::options ports;  // the router (none: the default gateway), lease, checks; the ledger is set here
  bool on_loopback = false;   // the tests' stand-in routers: runs with loopback_only too
  // While a port stays closed (no router yet, a refusal, no answer), asked again after these,
  // the last repeated: 1, 5, then every 15 minutes.
  std::vector<int> retry_ms = {60'000, 300'000, 900'000};
};
[[nodiscard]] std::unique_ptr<host_keeper> make_router_keeper(router_keeper_config c = {});

// What the router is asked for, from the context (pure; the tests): the
// service's port while it serves; the relay's, and its web-site look at
// outside 443 (falling back to its own number), only with the VPN on.
[[nodiscard]] std::vector<host_ports::port_wanted> wanted_ports(const keeper_context& ctx);
// "3 min ago", "just now", "2 h ago" (for status lines).
[[nodiscard]] std::string ago_words(std::int64_t then_unix, std::int64_t now_unix);

}  // namespace mv::driftwire::cli
