// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Sharepoint's opt-in DHT (issue #472, PR 3): bencoding, SHA-1 and BEP 44's
// signatures against the BEP's own test vectors, the four-word codes, and
// the items' sealed contents. No real DHT node is contacted here.
#include "sharepoint_fixture.h"

#include <sodium.h>

#include "addons/sharepoint/bencode.h"
#include "addons/sharepoint/dht_item.h"
#include "addons/sharepoint/sha1.h"

namespace bc = mv::sharepoint::bencode;
namespace dht = mv::sharepoint::dht;

namespace {
std::vector<std::uint8_t> unhex(std::string_view s) {
  std::vector<std::uint8_t> out;
  for (std::size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(static_cast<std::uint8_t>(std::stoi(std::string(s.substr(i, 2)), nullptr, 16)));
  return out;
}
std::string hexs(std::span<const std::uint8_t> b) {
  static constexpr char k[] = "0123456789abcdef";
  std::string s;
  for (auto x : b) {
    s.push_back(k[x >> 4]);
    s.push_back(k[x & 15]);
  }
  return s;
}
std::span<const std::uint8_t> bytes_of(std::string_view s) { return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()}; }
}  // namespace

TEST_CASE("sharepoint dht: bencoding round-trips and refuses what is malformed", "[sharepoint]") {
  const auto v = bc::value::dict({{"t", bc::value::bytes("aa")},
                                  {"y", bc::value::bytes("q")},
                                  {"q", bc::value::bytes("get")},
                                  {"a", bc::value::dict({{"id", bc::value::bytes(std::string(20, 'x'))},
                                                         {"target", bc::value::bytes(std::string(20, 'y'))}})},
                                  {"n", bc::value::num(-42)},
                                  {"l", bc::value::list({bc::value::num(0), bc::value::bytes("")})}});
  const std::string wire = bc::encode(v);
  // Keys come out sorted, as BEP 44's signatures need.
  CHECK(wire.find("1:a") < wire.find("1:l"));
  CHECK(wire.find("1:q") < wire.find("1:t"));
  const auto back = bc::decode(wire);
  REQUIRE(back);
  CHECK(bc::encode(*back) == wire);
  CHECK(back->integer("n") == -42);
  REQUIRE(back->get("a"));
  CHECK(back->get("a")->str("target")->size() == 20);
  for (const char* bad : {"", "i", "ie", "i-0e", "i03e", "i1", "3:ab", "01:a", "l", "d1:ae", "d1:ai1e1:ai2ee", "di1ei2ee",
                          "x", "i123456789012345678901e", "lllllllllli1eeeeeeeeee", "4294967296:x"}) {
    INFO(bad);
    CHECK_FALSE(bc::decode(bad));
  }
  CHECK_FALSE(bc::decode("i1ei2e"));  // a second value after the first
  std::string_view rest;
  REQUIRE(bc::decode("i1ei2e", &rest));
  CHECK(rest == "i2e");
}

TEST_CASE("sharepoint dht: SHA-1 and BEP 44's signatures match the BEP's test vectors", "[sharepoint]") {
  // test 3 (immutable): the target is SHA-1 of the bencoded value.
  CHECK(hexs(mv::sharepoint::sha1(bytes_of("12:Hello World!"))) == "e5f96f6f38320f0f33959cb4d3d656452117aadb");
  CHECK(hexs(mv::sharepoint::sha1(bytes_of("abc"))) == "a9993e364706816aba3e25717850c26c9cd0d89d");
  CHECK(hexs(mv::sharepoint::sha1(bytes_of(""))) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  const std::string million(1000000, 'a');
  CHECK(hexs(mv::sharepoint::sha1(bytes_of(million))) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
  const auto pk = unhex("77ff84905a91936367c01360803104f92432fcd904a43511876df5cdf3e7e548");
  std::array<std::uint8_t, 32> key;
  std::copy(pk.begin(), pk.end(), key.begin());
  // test 1 (mutable): the buffer signed, the target, the signature.
  CHECK(dht::signed_bytes("", 1, "12:Hello World!") == "3:seqi1e1:v12:Hello World!");
  CHECK(hexs(mv::sharepoint::sha1(pk)) == "4a533d47ec9c7d95b1ad75f576cffc641853b750");
  const auto sig1 = unhex("305ac8aeb6c9c151fa120f120ea2cfb923564e11552d06a5d856091e5e853cff1260d3f39e4999684aa92eb73ffd136e6f4f3ecbfda0ce53a1608ecd7ae21f01");
  CHECK(dht::verify(key, "", 1, "12:Hello World!", sig1));
  CHECK_FALSE(dht::verify(key, "", 2, "12:Hello World!", sig1));
  // test 2 (mutable with salt).
  CHECK(dht::signed_bytes("foobar", 1, "12:Hello World!") == "4:salt6:foobar3:seqi1e1:v12:Hello World!");
  std::vector<std::uint8_t> with_salt = pk;
  for (char c : std::string("foobar")) with_salt.push_back(static_cast<std::uint8_t>(c));
  CHECK(hexs(mv::sharepoint::sha1(with_salt)) == "411eba73b6f087ca51a3795d9c8c938d365e32c1");
  const auto sig2 = unhex("6834284b6b24c3204eb2fea824d82f88883a3d95e8b4a21b8c0ded553d17d17ddf9a8a7104b1258f30bed3787e6cb896fca78c58f8e03b5f18f14951a87d9a08");
  CHECK(dht::verify(key, "foobar", 1, "12:Hello World!", sig2));
  CHECK_FALSE(dht::verify(key, "foobaz", 1, "12:Hello World!", sig2));
}

TEST_CASE("sharepoint dht: a four-word code reads back however it was typed", "[sharepoint]") {
  const dht::word_code c = {0, 1, 2047, 1024};
  CHECK(dht::words_text(c) == "abandon ability zoo length");
  CHECK(dht::parse_words("abandon ability zoo length") == c);
  CHECK(dht::parse_words("  Abandon-ABILITY.zoo, length ") == c);
  CHECK(dht::parse_words("aban abil zoo leng") == c);  // the first four letters are enough
  for (const char* bad : {"", "abandon ability zoo", "abandon ability zoo length extra", "abandon ability zoo xyzzy",
                          "aba abil zoo leng", "abandon ability zoo length1"}) {
    INFO(bad);
    CHECK_FALSE(dht::parse_words(bad));
  }
  for (int i = 0; i < 50; ++i) {
    const auto r = dht::random_code();
    CHECK(dht::parse_words(dht::words_text(r)) == r);
  }
}

TEST_CASE("sharepoint dht: an item is sealed to the code, padded, and refused when old or altered", "[sharepoint]") {
  const dht::word_code code = {12, 345, 678, 1999};
  auto k = dht::derive(code, true);
  REQUIRE(k);
  auto again = dht::derive(dht::word_code{12, 345, 678, 1999}, true);
  REQUIRE(again);
  CHECK(again->public_key == k->public_key);
  const std::string salt = dht::salt_of(*k, dht::role::receiver, 0);
  CHECK(salt.size() == 16);
  CHECK(dht::salt_of(*k, dht::role::sender, 0) != salt);
  CHECK(dht::salt_of(*k, dht::role::receiver, 1) != salt);
  dht::contents c;
  c.candidates = {{"81.2.69.160", 41000}, {"2a00:1450:4009:81f::200e", 41001}, {"192.168.1.20", 41000}};
  c.written_ms = 1'760'000'000'000;
  const std::string v = dht::seal(*k, salt, c);
  CHECK(v.size() == 24 + dht::kPlainBytes + 16);  // fixed size: says nothing of how many addresses
  auto back = dht::open(*k, salt, v, c.written_ms + 1000);
  REQUIRE(back);
  REQUIRE(back->candidates.size() == 3);
  CHECK(back->candidates[1].address == "2a00:1450:4009:81f::200e");
  CHECK_FALSE(back->gone);
  // Another code, another salt, one flipped byte, too old or from the future: refused.
  auto other = dht::derive(dht::word_code{12, 345, 678, 1998}, true);
  REQUIRE(other);
  CHECK_FALSE(dht::open(*other, salt, v, c.written_ms));
  CHECK_FALSE(dht::open(*k, dht::salt_of(*k, dht::role::receiver, 1), v, c.written_ms));
  std::string flipped = v;
  flipped[40] ^= 1;
  CHECK_FALSE(dht::open(*k, salt, flipped, c.written_ms));
  CHECK_FALSE(dht::open(*k, salt, v, c.written_ms + dht::kItemMaxAgeMs + 1));
  CHECK_FALSE(dht::open(*k, salt, v, c.written_ms - dht::kItemSkewMs - 1));
  // Signed under the code's key; a put's signature verifies with its public key.
  const std::string bv = bc::encode(bc::value::bytes(v));
  const auto sig = dht::sign(*k, salt, 7, bv);
  CHECK(dht::verify(k->public_key, salt, 7, bv, sig));
  CHECK(dht::target_of(*k, salt) != dht::target_of(*k, dht::salt_of(*k, dht::role::sender, 0)));
}

// ---- one UDP socket: the DHT, the probes, the reliable stream ------------------------------------

#include "addons/sharepoint/udp_hub.h"

namespace {
struct hub_pair {
  std::shared_ptr<sp::udp_hub> a, b;
  hub_pair() {
    auto x = sp::udp_hub::open(0, true);
    auto y = sp::udp_hub::open(0, true);
    REQUIRE(x);
    REQUIRE(y);
    a = std::move(*x);
    b = std::move(*y);
  }
  sp::net::endpoint at_a(const char* ip = "127.0.0.1") const { return {ip, a->port()}; }
  sp::net::endpoint at_b(const char* ip = "127.0.0.1") const { return {ip, b->port()}; }
};

// `bytes` through a stream pair; true when every byte arrived in order.
bool stream_through(hub_pair& h, std::size_t bytes, double* mb_s = nullptr, const char* ip = "127.0.0.1") {
  auto sa = h.a->stream(h.at_b(ip), 77);
  auto sb = h.b->stream(h.at_a(ip), 77);
  const auto data = pattern(bytes, 11);
  const auto t0 = std::chrono::steady_clock::now();
  std::thread w([&] { REQUIRE(sa->write_all(data)); });
  std::vector<std::uint8_t> got;
  got.reserve(bytes);
  std::vector<std::uint8_t> buf(64 * 1024);
  while (got.size() < bytes) {
    auto n = sb->read_some(buf, 20000);
    if (!n || *n == 0) break;
    got.insert(got.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
  }
  w.join();
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (mb_s) *mb_s = static_cast<double>(bytes) / 1e6 / s;
  return got == data;
}
}  // namespace

TEST_CASE("sharepoint dht: a stream over the UDP hub delivers every byte in order, lossy or not", "[sharepoint]") {
  hub_pair h;
  double clean = 0, lossy = 0;
  CHECK(stream_through(h, 16u * 1024 * 1024, &clean));
  hub_pair l;
  l.a->test_loss(50);  // 5 % of the sender's packets lost
  l.b->test_loss(50);  // and of the acks
  CHECK(stream_through(l, 4u * 1024 * 1024, &lossy));
  hub_pair one;
  double lossy1 = 0;
  one.a->test_loss(10);
  one.b->test_loss(10);
  CHECK(stream_through(one, 8u * 1024 * 1024, &lossy1));
  hub_pair v6;
  CHECK(stream_through(v6, 1024 * 1024, nullptr, "::1"));  // the same socket, IPv6
  std::printf("{\"case\":\"udp_stream\",\"clean_mb_s\":%.1f,\"lossy_1pct_mb_s\":%.1f,\"lossy_5pct_mb_s\":%.1f}\n", clean,
              lossy1, lossy);
}

TEST_CASE("sharepoint dht: protocol 2 opens and carries a session over the UDP stream", "[sharepoint]") {
  hub_pair h;
  std::shared_ptr<sp::link> la(h.a->stream(h.at_b(), 5)), lb(h.b->stream(h.at_a(), 5));
  CHECK(la->via() == sp::transport::internet);
  mv::result<std::unique_ptr<sp::channel>> server = mv::err(mv::status::internal);
  std::thread t([&] { server = sp::channel::open(lb, false, "bbbbbbbbbbbbbbbb", "Bea"); });
  auto client = sp::channel::open(la, true, "aaaaaaaaaaaaaaaa", "Ann");
  t.join();
  REQUIRE(client);
  REQUIRE(server);
  CHECK((*client)->code() == (*server)->code());
  const auto chunk = pattern(sp::kChunkBytes, 3);
  for (int i = 0; i < 8; ++i) REQUIRE((*client)->send(sp::msg::data, chunk));
  sp::message m;
  for (int i = 0; i < 8; ++i) {
    REQUIRE((*server)->receive(m, 5000));
    CHECK(m.body == chunk);
  }
}

TEST_CASE("sharepoint dht: the hub hands KRPC and probes to their handlers, and drops strangers' stream packets",
          "[sharepoint]") {
  hub_pair h;
  std::atomic<int> dht{0}, probes{0};
  h.b->on_dht([&](std::span<const std::uint8_t>, const sp::net::endpoint&) { ++dht; });
  h.b->on_probe([&](std::span<const std::uint8_t>, const sp::net::endpoint&) { ++probes; });
  const std::string krpc = "d1:t2:aa1:y1:qe";
  REQUIRE(h.a->send({reinterpret_cast<const std::uint8_t*>(krpc.data()), krpc.size()}, h.at_b()));
  const std::uint8_t probe[] = {sp::kProbe, 1, 2, 3};
  REQUIRE(h.a->send(probe, h.at_b()));
  REQUIRE(eventually([&] { return dht.load() == 1 && probes.load() == 1; }, 5));
  // A stream that expects another peer gets nothing from this one.
  auto sb = h.b->stream({"127.0.0.1", 9}, 99);
  auto sa = h.a->stream(h.at_b(), 99);
  const std::uint8_t hi[] = {1, 2, 3};
  REQUIRE(sa->write_all(hi));
  std::uint8_t buf[8];
  auto n = sb->read_some(buf, 300);
  REQUIRE_FALSE(n);
  CHECK(n.error() == mv::status::timeout);
}

// ---- the DHT client, against a swarm of stand-in nodes on loopback -----------------------------------

#include "addons/sharepoint/dht.h"

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
      const auto t = mv::sharepoint::sha1({reinterpret_cast<const std::uint8_t*>(kt.data()), kt.size()});
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

dht::client::options test_options(const swarm& s, std::string table = {}) {
  dht::client::options o;
  o.use_routers = false;
  o.extra = {s.seed()};
  o.table_path = std::move(table);
  o.query_ms = 500;
  o.lookup_ms = 5000;
  return o;
}

}  // namespace

TEST_CASE("sharepoint dht: BEP 42's node ids check against the BEP's test vectors", "[sharepoint]") {
  struct row {
    const char* ip;
    std::uint8_t rand;
    const char* prefix;
  };
  for (const row r : {row{"124.31.75.21", 1, "5fbfbf"}, row{"21.75.31.124", 86, "5a3ce9"}, row{"65.23.51.170", 22, "a5d432"},
                      row{"84.124.73.14", 65, "1b0321"}, row{"43.213.53.83", 90, "e56f6c"}}) {
    INFO(r.ip);
    dht::node_id id{};
    const auto p = unhex(r.prefix);
    std::copy(p.begin(), p.end(), id.begin());
    id[19] = r.rand;
    const auto ip = sp::net::parse_address(r.ip);
    REQUIRE(ip);
    CHECK(dht::id_matches(id, *ip));
    id[0] ^= 1;
    CHECK_FALSE(dht::id_matches(id, *ip));
  }
  CHECK(dht::crc32c(bytes_of("123456789")) == 0xE3069283u);  // the CRC-32C check value
}

TEST_CASE("sharepoint dht: an item put by one client is found by another, and a forger is ignored", "[sharepoint]") {
  swarm s(16);
  s.nodes[3]->forge = true;  // one node lies about every item
  auto ha = sp::udp_hub::open(0, true);
  auto hb = sp::udp_hub::open(0, true);
  REQUIRE(ha);
  REQUIRE(hb);
  dht::client a(*ha, test_options(s)), b(*hb, test_options(s));
  REQUIRE(a.start({}));
  REQUIRE(b.start({}));
  CHECK(a.known() >= 8);
  // BEP 42: the swarm says where it saw us.
  const auto ext = a.external();
  REQUIRE(ext);
  CHECK(ext->port == (*ha)->port());
  auto k = dht::derive({1, 2, 3, 4}, true);
  REQUIRE(k);
  const std::string salt = dht::salt_of(*k, dht::role::receiver, 0);
  dht::contents c;
  c.candidates = {{"81.2.69.160", 41000}};
  c.written_ms = 1'760'000'000'000;
  const std::string v = dht::seal(*k, salt, c);
  CHECK(a.put(*k, salt, 5, v, {}) >= 4);
  auto got = b.get(k->public_key, salt, {});
  REQUIRE(got);
  CHECK(got->seq == 5);
  CHECK(got->v == v);
  auto open = dht::open(*k, salt, got->v, c.written_ms);
  REQUIRE(open);
  CHECK(open->candidates[0].port == 41000);
  // A newer one replaces it.
  c.gone = true;
  c.candidates.clear();
  CHECK(a.put(*k, salt, 6, dht::seal(*k, salt, c), {}) >= 4);
  got = b.get(k->public_key, salt, {});
  REQUIRE(got);
  CHECK(got->seq == 6);
  CHECK(dht::open(*k, salt, got->v, c.written_ms)->gone);
  // Nothing under a salt nobody used.
  CHECK_FALSE(b.get(k->public_key, dht::salt_of(*k, dht::role::sender, 3), {}));
}

TEST_CASE("sharepoint dht: the routing table is kept between runs", "[sharepoint]") {
  scratch_dir dir("sharepoint_dht_table");
  swarm s(12);
  const std::string table = utf8(dir.root() / "dht_nodes.dat");
  {
    auto h = sp::udp_hub::open(0, true);
    REQUIRE(h);
    dht::client c(*h, test_options(s, table));
    REQUIRE(c.start({}));
  }
  // The next run starts from the saved table alone (no seed, no router).
  auto h = sp::udp_hub::open(0, true);
  REQUIRE(h);
  auto o = test_options(s, table);
  o.extra.clear();
  dht::client c(*h, o);
  CHECK(c.known() >= 8);
  CHECK(c.start({}));
}

// ---- meeting on a word code ----------------------------------------------------------------------

#include "addons/sharepoint/meet.h"

TEST_CASE("sharepoint dht: two sides meet on four words, punch, and open the internet handshake over UDP",
          "[sharepoint]") {
  swarm s(16);
  auto hr = sp::udp_hub::open(0, true);
  auto hs = sp::udp_hub::open(0, true);
  REQUIRE(hr);
  REQUIRE(hs);
  dht::client cr(*hr, test_options(s)), cs(*hs, test_options(s));
  REQUIRE(cr.start({}));
  REQUIRE(cs.start({}));
  const dht::word_code code = dht::random_code();
  auto kr = dht::derive(code, true);
  auto ks = dht::derive(*dht::parse_words(dht::words_text(code)), true);  // typed on the other side
  REQUIRE(kr);
  REQUIRE(ks);
  dht::meet_options o;
  o.loopback = true;
  o.wait_ms = 20000;
  mv::result<std::unique_ptr<sp::link>> at_r = mv::err(mv::status::internal);
  std::thread r([&] { at_r = dht::meet(*hr, cr, *kr, dht::role::receiver, o, {}, {}); });
  std::this_thread::sleep_for(std::chrono::milliseconds(300));  // the sender types the code a little later
  auto at_s = dht::meet(*hs, cs, *ks, dht::role::sender, o, {}, {});
  r.join();
  REQUIRE(at_r);
  REQUIRE(at_s);
  std::shared_ptr<sp::link> lr(std::move(*at_r)), ls(std::move(*at_s));
  // The punched stream carries bytes both ways before any handshake: the
  // sender first (the receiver learns its peer from that packet).
  const auto read_all = [](sp::link& l, std::size_t n) {
    std::vector<std::uint8_t> got;
    std::vector<std::uint8_t> buf(n);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (got.size() < n && std::chrono::steady_clock::now() < until) {
      auto r = l.read_some(std::span(buf.data(), n - got.size()), 500);
      if (r) got.insert(got.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*r));
      else if (r.error() != mv::status::timeout) break;
    }
    return got;
  };
  const auto there = pattern(3000, 4), back = pattern(2000, 5);
  REQUIRE(ls->write_all(there));
  CHECK(read_all(*lr, there.size()) == there);
  REQUIRE(lr->write_all(back));
  CHECK(read_all(*ls, back.size()) == back);
  // The internet handshake over the punched path, CPace on the code's password.
  const std::vector<std::uint8_t> pw(kr->password.begin(), kr->password.end());
  mv::result<std::unique_ptr<sp::channel>> server = mv::err(mv::status::internal);
  std::thread t([&] {
    auto h = sp::channel::read_hello(*lr, std::chrono::steady_clock::now() + std::chrono::seconds(5));
    if (!h) {
      server = mv::err(h.error());
      return;
    }
    sp::channel::options so;
    so.internet_secret = pw;
    server = sp::channel::open_server(lr, std::move(*h), "bbbbbbbbbbbbbbbb", "Bea", so);
  });
  const auto opened_at = std::chrono::steady_clock::now();
  auto client = sp::channel::open_internet(ls, pw, "aaaaaaaaaaaaaaaa", "Ann");
  t.join();
  // Milliseconds on loopback; seconds is a socket wait holding up the sends
  // (WSAPoll on Windows, #472).
  std::printf("{\"case\":\"dht_meet_handshake\",\"open_internet_ms\":%.1f}\n",
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - opened_at).count());
  INFO("client " << (client ? 0 : static_cast<int>(client.error())) << ", server "
                 << (server ? 0 : static_cast<int>(server.error())));
  REQUIRE(client);
  REQUIRE(server);
  CHECK((*client)->code() == (*server)->code());
  CHECK((*client)->via() == sp::transport::internet);
  REQUIRE((*client)->send(sp::msg::data, pattern(sp::kChunkBytes, 1)));
  sp::message m;
  REQUIRE((*server)->receive(m, 5000));
  CHECK(m.body == pattern(sp::kChunkBytes, 1));
  // Then each side's items say it is gone.
  dht::withdraw(cs, *ks, dht::role::sender, {});
  auto left = cr.get(ks->public_key, dht::salt_of(*ks, dht::role::sender, 0), {});
  REQUIRE(left);
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count();
  CHECK(dht::open(*ks, dht::salt_of(*ks, dht::role::sender, 0), left->v, now)->gone);
}

TEST_CASE("sharepoint dht: a wrong word code finds nobody", "[sharepoint]") {
  swarm s(12);
  auto hr = sp::udp_hub::open(0, true);
  auto hs = sp::udp_hub::open(0, true);
  REQUIRE(hr);
  REQUIRE(hs);
  dht::client cr(*hr, test_options(s)), cs(*hs, test_options(s));
  REQUIRE(cr.start({}));
  REQUIRE(cs.start({}));
  auto kr = dht::derive({10, 20, 30, 40}, true);
  auto ks = dht::derive({10, 20, 30, 41}, true);
  REQUIRE(kr);
  REQUIRE(ks);
  dht::meet_options o;
  o.loopback = true;
  o.wait_ms = 2500;
  std::atomic<bool> done{false};
  std::thread r([&] { (void)dht::meet(*hr, cr, *kr, dht::role::receiver, o, [&] { return done.load(); }, {}); });
  const auto at_s = dht::meet(*hs, cs, *ks, dht::role::sender, o, {}, {});
  done.store(true);
  r.join();
  REQUIRE_FALSE(at_s);
  CHECK(at_s.error() == mv::status::timeout);
}

// ---- two engines over four words ----------------------------------------------------------------

namespace {
struct word_client {
  fake_host host;
  sp::engine eng;
  word_client(const fs::path& received, const std::string& name, const swarm& s)
      : host(received), eng(host, options(name, s)) {
    eng.screen_opened("");
    eng.on_event("internet", 2, "true");
    eng.on_event("dht", 2, "true");
  }
  static sp::engine::options options(const std::string& name, const swarm& s) {
    sp::engine::options o;
    o.my_name = name;
    o.use_bluetooth = false;
    o.loopback_only = true;
    o.make_discovery = [] { return std::make_unique<inject_discovery>(); };
    o.internet_loopback = true;
    o.internet_mapping = std::nullopt;
    o.dht_nodes = std::vector<sp::net::endpoint>{s.seed()};
    o.dht_fast_kdf = true;
    o.internet_force_words = true;
    return o;
  }
};
}  // namespace

TEST_CASE("sharepoint dht: four words send files between two engines, through the DHT and a punched path",
          "[sharepoint]") {
  scratch_dir dir("sharepoint_words_send");
  swarm s(16);
  word_client bea(dir / "bea_received", "Bea", s);
  word_client ann(dir / "ann_received", "Ann", s);
  const fs::path f = dir / "out" / "clip.mov";
  write_bytes(f, pattern(2 * 1024 * 1024 + 5, 21));
  ann.eng.set_files({utf8(f)});
  REQUIRE(eventually([&] { return ann.eng.snapshot().file_count == 1; }));
  bea.eng.on_event("make_code", 1, "");
  std::string words;
  REQUIRE(eventually([&] {
    const auto v = bea.eng.snapshot();
    words = v.code_text;
    return v.code_phase == static_cast<std::uint8_t>(sp::internet_receiver::phase::ready) && !words.empty();
  }, 20));
  CHECK(std::count(words.begin(), words.end(), ' ') == 3);  // four words
  CHECK(bea.eng.snapshot().code_link.empty());
  ann.eng.on_event("send_code", 2, "\"" + words + "\"");
  ann.eng.on_event("send_to_code", 1, "");
  REQUIRE(eventually([&] { return ann.eng.snapshot().out == sp::view::out_phase::naming; }, 30));
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::connecting; }));
  CHECK(ann.eng.snapshot().out_check == bea.eng.snapshot().in_code);
  CHECK(ann.eng.snapshot().out_via == "the internet");
  ann.eng.on_event("out_confirm", 1, "");
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  bea.eng.on_event("accept", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  INFO(ann.eng.snapshot().out_note);
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::done);
  CHECK(read_bytes(dir / "bea_received" / "clip.mov") == read_bytes(f));
}

// ---- harness: the real DHT (#472 spike), never run by CI -------------------------------------------
//
// mv_import_tests "[.sharepoint-dht-live]": joins the public BitTorrent DHT
// from this machine (its routers, then a saved table under MV_DHT_TABLE if
// set), and times MV_DHT_LOOKUPS gets of random targets (default 10). Read
// only: nothing is put. DHT nodes see this machine's address taking part.
// Prints one JSON line: start time, nodes known, lookup p50 / p95, and the
// public address votes (how many distinct ports: a hard NAT shows several).
TEST_CASE("sharepoint dht: lookups on the public DHT (harness)", "[.sharepoint-dht-live]") {
  auto h = sp::udp_hub::open(0, false);
  REQUIRE(h);
  dht::client::options o;
  if (const char* t = std::getenv("MV_DHT_TABLE"); t && *t) o.table_path = t;
  dht::client c(*h, o);
  const auto t0 = std::chrono::steady_clock::now();
  REQUIRE(c.start({}));
  const double start_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const int n = std::getenv("MV_DHT_LOOKUPS") ? std::atoi(std::getenv("MV_DHT_LOOKUPS")) : 10;
  std::vector<double> ms;
  for (int i = 0; i < n; ++i) {
    std::array<std::uint8_t, 32> key;
    randombytes_buf(key.data(), key.size());
    const auto a = std::chrono::steady_clock::now();
    (void)c.get(key, "", {});
    ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
  }
  std::sort(ms.begin(), ms.end());
  const auto votes = c.external_votes();
  std::printf("{\"case\":\"dht_live\",\"start_s\":%.2f,\"known\":%zu,\"lookups\":%d,\"p50_ms\":%.0f,\"p95_ms\":%.0f,"
              "\"public_ports_seen\":%zu}\n",
              start_s, c.known(), n, ms[ms.size() / 2], ms[std::min(ms.size() - 1, ms.size() * 95 / 100)],
              votes.size());
}
