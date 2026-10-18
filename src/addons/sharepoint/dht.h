// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A small BitTorrent DHT client (BEP 5, 42, 43, 44) for Sharepoint's opt-in
// word codes (issue #472 PR 3). The spike asked "libtorrent-rasterbar or a
// small KRPC + BEP 44/42 client in the add-on": this is the latter. All
// Sharepoint needs is to put and get two signed items and to learn its own
// public address; libtorrent brings Boost and a torrent engine for that, and
// owns its sockets, where this must share the punching socket (udp_hub.h).
//
//   - Read-only (BEP 43, "ro": 1): other nodes do not put it in their
//     routing tables, it answers no queries, and it is in the DHT only while
//     a send or receive waits on it (never at idle).
//   - Lookups are iterative (Kademlia): three queries in flight, towards the
//     eight closest nodes that answer. Each item found is checked: its key
//     hashes to the target, its signature verifies, the newest sequence
//     number wins. Fake nodes can delay a lookup, never read or forge an
//     item (dht_item.h seals what is in it).
//   - BEP 42: every answer's "ip" (how the node saw us) is a vote for this
//     machine's public address, so no STUN server is needed; nodes whose id
//     matches their address are preferred when a lookup starts.
//   - Bootstrap: a few well-known routers, a routing table saved between
//     runs (after the first run the routers are rarely needed), or a node
//     the caller names (a LAN peer, a remembered device).
// Nothing is logged with an address. Portable (D9).
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "addons/sharepoint/bencode.h"
#include "addons/sharepoint/dht_item.h"
#include "addons/sharepoint/udp_hub.h"

namespace mv::sharepoint::dht {

using node_id = std::array<std::uint8_t, 20>;

struct node {
  node_id id{};
  net::endpoint at;
  bool secure = false;  // BEP 42: its id matches its address
};

// BEP 42's id prefix check (crc32c of the masked address).
[[nodiscard]] bool id_matches(const node_id& id, const net::address& ip) noexcept;
[[nodiscard]] std::uint32_t crc32c(std::span<const std::uint8_t> data) noexcept;
// Compact node lists: 26 bytes each (IPv4), 38 each (IPv6).
[[nodiscard]] std::vector<node> parse_nodes(std::string_view compact, bool v6);
[[nodiscard]] std::string compact_nodes(const std::vector<node>& nodes);  // both kinds, as BEP 5 / 32 write them
[[nodiscard]] std::optional<net::endpoint> parse_compact_endpoint(std::string_view compact);

inline const std::vector<std::pair<std::string, std::uint16_t>>& bootstrap_routers() {
  static const std::vector<std::pair<std::string, std::uint16_t>> kRouters = {
      {"router.bittorrent.com", 6881}, {"dht.transmissionbt.com", 6881}, {"router.utorrent.com", 6881},
      {"dht.libtorrent.org", 25401}};
  return kRouters;
}

class client {
 public:
  struct options {
    // Where to start, besides the saved table: the routers (resolved when
    // the client starts) and any node the caller names.
    bool use_routers = true;
    std::vector<net::endpoint> extra;
    std::string table_path;  // "" keeps nothing between runs
    int query_ms = 1500;
    int lookup_ms = 15000;
  };
  client(std::shared_ptr<udp_hub> hub, options o);
  ~client();  // saves the table
  client(const client&) = delete;
  client& operator=(const client&) = delete;

  // Fills the table from the saved one, the routers and `extra`; false when
  // no node answered.
  [[nodiscard]] bool start(const std::function<bool()>& stop);

  struct item {
    std::string v;  // the value's bytes (a bencoded string's contents)
    std::int64_t seq = 0;
  };
  // BEP 44 get of a mutable item: the newest that verifies, if any.
  [[nodiscard]] std::optional<item> get(std::span<const std::uint8_t, 32> key, std::string_view salt,
                                        const std::function<bool()>& stop);
  // BEP 44 put: stored on the closest nodes that answered; how many took it.
  int put(const keys& k, std::string_view salt, std::int64_t seq, const std::string& v,
          const std::function<bool()>& stop);
  // BEP 42 votes: each node's latest answer of how it saw us, for
  // kVoteMaxAgeMs (a network change or an old lie ages out), at most
  // kMaxVoters of them.
  static constexpr std::int64_t kVoteMaxAgeMs = 10 * 60 * 1000;
  static constexpr std::size_t kMaxVoters = 256;
  // This machine as the DHT sees it (the most voters), once any answer said.
  [[nodiscard]] std::optional<net::endpoint> external() const;
  // The distinct endpoints the voters saw.
  [[nodiscard]] std::vector<net::endpoint> external_votes() const;
  // How many nodes have voted.
  [[nodiscard]] int vote_count() const;
  // The NAT maps each destination apart (a "hard" NAT, birthday punching):
  // for the most-seen address, at least three ports, and no port seen by
  // half the voters. One node's lie, or two's, does not make it so.
  [[nodiscard]] bool looks_hard() const;
  [[nodiscard]] std::size_t known() const;

 private:
  struct pending {
    std::optional<bencode::value> reply;
    net::endpoint from;
  };
  struct contact {
    node n;
    bool asked = false, answered = false;
    std::string token;
  };
  [[nodiscard]] std::optional<bencode::value> query(const net::endpoint& to, const std::string& q, bencode::value args,
                                                    const std::function<bool()>& stop);
  void on_datagram(std::span<const std::uint8_t> d, const net::endpoint& from);
  // Iterative lookup towards `target`; `ask` is the query each contact gets
  // ("get" or "find_node"); the contacts that answered, closest first.
  std::vector<contact> lookup(const node_id& target, const std::string& ask, const std::function<bool()>& stop,
                              const std::function<void(const contact&, const bencode::value&)>& on_reply);
  void learn(const std::vector<node>& nodes);
  void load_table();
  void save_table();

  std::shared_ptr<udp_hub> hub_;
  options opts_;
  node_id self_{};
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::map<std::string, std::shared_ptr<pending>> pending_;  // by transaction id
  std::uint16_t next_tid_ = 0;
  std::vector<node> table_;                                 // under m_, at most kTable
  struct vote {
    net::endpoint seen;
    std::chrono::steady_clock::time_point at;
  };
  std::map<std::string, vote> votes_;  // under m_: by the voter's "address|port"
  [[nodiscard]] std::vector<const vote*> recent_votes() const;  // under m_
};

}  // namespace mv::sharepoint::dht
