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
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "addons/driftwire/acme.h"
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
  // The name the relay's web-site look answers as (the person's own domain, else the DuckDNS name;
  // "": none): the certificate keeper gets Let's Encrypt's for it.
  std::string cert_name;
  bool loopback_only = false;        // the tests: never a real router, never duckdns.org
  // A keeper's news that the public address may have moved (the router keeper
  // saw a new outside address): the host passes it to every other keeper.
  // Any thread; never blocks.
  std::function<void()> address_moved;
  // The drives keeper's news (host_drives.h): the worst combined set's health
  // word for the phones ("": healthy or none), passed to serve's options::storage.
  // Any thread; never blocks.
  std::function<void(std::string)> storage_changed;
  // The certificate keeper's news: a new certificate is in <folder>/tls (the
  // relay takes it). Any thread; never blocks.
  std::function<void()> cert_changed;
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
  std::function<std::unique_ptr<host_keeper>()> drives;  // combined drives' health (host_drives.h)
  std::function<std::unique_ptr<host_keeper>()> cert;    // Let's Encrypt for the web-site look (vpn-stealth)
};

// The keepers this build has (host_keepers.cpp): DuckDNS over cli::https_get,
// the router on the default gateway, and the combined drives' health.
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
// service's port while it serves; with the VPN on, the relay's web-site look
// at outside 443 (else kLookFallbackPort), or the plain relay's port when the
// look is off.
inline constexpr std::uint16_t kLookFallbackPort = 8443;

// ---- the certificate keeper (docs/plans/vpn-stealth.md §1) ---------------------------------
//
// With the VPN's web-site look on and a DuckDNS name (and its token), gets a
// Let's Encrypt certificate for keeper_context::cert_name by DNS-01 (the TXT
// through DuckDNS; the person's own domain CNAMEs `_acme-challenge` to the
// DuckDNS name's), into <folder>/tls (account.pem, cert.pem, key.pem; mode
// 600), and tells the host (cert_changed). Checked at start and daily;
// renewed 30 days before the end. After a failure: 1, 2, 4 … 24 hours (or
// the CA's Retry-After, if longer), and never more than kOrdersPerDay orders
// in 24 hours (Let's Encrypt allows 5 failed validations an hour, 5 of the
// same certificate a week). The relay keeps its self-signed one until then.
inline constexpr int kOrdersPerDay = 3;
inline constexpr std::int64_t kRenewBeforeS = 30 * 86400;
struct cert_keeper_config {
  acme::http_fn http;                         // empty: cli::https_request
  ddns::fetch_fn fetch;                       // DuckDNS's TXT; empty: cli::https_get, its error text dropped
  std::string directory = acme::kLetsEncrypt;
  std::string ddns_base = ddns::kDuckDnsBase;
  std::function<std::int64_t()> now_unix;     // empty: the clock
  int propagation_ms = 60000;                 // the TXT reaching Let's Encrypt's resolvers
  int poll_ms = 3000;
  bool on_loopback = false;                   // the tests' stand-in CA and DuckDNS: runs with loopback_only too
};
[[nodiscard]] std::unique_ptr<host_keeper> make_cert_keeper(cert_keeper_config c = {});
// The certificate in <folder>/tls when it is for `name` and still good at
// `now` (the relay's start takes it): its two files' paths.
struct cert_files {
  std::string cert, key;
  std::int64_t not_after = 0;
};
[[nodiscard]] std::optional<cert_files> usable_certificate(const std::string& folder, const std::string& name,
                                                           std::int64_t now);
[[nodiscard]] std::vector<host_ports::port_wanted> wanted_ports(const keeper_context& ctx);
// "3 min ago", "just now", "2 h ago" (for status lines).
[[nodiscard]] std::string ago_words(std::int64_t then_unix, std::int64_t now_unix);

}  // namespace mv::driftwire::cli
