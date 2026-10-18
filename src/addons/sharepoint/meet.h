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
//     every probe with its tag. The first address that answers both ways is
//     the path, and its hole stays open: the stream uses the same socket.
//   - Not built here: the birthday punch for a hard NAT (256 sockets on the
//     hard side, needs a hub that polls many sockets), and TCP simultaneous
//     open. Two hard NATs, or a network with no UDP, do not meet this way;
//     the window says so.
// Portable (D9).
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "addons/sharepoint/dht.h"
#include "addons/sharepoint/udp_hub.h"

namespace mv::sharepoint::dht {

inline constexpr int kPunchMs = 10000;
inline constexpr int kPredictPorts = 16;

struct meet_options {
  // The tests: publish loopback addresses only.
  bool loopback = false;
  int wait_ms = 10 * 60 * 1000;  // how long to look for the other side
  // How long to punch once the other side's item is found. A remembered
  // device (#472 PR 4) notices a sender only at its next look, so the
  // sender punches that much longer.
  int punch_ms = kPunchMs;
  // Between lookups that found nothing yet. The handover (#472) looks again
  // sooner: both sides start within moments of each other, and the session
  // waits only kRejoinMs.
  int retry_ms = 2000;
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
// After the link is up: this side's items say "gone".
void withdraw(client& dht, const keys& k, role me, const std::function<bool()>& stop);

}  // namespace mv::sharepoint::dht
