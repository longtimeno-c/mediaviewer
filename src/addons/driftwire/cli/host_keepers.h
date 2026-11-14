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
// This file is the interface only: slice 2 builds none. A slice that builds
// one returns it from its factory in host_keepers() (host_cmd.cpp), nothing
// else in the host changes. A keeper never blocks start() or stop() on the
// network, never logs an address, token or name of a file (rule 6), and never
// runs in the viewer or on a phone.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace mv::driftwire::secrets {
class store;
}

namespace mv::driftwire::cli {

struct keeper_context {
  std::string folder;                // <data dir>/host: host.json is here; a keeper may keep a file of its own
  secrets::store* secrets = nullptr; // the host's own store (<folder>/secrets): ddns_token goes here
  std::uint16_t service_port = 0;    // serve's TCP port (0: not serving now: no folder shared)
  std::uint16_t relay_port = 0;      // the relay's TCP port
  bool vpn = false;                  // forwarding on: the relay's port must be reachable from outside too
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
};

struct keeper_factories {
  std::function<std::unique_ptr<host_keeper>()> ddns;    // slice 3
  std::function<std::unique_ptr<host_keeper>()> router;  // slice 4
};

// The keepers this build has (host_cmd.cpp): none until slices 3 and 4.
[[nodiscard]] keeper_factories host_keepers();

}  // namespace mv::driftwire::cli
