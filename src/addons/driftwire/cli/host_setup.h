// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// `driftwire host setup` and its parts (docs/plans/self-hosted.md §2, slices 5
// and 7): the guided run that makes this computer the phones' server and the
// laptop's VPN exit, in the order a person needs them:
//
//   1. a name      a DuckDNS subdomain and its token (typed or pasted, never
//                  an argument), checked by one update and then by resolving
//                  the name; skipped: phones reach it at home only (said so)
//   2. a folder    `host share`, and optionally a backup folder with a limit
//   3. the router  host_ports.* asked for the service's port (and the
//                  relay's with the VPN on); the one port to forward by hand
//                  when it says no; carrier NAT said plainly (§5.4)
//   4. a phone     the pairing QR in the terminal, the DuckDNS name as the
//                  public route (`host pair` alone)
//   5. the VPN     `host vpn on`: forwarding on, and the relay line
//                  `name.duckdns.org:PORT#KEY` for the laptop's
//                  Relay (your own server) field
//
// and start at login. `host name duckdns|none`, `host pair` and `host vpn
// on|off` run one part alone.
//
// What it changes is the host's files (host.json, serve.json, the secret
// stores), which a running host takes within seconds; the router probe is
// removed when it ends (the running host keeps its own). It prints no
// address but in the relay line and the QR, which are for the person's own
// screen; nothing here is logged (rule 6).
//
// The host library: a phone needs a library to pair with. With no desktop
// library on the service, `host pair` makes the host's own (an id and a
// publisher key kept in the host's secrets) and writes its library.json in
// <data dir>/library/.mediaviewer, shared as the "Catalogue" folder, so the
// phone opens the shared folders in file mode (docs/plans/phone-in-the-app.md
// §4), as it does a NAS's before Local search has published.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "addons/driftwire/cli/args.h"
#include "addons/driftwire/cli/host_cmd.h"
#include "addons/driftwire/ddns.h"
#include "addons/driftwire/host_ports.h"
#include "wire/net.h"

namespace mv::driftwire::cli {

inline constexpr std::int64_t kHostPairSeconds = 15 * 60;  // a terminal QR's code
inline constexpr const char* kHostPublisherSecret = "host_publisher_seed";

// The person at the terminal.
struct setup_io {
  // One line typed, its newline gone (hidden: echo off, a secret); nullopt
  // when the input ended.
  std::function<std::optional<std::string>(const std::string& prompt, bool hidden)> ask;
  std::function<void(const std::string& text)> say;  // printed as it is
  bool colour = true;                                // the QR's own black on white
};

// What setup talks to. The real one: duckdns.org through cli::https_get, the
// system's resolver, the default gateway. The tests: stand-ins on loopback.
struct setup_world {
  ddns::fetch_fn fetch;                         // empty: cli::https_get (its error text dropped)
  std::string ddns_base = ddns::kDuckDnsBase;
  ddns::resolve_fn resolve;                     // empty: net::resolve
  int name_tries = 9;                           // the name resolved again until it matches (a fresh
  int name_wait_ms = 10000;                     //   DuckDNS name can take its 60 s TTL to show)
  bool ask_router = true;                       // false: step 3 says the router was not asked
  host_ports::options router;                   // the gateway (none: the default), timeouts
  std::optional<net::address> lan;              // this computer on the home network (none: toward the gateway)
  bool loopback = false;                        // the test build: 127.0.0.1, never duckdns.org or a router
  bool any_port = false;                        // serve.json's port 0 (the test build)
  std::function<std::int64_t()> now_unix;       // empty: the clock
  std::function<void(int ms)> sleep;            // empty: this thread sleeps
  std::function<int(const args&)> start_at_login;  // empty: host_files::start_at_login (the test build refuses)
};

// A phone's pairing code for this host.
struct host_pair_code {
  std::string url;    // mediaviewer://pair#… (what the QR holds)
  std::string check;  // the check the phone shows (pairing_check)
  std::int64_t expires = 0;
  std::string local, public_name;  // the routes in it: "address:port", "name.duckdns.org:port"
};
// Makes the host library if there is none, a single-use code, and the
// payload (catalog::pair_url's shape). nullopt with `why` (no folder shared,
// the keys cannot be read).
[[nodiscard]] std::optional<host_pair_code> make_host_pair(const host_layout& l, const setup_world& w, std::string& why);

// The relay line for the laptop: "name.duckdns.org:PORT#KEY" (the LAN
// address without a name). `port`: the outside port the router gave, else
// the relay's own. "" with `why` when the key cannot be had.
[[nodiscard]] std::string host_relay_line(const host_layout& l, const setup_world& w, std::string& why);

// The parts and the whole run; each returns the program's exit code.
int host_name(const host_layout& l, bool duckdns, setup_io& io, const setup_world& w);
int host_pair(const host_layout& l, setup_io& io, const setup_world& w);
int host_vpn(const host_layout& l, bool on, std::uint16_t tls_port, setup_io& io, const setup_world& w);
int host_setup(const host_layout& l, setup_io& io, const setup_world& w);

// From run_host: the terminal as the setup_io (a terminal needed, but in the
// test build), the real world (loopback in the test build with --test-loopback).
int run_host_setup(const args& a, const host_layout& l, bool test_hooks);
int run_host_name(const args& a, const host_layout& l, bool test_hooks);
int run_host_pair(const args& a, const host_layout& l, bool test_hooks);
int run_host_vpn(const args& a, const host_layout& l, bool test_hooks);

}  // namespace mv::driftwire::cli
