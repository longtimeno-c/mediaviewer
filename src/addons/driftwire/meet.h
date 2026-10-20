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
//     kPunchMs. A probe carries a session tag from the code and this side's
//     nonce for the meeting, and a MAC under a key from the code (#499: a
//     reply, choose and chosen over both sides' nonces), so only a holder of
//     the code can answer or pick; a side answers every hello that verifies
//     with a reply, and probes back at once a path the item did not name (a
//     NAT's fresh mapping). The other side's private addresses are probed
//     only when both share a public address (one router). The sender picks the first path a
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
//     code's meeting, and a remembered device's once a sender knocks), beside the UDP punch: each
//     side publishes its TCP port, and both connect() from it to the other's
//     at each of its addresses while listening on it. Works where a NAT
//     keeps a TCP connection's port (most home routers), whatever it does to
//     UDP. Both sides write a hello on each connection; the sender's choose
//     goes on the first connection whose hello verifies if UDP found nothing
//     in its first 1.5 s, and waits for the chosen there until the punch
//     ends. A connection that closes or answers anything else was not taken,
//     so the sender goes on to the next path that answers (#499).
//   - Not kept before the handshake (#499, meet_options::open / later): when
//     the handshake over the path agreed on fails, the sender agrees on the
//     next path a reply came back on, under the next attempt number, and the
//     receiver hands that one on as a later link.
//   - Two hard NATs whose TCP ports are rewritten too, or a network with no
//     UDP, still do not meet; the window says so.
// Portable (D9).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "addons/driftwire/dht.h"
#include "addons/driftwire/udp_hub.h"

namespace mv::driftwire::dht {

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
  // How long to punch once the other side's item is found.
  int punch_ms = kPunchMs;
  // Try TCP simultaneous open beside the UDP punch. Off unless asked: it
  // listens on every interface while it runs (a firewall prompt on Windows).
  // A word code's meeting and a remembered device's ask (the latter only
  // once a sender knocks, never while it waits).
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
  // #499, nothing kept before the handshake holds. The sender: each path
  // agreed on goes to `open` (which runs the handshake over it); true ends
  // the meeting (it took the link, or failed in a way no other path mends)
  // and meet() returns an empty link; false drops that path, and the next
  // one a reply came back on is agreed on, at most three in all
  // (status::permission_denied when none held). Unset: the first path
  // agreed on is returned.
  std::function<bool(std::unique_ptr<link>&)> open;
  // The receiver: a path the sender agrees on after meet() returned (its
  // handshake over the last one failed), for a minute after. On the hub's
  // reader thread: brief, never blocking. Unset: later picks go unanswered.
  std::function<void(std::unique_ptr<link>)> later;
};

// What a meeting reports along the way, for the window ("Looking for the
// other computer on the DHT…").
using progress_fn = std::function<void(const std::string& ascii)>;

// The candidates this side publishes.
[[nodiscard]] std::vector<net::endpoint> own_candidates(const udp_hub& hub, const client& dht, bool loopback);

// Publish, find, punch. The stream to the peer, its id from both sides'
// nonces (with o.open: an empty link once open took one).
[[nodiscard]] result<std::unique_ptr<link>> meet(const std::shared_ptr<udp_hub>& hub, client& dht, const keys& k, role me,
                                                 const meet_options& o, const std::function<bool()>& stop,
                                                 const progress_fn& progress);
// Which of `candidates` (a word code's keys for the hour now and the hour
// before, #499) the other side published under: the first whose item of
// role `other` opens, looked for until `wait_ms` passes or `stop`. nullopt
// when none was found.
[[nodiscard]] std::optional<std::size_t> published_under(client& dht, const std::vector<const keys*>& candidates,
                                                         role other, int wait_ms, int retry_ms,
                                                         const std::function<bool()>& stop);
// After the link is up (or a meeting gave up): this side's items say
// "gone", at a sequence number above `seq_floor`; the number used.
std::int64_t withdraw(client& dht, const keys& k, role me, const std::function<bool()>& stop,
                      std::int64_t seq_floor = 0);

}  // namespace mv::driftwire::dht
