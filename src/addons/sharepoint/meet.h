// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Meeting on a four-word code (issue #472 PR 3): both sides publish where
// they can be reached on the DHT, find the other's, and punch through their
// NATs from the one UDP socket (udp_hub.h). The result is a reliable stream
// to the peer, over which the internet handshake runs as with a direct code
// (CPace on the code, then Noise; channel.h).
//
//   - What each side publishes (dht_item.h, under kTargets salts of its
//     role): its addresses on this network, its public address as the DHT
//     saw it (BEP 42, no STUN), its public IPv6, all at the hub's port; and
//     whether the DHT saw it on more than one port (a NAT that maps each
//     destination apart, "hard").
//   - Each side looks up the other role's kTargets items in parallel; the
//     first that opens under the code wins.
//   - Punching: each side sends probes to every address the other gave, and
//     to a few ports around the other's public port when that side's NAT
//     hands ports out in sequence (port prediction), every 100 ms for up to
//     kPunchMs. A probe carries a session tag from the code; a side answers
//     every hello with a reply, and probes back at once a path the item did
//     not name (a NAT's fresh mapping). The sender picks the first path a
//     reply came back on and offers it ("choose") until the receiver answers
//     ("chosen"); the receiver answers only the first path it was offered.
//     The hole stays open: the stream uses the same socket.
//   - The birthday punch (one hard NAT): the hard side opens kBirthdaySockets
//     more sockets on the hub and probes the easy side's public address from
//     each (every one a new mapping in its NAT); the easy side probes random
//     ports of the hard side's address. At kBirthdayRate a second each way
//     (the hard side's mappings open 10 at a time, all 256 by 2.6 s), the
//     ~1000 random probes of a 10 s punch meet about 97 times in 100. Every
//     extra socket but the stream's is closed; the stream's own goes with it.
//   - Every probe, all kinds together, at most kProbeRate a second each way:
//     the paths learned from hellos first, then the item's addresses and
//     predicted ports in turn.
//   - TCP simultaneous open (net::tcp_rendezvous, meet_options::tcp: a word
//     code's meeting, never a remembered device's in the background), beside the UDP punch: each
//     side publishes its TCP port, and both connect() from it to the other's
//     at each of its addresses while listening on it. Works where a NAT
//     keeps a TCP connection's port (most home routers), whatever it does to
//     UDP. The sender's choose goes on the first connection that opens if
//     UDP found nothing in its first 1.5 s; one pick only, either way: a
//     sender that offered a TCP connection waits for its answer there until
//     the punch ends, never falls back to UDP.
//   - Two hard NATs whose TCP ports are rewritten too, or a network with no
//     UDP, still do not meet; the window says so.
// Portable (D9).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "addons/sharepoint/dht.h"
#include "addons/sharepoint/udp_hub.h"

namespace mv::sharepoint::dht {

inline constexpr int kPunchMs = 10000;
inline constexpr int kPredictPorts = 16;
inline constexpr int kBirthdaySockets = 256;
inline constexpr int kBirthdayRate = 100;  // birthday probes a second, each way
inline constexpr int kProbeRate = 200;     // every probe a second, each way (the birthday's among them)

// The DHT's voters saw this machine on many ports of one address, none on
// most of them (client::looks_hard): its NAT maps each destination apart.
[[nodiscard]] bool seen_hard(const client& dht);

struct meet_options {
  // The tests: publish loopback addresses only.
  bool loopback = false;
  int wait_ms = 10 * 60 * 1000;  // how long to look for the other side
  // How long to punch once the other side's item is found. A remembered
  // device (#472 PR 4) notices a sender only at its next look, so the
  // sender punches that much longer.
  int punch_ms = kPunchMs;
  // Try TCP simultaneous open beside the UDP punch. Off unless asked: it
  // listens on every interface while it runs (a firewall prompt on Windows),
  // so a remembered device's background meetings leave it off.
  bool tcp = false;
  // The ports the easy side's birthday probes pick from (the tests: the
  // stand-in NAT's window).
  std::uint16_t birthday_lo = 1024, birthday_hi = 65535;
  // Between lookups that found nothing yet. The handover (#472) looks again
  // sooner: both sides start within moments of each other, and the session
  // waits only kRejoinMs.
  int retry_ms = 2000;
  // The items' sequence number is at least this (BEP 44 nodes keep only a
  // higher one): a meeting soon after a withdraw under the same keys.
  std::int64_t seq_floor = 0;
};

// What a meeting reports along the way, for the window ("Looking for the
// other computer on the DHT…").
using progress_fn = std::function<void(const std::string& ascii)>;

// The candidates this side publishes.
[[nodiscard]] std::vector<net::endpoint> own_candidates(const udp_hub& hub, const client& dht, bool loopback);

// Publish, find, punch. The stream to the peer, its id derived from the code.
[[nodiscard]] result<std::unique_ptr<link>> meet(const std::shared_ptr<udp_hub>& hub, client& dht, const keys& k, role me,
                                                 const meet_options& o, const std::function<bool()>& stop,
                                                 const progress_fn& progress);
// After the link is up (or a meeting gave up): this side's items say
// "gone", at a sequence number above `seq_floor`; the number used.
std::int64_t withdraw(client& dht, const keys& k, role me, const std::function<bool()>& stop,
                      std::int64_t seq_floor = 0);

}  // namespace mv::sharepoint::dht
