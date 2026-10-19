// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A stand-in DHT swarm on loopback (#472 PR 3), for the DHT tests and the
// handover's (test_driftwire_session.cpp). No real DHT node is contacted.
#pragma once

#include <sodium.h>

#include <algorithm>
#include <map>

#include "addons/driftwire/bencode.h"
#include "addons/driftwire/dht.h"
#include "addons/driftwire/sha1.h"
#include "driftwire_fixture.h"

namespace bc = mv::driftwire::bencode;
namespace dht = mv::driftwire::dht;

namespace {

// A full DHT node, for the tests only: answers find_node, get and put (BEP 5,
// 44), checks a put's token and signature, says how it saw the asker (BEP 42
// "ip"). `forge`: answers every get with an item it signed itself.
struct swarm_node {
  std::shared_ptr<sp::udp_hub> hub;
  dht::node self;
  std::vector<dht::node>* everyone = nullptr;
  std::mutex m;
  struct stored {
    std::string k, sig, v;
    std::int64_t seq = 0;
  };
  std::map<std::string, stored> items;  // by target
  bool forge = false;
  std::atomic<int> gets{0};

  void on(std::span<const std::uint8_t> d, const sp::net::endpoint& from) {
    auto v = bc::decode({reinterpret_cast<const char*>(d.data()), d.size()});
    if (!v || !v->str("y") || *v->str("y") != "q" || !v->str("t") || !v->str("q")) return;
    const bc::value* a = v->get("a");
    if (!a) return;
    const std::string q = *v->str("q");
    bc::value r = bc::value::dict({{"id", bc::value::bytes(std::string(reinterpret_cast<const char*>(self.id.data()), 20))}});
    const std::string* target = a->str("target");
    if (q == "find_node" || q == "get") {
      std::vector<dht::node> close = *everyone;
      if (target && target->size() == 20) {
        dht::node_id t;
        std::memcpy(t.data(), target->data(), 20);
        std::sort(close.begin(), close.end(), [&](const dht::node& x, const dht::node& y) {
          dht::node_id dx, dy;
          for (int i = 0; i < 20; ++i) {
            dx[i] = x.id[i] ^ t[i];
            dy[i] = y.id[i] ^ t[i];
          }
          return dx < dy;
        });
      }
      if (close.size() > 8) close.resize(8);
      r.d.emplace_back("nodes", bc::value::bytes(dht::compact_nodes(close)));
    }
    if (q == "get" && target) {
      ++gets;
      r.d.emplace_back("token", bc::value::bytes("tok" + from.address + std::to_string(from.port)));
      std::lock_guard lock(m);
      if (forge) {
        r.d.emplace_back("k", bc::value::bytes(std::string(32, 'K')));
        r.d.emplace_back("seq", bc::value::num(999));
        r.d.emplace_back("sig", bc::value::bytes(std::string(64, 'S')));
        r.d.emplace_back("v", bc::value::bytes("forged"));
      } else if (auto it = items.find(*target); it != items.end()) {
        r.d.emplace_back("k", bc::value::bytes(it->second.k));
        r.d.emplace_back("seq", bc::value::num(it->second.seq));
        r.d.emplace_back("sig", bc::value::bytes(it->second.sig));
        r.d.emplace_back("v", bc::value::bytes(it->second.v));
      }
    }
    if (q == "put") {
      const std::string* tok = a->str("token");
      const std::string* k = a->str("k");
      const std::string* sig = a->str("sig");
      const auto seq = a->integer("seq");
      const bc::value* val = a->get("v");
      const std::string salt = a->str("salt") ? *a->str("salt") : std::string();
      if (!tok || *tok != "tok" + from.address + std::to_string(from.port) || !k || k->size() != 32 || !sig || !seq || !val) return;
      std::array<std::uint8_t, 32> key;
      std::memcpy(key.data(), k->data(), 32);
      if (!dht::verify(key, salt, *seq, bc::encode(*val), {reinterpret_cast<const std::uint8_t*>(sig->data()), sig->size()})) return;
      std::string kt = *k + salt;
      const auto t = mv::driftwire::sha1({reinterpret_cast<const std::uint8_t*>(kt.data()), kt.size()});
      std::lock_guard lock(m);
      auto& s = items[std::string(reinterpret_cast<const char*>(t.data()), 20)];
      if (*seq >= s.seq) s = {*k, *sig, val->s, *seq};
    }
    const auto ip = sp::net::parse_address(from.address);
    std::string compact(reinterpret_cast<const char*>(ip->bytes.data()), ip->v6 ? 16 : 4);
    compact.push_back(static_cast<char>(from.port >> 8));
    compact.push_back(static_cast<char>(from.port & 0xff));
    const std::string wire = bc::encode(bc::value::dict({{"t", bc::value::bytes(*v->str("t"))},
                                                        {"y", bc::value::bytes("r")},
                                                        {"r", std::move(r)},
                                                        {"ip", bc::value::bytes(compact)}}));
    (void)hub->send({reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size()}, from);
  }
};

struct swarm {
  std::vector<std::unique_ptr<swarm_node>> nodes;
  std::vector<dht::node> list;
  explicit swarm(int n) {
    for (int i = 0; i < n; ++i) {
      auto s = std::make_unique<swarm_node>();
      auto h = sp::udp_hub::open(0, true);
      REQUIRE(h);
      s->hub = std::move(*h);
      randombytes_buf(s->self.id.data(), 20);
      s->self.at = {"127.0.0.1", s->hub->port()};
      list.push_back(s->self);
      nodes.push_back(std::move(s));
    }
    for (auto& s : nodes) {
      s->everyone = &list;
      swarm_node* raw = s.get();
      s->hub->on_dht([raw](std::span<const std::uint8_t> d, const sp::net::endpoint& f) { raw->on(d, f); });
    }
  }
  ~swarm() {
    for (auto& s : nodes) s->hub->on_dht(nullptr);
  }
  sp::net::endpoint seed() const { return list.front().at; }
};

[[maybe_unused]] dht::client::options test_options(const swarm& s, std::string table = {}) {
  dht::client::options o;
  o.use_routers = false;
  o.extra = {s.seed()};
  o.table_path = std::move(table);
  o.query_ms = 500;
  o.lookup_ms = 5000;
  return o;
}

}  // namespace
