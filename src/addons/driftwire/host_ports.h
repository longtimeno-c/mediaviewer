// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The self-host's router keeper (docs/plans/self-hosted.md §5, slice 4): while
// `driftwire host` runs, the router forwards the host's own TCP ports (the
// service's, and the relay's when the VPN is on) to this computer, the
// service's for UDP too (its sessions over UDP, at the same outside port), and says
// where the home faces the internet from.
//
// It is mapping.* held for a set of ports: PCP → NAT-PMP → UPnP-IGD, short
// leases renewed by mapping::keeper, an IGDv2 IPv6 pinhole where the router
// offers one, every forward removed when this goes (a crash: the ledger, and
// the next start removes what is left). A port someone else already forwards
// (the person's own 443) is never taken over: the mapper refuses it, and a
// port's fallback is asked instead. It talks to the gateway only.
//
// This is the one place `serve`'s "never touches a router's mapping" bends
// (owner's call 2, 2026-11-14, docs/design/12-decision-log.md): the host's
// own ports only, never SMB or RDP, never another program's.
//
// Using it (the supervisor, cli/host_cmd.*), on a worker thread:
//
//   host_ports::options o;
//   o.ledger = data_dir + "/host/router.ledger";
//   o.on_outside_changed = [&](const net::address&) { ddns.kick(); };  // any thread
//   std::vector<host_ports::port_wanted> want = {{.internal_port = 7878, .what = "service"}};
//   if (vpn) want.push_back({.internal_port = relay_tls_port, .external_port = 443,
//                            .fallback_external = relay_port, .what = "relay"});
//   auto ports = std::make_unique<host_ports::keeper>(std::move(want), std::move(o));  // blocks
//   ... host_ports::advice(ports->status()) printed once; status() for `host status` ...
//   ports.reset();  // the forwards are removed: blocks while an ask in flight ends
//                   // (up to ~2 x timeout_ms each for the watch thread's UPnP ask and a
//                   // renewal), then up to a second a forward
//
// Rule 6: notes and advice() carry no address and may be logged; the
// addresses in a report (`outside`, `this_computer`) are for the person's own
// screen only, never a log, a crash report or telemetry. Portable (D9).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "addons/driftwire/mapping.h"
#include "wire/net.h"

namespace mv::driftwire::host_ports {

// What the router did with one port.
enum class port_state : std::uint8_t {
  not_offered,  // no router answered PCP, NAT-PMP or UPnP (or no router at all)
  refused,      // a router answered and said no, or faces the internet from an address that is not public
  mapped,       // forwarded (renewed until the keeper goes)
};
[[nodiscard]] const char* port_state_name(port_state s) noexcept;  // "not offered", "refused", "mapped"

// Where the router faces the internet from.
enum class outside_kind : std::uint8_t {
  unknown,         // no router said (none found, or none speaks a mapping protocol)
  public_address,  // a forward works
  double_nat,      // a private address (RFC 1918 and the like): another router beyond this one
  carrier_nat,     // 100.64/10 (RFC 6598) or DS-Lite's 192.0.0/29: the provider's NAT; no forward can help
  not_public,      // not public and neither of those (a non-public IPv6 outside address)
};
[[nodiscard]] const char* outside_kind_name(outside_kind k) noexcept;
// The kind of an outside IPv4 address.
[[nodiscard]] outside_kind classify(const net::address& outside) noexcept;

// Whether a port may ever be forwarded: never NetBIOS (137-139), SMB (445) or
// RDP (3389), whoever asks (docs/plans/self-host-users-smb.md §6): the LAN
// share is for the home network only. The keeper refuses such a port before
// it talks to any router.
[[nodiscard]] bool forwardable(std::uint16_t port) noexcept;
inline constexpr const char* kNeverForwarded = "never forwarded (a LAN-only service)";

struct port_wanted {
  std::uint16_t internal_port = 0;      // what this computer listens on
  std::uint16_t external_port = 0;      // asked of the router (0: the same as internal)
  // Asked when a router answered but would not give `external_port` itself
  // (443 is someone else's forward, or it offered another port; that other
  // one is removed first). Its answer is kept whatever port it is. 0: no
  // fallback, and whatever port the router offered for `external_port` is
  // kept (the report says which).
  std::uint16_t fallback_external = 0;
  // An IPv6 pinhole too, at `internal_port` (a pinhole does not translate),
  // if this computer has a public IPv6 address and the router offers IGDv2.
  bool v6 = true;
  // UDP as well, at the outside port TCP got (the phone dials the TCP route's port over UDP):
  // one the router gives elsewhere is removed, as a phone could not find it.
  bool udp = false;
  std::string what;                     // "service", "relay": for the advice ("" says "a port")
};

struct port_report {
  port_wanted wanted;
  port_state v4 = port_state::not_offered;
  mapping::method how = mapping::method::none;
  std::uint16_t external_port = 0;      // when mapped: the port a phone or laptop dials
  // nullopt: not asked (not wanted, no public IPv6 address here, or PCP
  // answered: UPnP is not searched for a pinhole besides, #499).
  std::optional<port_state> v6;
  // nullopt: not asked (not wanted, or TCP was not mapped). The IPv6 pinhole for UDP goes with the TCP one.
  std::optional<port_state> udp;
  std::vector<std::string> why;         // the mapper's notes for this port, no addresses
};

struct report {
  std::vector<port_report> ports;
  outside_kind kind = outside_kind::unknown;
  std::optional<net::address> outside;        // the router's outside IPv4 (screen only, rule 6)
  std::optional<net::address> this_computer;  // toward the gateway: where a hand forward points (screen only)
  bool router_found = false;                  // a default gateway exists
  bool cancelled = false;                     // `router.cancelled` ended the start: states are partial
};

struct options {
  // The gateway (none: the default one), timeouts, cancelled. `cancelled` is
  // kept for renewals too (once true, they stop quietly), so whatever it
  // captures must outlive the keeper.
  mapping::options router;
  int lease_s = 120;                  // each forward's lease, renewed at half (internet.h's kMapLifetimeS)
  std::string ledger;                 // mapping::keeper's ledger ("": none); leftovers there are removed first
  std::optional<net::address> local_v6;  // the tests; none: this computer's first public IPv6 address
  int check_every_ms = 60'000;        // how often the router is asked its outside address
  // Called on the keeper's own thread when the router's outside IPv4 differs
  // from the last one seen and is public (the DuckDNS keeper updates at once;
  // a move behind carrier NAT shows in status() instead, never as a name's
  // address). Not called for the first address found at start; called when
  // an address first appears after a start that found none. Must not block
  // for long, and must not destroy the keeper.
  std::function<void(const net::address& outside)> on_outside_changed;
};

class keeper {
 public:
  // Asks the router for every port (blocks: a few seconds a port at worst,
  // less with `router.cancelled`), then holds what it got. A worker thread's
  // call, never the UI's.
  keeper(std::vector<port_wanted> ports, options o);
  // Every forward and pinhole removed (best effort), the ledger gone.
  ~keeper();
  keeper(const keeper&) = delete;
  keeper& operator=(const keeper&) = delete;

  // A copy, any thread. A lease the router stops renewing is not reflected
  // (mapping::keeper asks again soon); `outside` and `kind` follow the
  // router's answers every `check_every_ms`.
  [[nodiscard]] report status() const;

 private:
  void watch();

  options opts_;
  mutable std::mutex m_;
  report report_;  // under m_
  std::unique_ptr<mapping::keeper> held_;
  mapping::options ask_;                    // the watch thread's: cancelled on stop too
  std::unique_ptr<mapping::mapper> asker_;  // the watch thread's own
  std::condition_variable cv_;
  std::atomic<bool> stop_{false};
  std::thread watch_;
};

// What to tell the person, plain lines with no addresses (a `host setup` or
// `host status` prints them; empty when every port is mapped on a public
// address): the one port to forward by hand when mapping failed; on carrier
// NAT that no forward can help and a relay on a rented server is needed
// (plan §5.4); on double NAT, the outer router.
[[nodiscard]] std::vector<std::string> advice(const report& r);

}  // namespace mv::driftwire::host_ports
