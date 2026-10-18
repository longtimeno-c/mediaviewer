// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/sharepoint/dht.h"

#include <sodium.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>

#include "addons/sharepoint/sha1.h"

namespace mv::sharepoint::dht {

namespace {

using clock = std::chrono::steady_clock;

constexpr std::size_t kTable = 512;
constexpr std::size_t kK = 8;     // the closest nodes a lookup settles on
constexpr int kAlpha = 3;         // queries in flight
constexpr std::size_t kMaxV = 1000;  // BEP 44

node_id distance(const node_id& a, const node_id& b) noexcept {
  node_id d;
  for (std::size_t i = 0; i < d.size(); ++i) d[i] = a[i] ^ b[i];
  return d;
}

std::string as_string(std::span<const std::uint8_t> b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

std::string compact_endpoint(const net::endpoint& e) {
  const auto ip = net::parse_address(e.address);
  if (!ip) return {};
  std::string s(reinterpret_cast<const char*>(ip->bytes.data()), ip->v6 ? 16 : 4);
  s.push_back(static_cast<char>(e.port >> 8));
  s.push_back(static_cast<char>(e.port & 0xff));
  return s;
}

}  // namespace

std::uint32_t crc32c(std::span<const std::uint8_t> data) noexcept {
  std::uint32_t c = 0xffffffffu;
  for (const std::uint8_t b : data) {
    c ^= b;
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1u)));
  }
  return ~c;
}

bool id_matches(const node_id& id, const net::address& ip) noexcept {
  // BEP 42: crc32c((ip & mask) | (r << 29)), r = id[19] & 7; the top 21 bits of id.
  std::uint8_t masked[8];
  std::size_t n = 0;
  static constexpr std::uint8_t kMask4[4] = {0x03, 0x0f, 0x3f, 0xff};
  static constexpr std::uint8_t kMask6[8] = {0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0x7f, 0xff};
  if (ip.v6) {
    n = 8;
    for (std::size_t i = 0; i < 8; ++i) masked[i] = ip.bytes[i] & kMask6[i];
  } else {
    n = 4;
    for (std::size_t i = 0; i < 4; ++i) masked[i] = ip.bytes[i] & kMask4[i];
  }
  const std::uint8_t r = id[19] & 7;
  masked[0] |= static_cast<std::uint8_t>(r << 5);
  const std::uint32_t c = crc32c({masked, n});
  return id[0] == static_cast<std::uint8_t>(c >> 24) && id[1] == static_cast<std::uint8_t>(c >> 16) &&
         (id[2] & 0xf8) == (static_cast<std::uint8_t>(c >> 8) & 0xf8);
}

std::vector<node> parse_nodes(std::string_view compact, bool v6) {
  const std::size_t each = v6 ? 38 : 26;
  std::vector<node> out;
  if (compact.size() % each != 0 || compact.size() > each * 64) return out;
  for (std::size_t at = 0; at < compact.size(); at += each) {
    node n;
    std::memcpy(n.id.data(), compact.data() + at, 20);
    net::address ip;
    ip.v6 = v6;
    std::memcpy(ip.bytes.data(), compact.data() + at + 20, v6 ? 16 : 4);
    const std::size_t p = at + 20 + (v6 ? 16 : 4);
    const auto port = static_cast<std::uint16_t>(static_cast<std::uint8_t>(compact[p]) << 8 |
                                                  static_cast<std::uint8_t>(compact[p + 1]));
    if (port == 0) continue;
    n.at = {net::address_text(ip), port};
    n.secure = id_matches(n.id, ip);
    out.push_back(n);
  }
  return out;
}

std::string compact_nodes(const std::vector<node>& nodes) {
  std::string s;
  for (const auto& n : nodes) {
    const std::string e = compact_endpoint(n.at);
    if (e.empty()) continue;
    s.append(reinterpret_cast<const char*>(n.id.data()), 20);
    s += e;
  }
  return s;
}

std::optional<net::endpoint> parse_compact_endpoint(std::string_view c) {
  if (c.size() != 6 && c.size() != 18) return std::nullopt;
  net::address ip;
  ip.v6 = c.size() == 18;
  std::memcpy(ip.bytes.data(), c.data(), ip.v6 ? 16 : 4);
  const std::size_t p = ip.v6 ? 16 : 4;
  const auto port =
      static_cast<std::uint16_t>(static_cast<std::uint8_t>(c[p]) << 8 | static_cast<std::uint8_t>(c[p + 1]));
  if (port == 0) return std::nullopt;
  return net::endpoint{net::address_text(ip), port};
}

client::client(std::shared_ptr<udp_hub> hub, options o) : hub_(std::move(hub)), opts_(std::move(o)) {
  (void)sodium_init();
  randombytes_buf(self_.data(), self_.size());
  hub_->on_dht([this](std::span<const std::uint8_t> d, const net::endpoint& from) { on_datagram(d, from); });
  load_table();
}

client::~client() {
  hub_->on_dht(nullptr);
  save_table();
}

void client::on_datagram(std::span<const std::uint8_t> d, const net::endpoint& from) {
  auto v = bencode::decode({reinterpret_cast<const char*>(d.data()), d.size()});
  if (!v || v->k != bencode::value::kind::dict) return;
  const std::string* t = v->str("t");
  const std::string* y = v->str("y");
  if (!t || !y || (*y != "r" && *y != "e")) return;  // read-only: queries to us go unanswered
  std::lock_guard lock(m_);
  const auto it = pending_.find(*t);
  if (it == pending_.end() || it->second->reply) return;
  // Only the node asked may answer its transaction.
  if (from.address != it->second->from.address || from.port != it->second->from.port) return;
  it->second->reply = std::move(*v);
  cv_.notify_all();
}

std::optional<bencode::value> client::query(const net::endpoint& to, const std::string& q, bencode::value args,
                                            const std::function<bool()>& stop) {
  auto p = std::make_shared<pending>();
  p->from = to;
  std::string tid;
  {
    std::lock_guard lock(m_);
    const std::uint16_t n = ++next_tid_;
    tid = {static_cast<char>(n >> 8), static_cast<char>(n & 0xff)};
    pending_[tid] = p;
  }
  args.d.emplace_back("id", bencode::value::bytes(as_string(self_)));
  const auto msg = bencode::value::dict({{"t", bencode::value::bytes(tid)},
                                         {"y", bencode::value::bytes("q")},
                                         {"q", bencode::value::bytes(q)},
                                         {"ro", bencode::value::num(1)},
                                         {"a", std::move(args)}});
  const std::string wire = bencode::encode(msg);
  (void)hub_->send({reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size()}, to);
  std::unique_lock lock(m_);
  const auto until = clock::now() + std::chrono::milliseconds(opts_.query_ms);
  while (!p->reply && clock::now() < until && !(stop && stop())) cv_.wait_until(lock, std::min(until, clock::now() + std::chrono::milliseconds(50)));
  pending_.erase(tid);
  if (!p->reply) return std::nullopt;
  bencode::value reply = std::move(*p->reply);
  // BEP 42: how the node saw us, a vote for our public address.
  if (const std::string* ip = reply.str("ip")) {
    if (const auto e = parse_compact_endpoint(*ip)) {
      votes_[to.address + "|" + std::to_string(to.port)] = {*e, clock::now()};
      if (votes_.size() > kMaxVoters) {
        auto oldest = votes_.begin();
        for (auto it = votes_.begin(); it != votes_.end(); ++it) {
          if (it->second.at < oldest->second.at) oldest = it;
        }
        votes_.erase(oldest);
      }
    }
  }
  if (reply.str("y") && *reply.str("y") == "e") return std::nullopt;
  const bencode::value* r = reply.get("r");
  if (!r || r->k != bencode::value::kind::dict) return std::nullopt;
  return *r;
}

void client::learn(const std::vector<node>& nodes) {
  std::lock_guard lock(m_);
  for (const auto& n : nodes) {
    if (std::any_of(table_.begin(), table_.end(),
                    [&](const node& o) { return o.id == n.id || (o.at.address == n.at.address && o.at.port == n.at.port); })) {
      continue;
    }
    if (table_.size() >= kTable) {
      // Keep the secure ones: an insecure entry makes room.
      const auto weak = std::find_if(table_.begin(), table_.end(), [](const node& o) { return !o.secure; });
      if (weak == table_.end() || !n.secure) continue;
      *weak = n;
      continue;
    }
    table_.push_back(n);
  }
}

bool client::start(const std::function<bool()>& stop) {
  std::vector<net::endpoint> seeds = opts_.extra;
  {
    std::lock_guard lock(m_);
    for (const auto& n : table_) {
      if (seeds.size() >= 32) break;
      seeds.push_back(n.at);
    }
  }
  if (opts_.use_routers) {
    for (const auto& [name, port] : bootstrap_routers()) {
      for (const auto& ip : net::resolve(name)) seeds.push_back({net::address_text(ip), port});
    }
  }
  // find_node for ourselves, a few at once, from every seed.
  std::vector<std::thread> asks;
  std::atomic<int> answered{0};
  for (const auto& s : seeds) {
    asks.emplace_back([&, s] {
      auto r = query(s, "find_node", bencode::value::dict({{"target", bencode::value::bytes(as_string(self_))}}), stop);
      if (!r) return;
      ++answered;
      if (const std::string* id = r->str("id"); id && id->size() == 20) {
        node n;
        std::memcpy(n.id.data(), id->data(), 20);
        n.at = s;
        if (const auto ip = net::parse_address(s.address)) n.secure = id_matches(n.id, *ip);
        learn({n});
      }
      if (const std::string* c = r->str("nodes")) learn(parse_nodes(*c, false));
      if (const std::string* c = r->str("nodes6")) learn(parse_nodes(*c, true));
    });
    if (asks.size() >= 16) {
      for (auto& t : asks) t.join();
      asks.clear();
    }
  }
  for (auto& t : asks) t.join();
  return answered.load() > 0;
}

bool client::refresh(const std::function<bool()>& stop) {
  std::vector<net::endpoint> ask;
  std::map<std::string, vote> before;
  {
    std::lock_guard lock(m_);
    before.swap(votes_);
    // The secure nodes first: their votes cannot be steered by where they sit.
    for (int pass = 0; pass < 2; ++pass) {
      for (const auto& n : table_) {
        if (ask.size() >= 8) break;
        if (n.secure == (pass == 0)) ask.push_back(n.at);
      }
    }
  }
  std::vector<std::thread> asks;
  std::atomic<int> answered{0};
  for (const auto& to : ask) {
    asks.emplace_back([&, to] {
      if (query(to, "find_node", bencode::value::dict({{"target", bencode::value::bytes(as_string(self_))}}), stop)) {
        ++answered;
      }
    });
  }
  for (auto& t : asks) t.join();
  if (answered.load() > 0) return true;
  // Nobody answered: what the DHT said before is all there is.
  std::lock_guard lock(m_);
  for (const auto& [k, v] : before) votes_.try_emplace(k, v);  // a vote heard since stays
  return false;
}

std::vector<client::contact> client::lookup(const node_id& target, const std::string& ask,
                                            const std::function<bool()>& stop,
                                            const std::function<void(const contact&, const bencode::value&)>& on_reply) {
  std::vector<contact> c;
  {
    std::lock_guard lock(m_);
    for (const auto& n : table_) c.push_back(contact{n, false, false, {}});
  }
  std::mutex cm;
  auto closer = [&](const contact& a, const contact& b) {
    // Secure nodes first among equals-ish: a node whose id matches its address
    // cannot have picked its place in the keyspace.
    const auto da = distance(a.n.id, target), db = distance(b.n.id, target);
    return da < db;
  };
  const auto until = clock::now() + std::chrono::milliseconds(opts_.lookup_ms);
  for (;;) {
    if ((stop && stop()) || clock::now() >= until) break;
    std::vector<std::size_t> batch;
    {
      std::lock_guard lock(cm);
      std::sort(c.begin(), c.end(), closer);
      if (c.size() > 256) c.resize(256);
      // Done when the k closest that answered have all been asked.
      std::size_t answered = 0;
      bool pending_near = false;
      for (std::size_t i = 0; i < c.size() && answered < kK; ++i) {
        if (c[i].answered) {
          ++answered;
        } else if (!c[i].asked) {
          pending_near = true;
          if (static_cast<int>(batch.size()) < kAlpha) batch.push_back(i);
        }
      }
      if (!pending_near || batch.empty()) break;
      for (std::size_t i : batch) c[i].asked = true;
    }
    std::vector<std::thread> flight;
    for (std::size_t i : batch) {
      flight.emplace_back([&, i] {
        node n;
        {
          std::lock_guard lock(cm);
          n = c[i].n;
        }
        auto r = query(n.at, ask, bencode::value::dict({{"target", bencode::value::bytes(as_string(target))}}), stop);
        if (!r) return;
        std::vector<node> more;
        if (const std::string* x = r->str("nodes")) more = parse_nodes(*x, false);
        if (const std::string* x = r->str("nodes6")) {
          auto m6 = parse_nodes(*x, true);
          more.insert(more.end(), m6.begin(), m6.end());
        }
        learn(more);
        std::lock_guard lock(cm);
        // Find i again by id: the vector may have been sorted meanwhile.
        for (auto& x : c) {
          if (x.n.id == n.id && x.n.at.address == n.at.address) {
            x.answered = true;
            if (const std::string* tok = r->str("token"); tok && tok->size() <= 64) x.token = *tok;
            if (on_reply) on_reply(x, *r);
            break;
          }
        }
        for (const auto& m : more) {
          if (std::none_of(c.begin(), c.end(), [&](const contact& x) { return x.n.id == m.id; })) c.push_back(contact{m, false, false, {}});
        }
      });
    }
    for (auto& t : flight) t.join();
  }
  std::vector<contact> out;
  std::lock_guard lock(cm);
  std::sort(c.begin(), c.end(), closer);
  for (const auto& x : c) {
    if (x.answered) out.push_back(x);
    if (out.size() >= kK) break;
  }
  return out;
}

std::optional<client::item> client::get(std::span<const std::uint8_t, 32> key, std::string_view salt,
                                        const std::function<bool()>& stop) {
  std::vector<std::uint8_t> kt(key.begin(), key.end());
  kt.insert(kt.end(), salt.begin(), salt.end());
  const node_id target = sha1(kt);
  std::optional<item> best;
  (void)lookup(target, "get", stop, [&](const contact&, const bencode::value& r) {
    const std::string* k = r.str("k");
    const std::string* sig = r.str("sig");
    const auto seq = r.integer("seq");
    const bencode::value* v = r.get("v");
    if (!k || !sig || !seq || !v || v->k != bencode::value::kind::bytes) return;
    // The key must be the one the target came from, and the signature hold.
    if (k->size() != 32 || std::memcmp(k->data(), key.data(), 32) != 0 || v->s.size() > kMaxV) return;
    if (!verify(key, salt, *seq, bencode::encode(*v),
                {reinterpret_cast<const std::uint8_t*>(sig->data()), sig->size()})) {
      return;
    }
    if (!best || *seq > best->seq) best = item{v->s, *seq};
  });
  return best;
}

int client::put(const keys& k, std::string_view salt, std::int64_t seq, const std::string& v,
                const std::function<bool()>& stop) {
  if (v.size() > kMaxV) return 0;
  std::vector<std::uint8_t> kt(k.public_key.begin(), k.public_key.end());
  kt.insert(kt.end(), salt.begin(), salt.end());
  const node_id target = sha1(kt);
  const auto closest = lookup(target, "get", stop, nullptr);
  const std::string bv = bencode::encode(bencode::value::bytes(v));
  const auto sig = sign(k, salt, seq, bv);
  std::atomic<int> stored{0};
  std::vector<std::thread> puts;
  for (const auto& c : closest) {
    if (c.token.empty()) continue;
    puts.emplace_back([&, c] {
      auto args = bencode::value::dict({{"token", bencode::value::bytes(c.token)},
                                        {"k", bencode::value::bytes(as_string(k.public_key))},
                                        {"seq", bencode::value::num(seq)},
                                        {"sig", bencode::value::bytes(as_string(sig))},
                                        {"v", bencode::value::bytes(v)}});
      if (!salt.empty()) args.d.emplace_back("salt", bencode::value::bytes(std::string(salt)));
      if (query(c.n.at, "put", std::move(args), stop)) ++stored;
    });
  }
  for (auto& t : puts) t.join();
  return stored.load();
}

std::vector<const client::vote*> client::recent_votes() const {
  std::vector<const vote*> out;
  const auto now = clock::now();
  for (const auto& [voter, v] : votes_) {
    if (now - v.at < std::chrono::milliseconds(kVoteMaxAgeMs)) out.push_back(&v);
  }
  return out;
}

std::optional<net::endpoint> client::external() const {
  std::lock_guard lock(m_);
  std::map<std::string, int> count;
  std::optional<net::endpoint> best;
  int n = 0;
  for (const vote* v : recent_votes()) {
    const int c = ++count[v->seen.address + "|" + std::to_string(v->seen.port)];
    if (c > n) {
      n = c;
      best = v->seen;
    }
  }
  return best;
}

std::vector<net::endpoint> client::external_votes() const {
  std::lock_guard lock(m_);
  std::vector<net::endpoint> out;
  for (const vote* v : recent_votes()) {
    const bool seen = std::any_of(out.begin(), out.end(), [&](const net::endpoint& e) {
      return e.port == v->seen.port && e.address == v->seen.address;
    });
    if (!seen) out.push_back(v->seen);
  }
  return out;
}

int client::vote_count() const {
  std::lock_guard lock(m_);
  return static_cast<int>(recent_votes().size());
}

bool client::looks_hard() const {
  std::lock_guard lock(m_);
  const auto recent = recent_votes();
  std::map<std::string, int> by_address;
  for (const vote* v : recent) ++by_address[v->seen.address];
  std::string address;
  int voters = 0;
  for (const auto& [a, c] : by_address) {
    if (c > voters) {
      voters = c;
      address = a;
    }
  }
  std::map<std::uint16_t, int> by_port;
  for (const vote* v : recent) {
    if (v->seen.address == address) ++by_port[v->seen.port];
  }
  int most = 0;
  for (const auto& [p, c] : by_port) most = std::max(most, c);
  return by_port.size() >= 3 && most * 2 < voters;
}

std::size_t client::known() const {
  std::lock_guard lock(m_);
  return table_.size();
}

void client::load_table() {
  if (opts_.table_path.empty()) return;
  std::ifstream f(opts_.table_path, std::ios::binary);
  std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (data.size() > 64 * 1024) return;
  auto v = bencode::decode(data);
  if (!v) return;
  if (const std::string* n4 = v->str("nodes")) learn(parse_nodes(*n4, false));
  if (const std::string* n6 = v->str("nodes6")) learn(parse_nodes(*n6, true));
}

void client::save_table() {
  if (opts_.table_path.empty()) return;
  std::vector<node> v4, v6;
  {
    std::lock_guard lock(m_);
    for (const auto& n : table_) {
      const auto ip = net::parse_address(n.at.address);
      if (!ip) continue;
      (ip->v6 ? v6 : v4).push_back(n);
      if (v4.size() + v6.size() >= 200) break;
    }
  }
  const std::string text = bencode::encode(bencode::value::dict(
      {{"nodes", bencode::value::bytes(compact_nodes(v4))}, {"nodes6", bencode::value::bytes(compact_nodes(v6))}}));
  const std::string tmp = opts_.table_path + ".new";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) return;
  }
  std::remove(opts_.table_path.c_str());
  (void)std::rename(tmp.c_str(), opts_.table_path.c_str());
}

}  // namespace mv::sharepoint::dht
