// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Driftwire's opt-in DHT (issue #472, PR 3): bencoding, SHA-1 and BEP 44's
// signatures against the BEP's own test vectors, the four-word codes, and
// the items' sealed contents. No real DHT node is contacted here.
#include "driftwire_fixture.h"

#include <sodium.h>

#include "addons/driftwire/bencode.h"
#include "addons/driftwire/dht_item.h"
#include "addons/driftwire/known.h"
#include "addons/driftwire/secrets.h"
#include "addons/driftwire/sha1.h"

namespace bc = mv::driftwire::bencode;
namespace dht = mv::driftwire::dht;

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

TEST_CASE("driftwire dht: bencoding round-trips and refuses what is malformed", "[driftwire]") {
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

TEST_CASE("driftwire dht: SHA-1 and BEP 44's signatures match the BEP's test vectors", "[driftwire]") {
  // test 3 (immutable): the target is SHA-1 of the bencoded value.
  CHECK(hexs(mv::driftwire::sha1(bytes_of("12:Hello World!"))) == "e5f96f6f38320f0f33959cb4d3d656452117aadb");
  CHECK(hexs(mv::driftwire::sha1(bytes_of("abc"))) == "a9993e364706816aba3e25717850c26c9cd0d89d");
  CHECK(hexs(mv::driftwire::sha1(bytes_of(""))) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  const std::string million(1000000, 'a');
  CHECK(hexs(mv::driftwire::sha1(bytes_of(million))) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
  const auto pk = unhex("77ff84905a91936367c01360803104f92432fcd904a43511876df5cdf3e7e548");
  std::array<std::uint8_t, 32> key;
  std::copy(pk.begin(), pk.end(), key.begin());
  // test 1 (mutable): the buffer signed, the target, the signature.
  CHECK(dht::signed_bytes("", 1, "12:Hello World!") == "3:seqi1e1:v12:Hello World!");
  CHECK(hexs(mv::driftwire::sha1(pk)) == "4a533d47ec9c7d95b1ad75f576cffc641853b750");
  const auto sig1 = unhex("305ac8aeb6c9c151fa120f120ea2cfb923564e11552d06a5d856091e5e853cff1260d3f39e4999684aa92eb73ffd136e6f4f3ecbfda0ce53a1608ecd7ae21f01");
  CHECK(dht::verify(key, "", 1, "12:Hello World!", sig1));
  CHECK_FALSE(dht::verify(key, "", 2, "12:Hello World!", sig1));
  // test 2 (mutable with salt).
  CHECK(dht::signed_bytes("foobar", 1, "12:Hello World!") == "4:salt6:foobar3:seqi1e1:v12:Hello World!");
  std::vector<std::uint8_t> with_salt = pk;
  for (char c : std::string("foobar")) with_salt.push_back(static_cast<std::uint8_t>(c));
  CHECK(hexs(mv::driftwire::sha1(with_salt)) == "411eba73b6f087ca51a3795d9c8c938d365e32c1");
  const auto sig2 = unhex("6834284b6b24c3204eb2fea824d82f88883a3d95e8b4a21b8c0ded553d17d17ddf9a8a7104b1258f30bed3787e6cb896fca78c58f8e03b5f18f14951a87d9a08");
  CHECK(dht::verify(key, "foobar", 1, "12:Hello World!", sig2));
  CHECK_FALSE(dht::verify(key, "foobaz", 1, "12:Hello World!", sig2));
}

TEST_CASE("driftwire dht: a four-word code reads back however it was typed", "[driftwire]") {
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

TEST_CASE("driftwire dht: an item is sealed to the code, padded, and refused when old or altered", "[driftwire]") {
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
  CHECK_FALSE(back->hard);
  CHECK(back->tcp_port == 0);
  // #472: the hard flag and the TCP port ride after the candidates.
  c.hard = true;
  c.tcp_port = 50123;
  auto flagged = dht::open(*k, salt, dht::seal(*k, salt, c), c.written_ms);
  REQUIRE(flagged);
  CHECK(flagged->hard);
  CHECK(flagged->tcp_port == 50123);
  CHECK(flagged->candidates.size() == 3);
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

#include "addons/driftwire/udp_hub.h"

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

TEST_CASE("driftwire dht: a stream over the UDP hub delivers every byte in order, lossy or not", "[driftwire]") {
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

TEST_CASE("driftwire dht: protocol 2 opens and carries a session over the UDP stream", "[driftwire]") {
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

TEST_CASE("driftwire dht: the hub hands KRPC and probes to their handlers, and drops strangers' stream packets",
          "[driftwire]") {
  hub_pair h;
  std::atomic<int> dht{0}, probes{0};
  h.b->on_dht([&](std::span<const std::uint8_t>, const sp::net::endpoint&) { ++dht; });
  h.b->on_probe([&](std::span<const std::uint8_t>, const sp::net::endpoint&, std::uint32_t) { ++probes; });
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

#include "driftwire_swarm.h"

TEST_CASE("driftwire dht: BEP 42's node ids check against the BEP's test vectors", "[driftwire]") {
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

TEST_CASE("driftwire dht: an item put by one client is found by another, and a forger is ignored", "[driftwire]") {
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

TEST_CASE("driftwire dht: the routing table is kept between runs", "[driftwire]") {
  scratch_dir dir("driftwire_dht_table");
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

#include "addons/driftwire/meet.h"

TEST_CASE("driftwire dht: two sides meet on four words, punch, and open the internet handshake over UDP",
          "[driftwire]") {
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

TEST_CASE("driftwire dht: a wrong word code finds nobody", "[driftwire]") {
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


// ---- stand-in NATs: the birthday punch and TCP simultaneous open (#472) ------------------------

#include <set>

namespace {

// A NAT on loopback, for the tests only: every socket a hub opens is wrapped
// (udp_hub::socket_wrap). A send leaves from one of the NAT's outside sockets,
// on a port in [lo, lo + 1023]; what comes back to that port from a peer the
// mapping allows is handed in, with the peer as its sender. Anything reaching
// the inside socket directly is dropped: behind a NAT nobody can. IPv4 only
// (an IPv6 send is dropped).
//   hard: a new outside port for each destination, answered only by it
//         (endpoint-dependent mapping and filtering: a "symmetric" NAT).
//   else: one outside port per inside socket, answered only by peers it has
//         sent to (a port-restricted cone: the easy side).
struct stand_in_nat {
  struct mapping {
    std::shared_ptr<sp::net::udp_socket> out;
    std::uint16_t inside = 0;
    std::set<std::string> peers;  // "address|port" that may answer
  };
  bool hard;
  std::uint16_t lo;
  std::mutex m;
  std::map<std::string, std::shared_ptr<mapping>> by_key;
  std::map<std::uint16_t, std::shared_ptr<mapping>> by_out;
  std::atomic<bool> stop{false};
  std::thread pump;

  explicit stand_in_nat(bool h) : hard(h), lo(static_cast<std::uint16_t>(20000 + randombytes_uniform(30000))) {
    pump = std::thread([this] { run(); });
  }
  ~stand_in_nat() {
    stop.store(true);
    pump.join();
  }
  static std::string key(const sp::net::endpoint& e) { return e.address + "|" + std::to_string(e.port); }
  std::uint16_t hi() const { return static_cast<std::uint16_t>(lo + 1023); }
  std::size_t outside_ports() {
    std::lock_guard lock(m);
    return by_out.size();
  }

  // Outbound through the NAT.
  mv::expected send(std::uint16_t inside, std::span<const std::uint8_t> d, const sp::net::endpoint& to) {
    const auto ip = sp::net::parse_address(to.address);
    if (!ip || ip->v6) return {};
    std::shared_ptr<mapping> mp;
    {
      std::lock_guard lock(m);
      const std::string k = hard ? std::to_string(inside) + "|" + key(to) : std::to_string(inside);
      auto& slot = by_key[k];
      if (!slot) {
        for (int tries = 0; tries < 4000 && !slot; ++tries) {
          const auto port = static_cast<std::uint16_t>(lo + randombytes_uniform(1024));
          if (by_out.count(port)) continue;
          auto o = sp::net::open_udp_dual(port, true);
          if (!o) continue;
          slot = std::make_shared<mapping>();
          slot->out = std::move(*o);
          slot->inside = inside;
          by_out[port] = slot;
        }
        if (!slot) return mv::err(mv::status::io);
      }
      slot->peers.insert(key(to));
      mp = slot;
    }
    return mp->out->send_to(d, to);
  }

  // Inbound: allowed answers go to the inside socket, the peer in front.
  void run() {
    std::vector<std::uint8_t> buf(65536);
    while (!stop.load()) {
      std::vector<std::shared_ptr<mapping>> all;
      {
        std::lock_guard lock(m);
        for (auto& [p, mp] : by_out) all.push_back(mp);
      }
      if (all.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
      std::vector<sp::net::udp_socket*> raw;
      for (auto& mp : all) raw.push_back(mp->out.get());
      std::unique_ptr<bool[]> ready(new bool[raw.size()]());
      if (!sp::net::wait_readable(raw, std::span<bool>(ready.get(), raw.size()), 20)) continue;
      for (std::size_t i = 0; i < raw.size(); ++i) {
        if (!ready[i]) continue;
        sp::net::endpoint from;
        auto n = raw[i]->receive_from({buf.data() + 6, buf.size() - 6}, 0, from);
        if (!n) continue;
        {
          std::lock_guard lock(m);
          if (!all[i]->peers.count(key(from))) continue;  // nobody inside asked
        }
        const auto ip = sp::net::parse_address(from.address);
        if (!ip || ip->v6) continue;
        std::memcpy(buf.data(), ip->bytes.data(), 4);
        buf[4] = static_cast<std::uint8_t>(from.port >> 8);
        buf[5] = static_cast<std::uint8_t>(from.port);
        (void)raw[i]->send_to({buf.data(), *n + 6}, {"127.0.0.1", all[i]->inside});
      }
    }
  }

  bool is_outside(const sp::net::endpoint& e) {
    std::lock_guard lock(m);
    return e.address == "127.0.0.1" && by_out.count(e.port);
  }

  // A socket behind this NAT.
  class inside_socket final : public sp::net::udp_socket {
   public:
    inside_socket(stand_in_nat& nat, std::unique_ptr<sp::net::udp_socket> in) : nat_(nat), in_(std::move(in)) {}
    mv::expected send_to(std::span<const std::uint8_t> d, const sp::net::endpoint& to) override {
      return nat_.send(in_->port(), d, to);
    }
    mv::result<std::size_t> receive_from(std::span<std::uint8_t> into, int timeout_ms,
                                         sp::net::endpoint& from) override {
      std::uint8_t buf[65536];
      sp::net::endpoint via;
      auto n = in_->receive_from(buf, timeout_ms, via);
      if (!n) return n;
      if (*n < 6 || !nat_.is_outside(via)) return mv::err(mv::status::timeout);  // not through the NAT: dropped
      sp::net::address ip;
      std::memcpy(ip.bytes.data(), buf, 4);
      from = {sp::net::address_text(ip), static_cast<std::uint16_t>(buf[4] << 8 | buf[5])};
      const std::size_t len = std::min(*n - 6, into.size());
      std::memcpy(into.data(), buf + 6, len);
      return len;
    }
    void shutdown() noexcept override { in_->shutdown(); }
    std::uint16_t port() const noexcept override { return in_->port(); }
    std::intptr_t handle() const noexcept override { return in_->handle(); }
    void set_buffers(int bytes) noexcept override { in_->set_buffers(bytes); }

   private:
    stand_in_nat& nat_;
    std::unique_ptr<sp::net::udp_socket> in_;
  };

  sp::udp_hub::socket_wrap wrap() {
    return [this](std::unique_ptr<sp::net::udp_socket> s) -> std::unique_ptr<sp::net::udp_socket> {
      return std::make_unique<inside_socket>(*this, std::move(s));
    };
  }
};

std::shared_ptr<sp::udp_hub> hub_behind(stand_in_nat* nat) {
  auto h = sp::udp_hub::open(0, true, nat ? nat->wrap() : sp::udp_hub::socket_wrap{});
  REQUIRE(h);
  return std::move(*h);
}

// The internet handshake and a chunk over a met link, CPace on the code's password.
void handshake_over(std::unique_ptr<sp::link> r, std::unique_ptr<sp::link> s, const dht::keys& k) {
  const std::vector<std::uint8_t> pw(k.password.begin(), k.password.end());
  std::shared_ptr<sp::link> lr(std::move(r)), ls(std::move(s));
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
  auto client = sp::channel::open_internet(ls, pw, "aaaaaaaaaaaaaaaa", "Ann");
  t.join();
  REQUIRE(client);
  REQUIRE(server);
  CHECK((*client)->code() == (*server)->code());
  CHECK((*client)->via() == sp::transport::internet);
  REQUIRE((*client)->send(sp::msg::data, pattern(sp::kChunkBytes, 3)));
  sp::message msg;
  REQUIRE((*server)->receive(msg, 5000));
  CHECK(msg.body == pattern(sp::kChunkBytes, 3));
}

struct met_pair {
  mv::result<std::unique_ptr<sp::link>> r = mv::err(mv::status::internal), s = mv::err(mv::status::internal);
  std::optional<dht::keys> k;
};

// Both sides meet on one code: the receiver's hub `hr`, the sender's `hs`.
met_pair meet_both(swarm& sw, const std::shared_ptr<sp::udp_hub>& hr, const std::shared_ptr<sp::udp_hub>& hs,
                   dht::meet_options o, bool* r_hard = nullptr, bool* s_hard = nullptr) {
  dht::client cr(hr, test_options(sw)), cs(hs, test_options(sw));
  REQUIRE(cr.start({}));
  REQUIRE(cs.start({}));
  const dht::word_code code = dht::random_code();
  auto kr = dht::derive(code, true);
  auto ks = dht::derive(code, true);
  REQUIRE(kr);
  REQUIRE(ks);
  o.loopback = true;
  o.wait_ms = 20000;
  met_pair out;
  std::thread r([&] { out.r = dht::meet(hr, cr, *kr, dht::role::receiver, o, {}, {}); });
  out.s = dht::meet(hs, cs, *ks, dht::role::sender, o, {}, {});
  r.join();
  // How the DHT saw each side (the meeting's own puts asked many nodes).
  if (r_hard) *r_hard = dht::seen_hard(cr);
  if (s_hard) *s_hard = dht::seen_hard(cs);
  out.k.emplace();
  *out.k = std::move(*kr);
  return out;
}

}  // namespace

TEST_CASE("driftwire nat: a hard NAT meets an easy one by the birthday punch, either side hard",
          "[driftwire]") {
  for (const bool receiver_hard : {true, false}) {
    INFO("receiver hard: " << receiver_hard);
    swarm sw(12);
    stand_in_nat hard_nat(true), easy_nat(false);
    auto hr = hub_behind(receiver_hard ? &hard_nat : &easy_nat);
    auto hs = hub_behind(receiver_hard ? &easy_nat : &hard_nat);
    dht::meet_options o;
    o.tcp = false;  // UDP only (the default): the birthday punch alone
    o.birthday_lo = hard_nat.lo;
    o.birthday_hi = hard_nat.hi();
    bool r_hard = false, s_hard = false;
    const auto t0 = std::chrono::steady_clock::now();
    auto met = meet_both(sw, hr, hs, o, &r_hard, &s_hard);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    UNSCOPED_INFO("met in " << secs << " s");
    // The DHT saw the hard side on many ports, the easy side on one.
    CHECK(r_hard == receiver_hard);
    CHECK(s_hard == !receiver_hard);
    REQUIRE(met.r);
    REQUIRE(met.s);
    // The hard side's stream runs on the one birthday socket that met; the
    // other 255 are closed. Its main socket punches the easy side too, so
    // the easy side's random probes can meet that one instead (about 1 run
    // in 20 here): then no extra socket is kept.
    auto& hard_hub = receiver_hard ? hr : hs;
    CHECK(hard_hub->extra_count() <= 1);
    CHECK((receiver_hard ? hs : hr)->extra_count() == 0);
    handshake_over(std::move(*met.r), std::move(*met.s), *met.k);
    // The links are gone: so is the stream's birthday socket, and the hub is
    // back to its one socket (and its reader to the idle wait).
    CHECK(hard_hub->extra_count() == 0);
  }
}

TEST_CASE("driftwire nat: two hard NATs do not meet over UDP, and every extra socket is closed", "[driftwire]") {
  swarm sw(12);
  stand_in_nat a(true), b(true);
  auto hr = hub_behind(&a);
  auto hs = hub_behind(&b);
  dht::meet_options o;
  o.tcp = false;
  o.punch_ms = 2500;
  auto met = meet_both(sw, hr, hs, o);
  REQUIRE_FALSE(met.r);
  REQUIRE_FALSE(met.s);
  CHECK(met.s.error() == mv::status::timeout);
  // Neither side birthday-punches another hard NAT: it would not meet.
  CHECK(hr->extra_count() == 0);
  CHECK(hs->extra_count() == 0);
}

TEST_CASE("driftwire nat: TCP simultaneous open connects two ports that each connect to the other",
          "[driftwire]") {
  auto a = sp::net::open_rendezvous(true);
  auto b = sp::net::open_rendezvous(true);
  REQUIRE(a);
  REQUIRE(b);
  const sp::net::endpoint at_a{"127.0.0.1", (*a)->port()}, at_b{"127.0.0.1", (*b)->port()};
  mv::result<std::unique_ptr<sp::link>> la = mv::err(mv::status::internal);
  std::thread t([&] { la = (*a)->next(std::span<const sp::net::endpoint>(&at_b, 1), 5000); });
  auto lb = (*b)->next(std::span<const sp::net::endpoint>(&at_a, 1), 5000);
  t.join();
  REQUIRE(la);
  REQUIRE(lb);
  // One connection between the two ports, whoever's SYN got there: bytes both ways.
  const std::uint8_t ping[] = {1, 2, 3, 4};
  REQUIRE((*la)->write_all(ping));
  std::uint8_t got[4] = {};
  REQUIRE(sp::read_exact_until(**lb, got, std::chrono::steady_clock::now() + std::chrono::seconds(5)));
  CHECK(std::memcmp(got, ping, 4) == 0);
  REQUIRE((*lb)->write_all(ping));
  REQUIRE(sp::read_exact_until(**la, got, std::chrono::steady_clock::now() + std::chrono::seconds(5)));
  // A port nobody is at: the attempts are tried again until the time is up, then a timeout.
  auto c = sp::net::open_rendezvous(true);
  REQUIRE(c);
  const sp::net::endpoint nobody{"127.0.0.1", 9};
  const auto t0 = std::chrono::steady_clock::now();
  auto none = (*c)->next(std::span<const sp::net::endpoint>(&nobody, 1), 700);
  REQUIRE_FALSE(none);
  CHECK(none.error() == mv::status::timeout);
  CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(650));
  (*c)->shutdown();
  auto after = (*c)->next(std::span<const sp::net::endpoint>(&nobody, 1), 700);
  REQUIRE_FALSE(after);
  CHECK(after.error() == mv::status::cancelled);
}

TEST_CASE("driftwire nat: two hard NATs meet over TCP simultaneous open when UDP cannot", "[driftwire]") {
  swarm sw(12);
  stand_in_nat a(true), b(true);  // UDP only; the stand-ins leave TCP alone (a NAT that keeps TCP ports)
  auto hr = hub_behind(&a);
  auto hs = hub_behind(&b);
  dht::meet_options o;
  o.punch_ms = 8000;
  o.tcp = true;
  auto met = meet_both(sw, hr, hs, o);
  REQUIRE(met.r);
  REQUIRE(met.s);
  CHECK((*met.r)->via() == sp::transport::internet);
  handshake_over(std::move(*met.r), std::move(*met.s), *met.k);
}

TEST_CASE("driftwire nat: the hub reads extra sockets with its own and runs a stream on one", "[driftwire]") {
  hub_pair h;
  std::atomic<int> on_extra{0};
  std::atomic<std::uint32_t> last_sock{0};
  h.b->on_probe([&](std::span<const std::uint8_t>, const sp::net::endpoint&, std::uint32_t sock) {
    last_sock.store(sock);
    ++on_extra;
  });
  const auto ids = h.b->open_extras(8);
  REQUIRE(ids.size() == 8);
  CHECK(h.b->extra_count() == 8);
  // A probe from b's fifth extra to a, and a's answer back to where it came from.
  sp::net::endpoint seen;
  std::atomic<bool> got{false};
  h.a->on_probe([&](std::span<const std::uint8_t>, const sp::net::endpoint& from, std::uint32_t) {
    seen = from;
    got.store(true);
  });
  const std::uint8_t probe[] = {sp::kProbe, 9};
  REQUIRE(h.b->send(probe, h.at_a(), ids[4]));
  REQUIRE(eventually([&] { return got.load(); }, 5));
  CHECK(seen.port != h.b->port());
  REQUIRE(h.a->send(probe, {"127.0.0.1", seen.port}));
  REQUIRE(eventually([&] { return on_extra.load() == 1; }, 5));
  CHECK(last_sock.load() == ids[4]);
  // A stream on that extra, both ways.
  auto sb = h.b->stream(h.at_a(), 5, false, ids[4]);
  auto sa = h.a->stream({"127.0.0.1", seen.port}, 5);
  const auto data = pattern(300 * 1024, 4);
  std::atomic<bool> wrote{false};  // asserted here, not on the writer's thread (Catch2)
  std::thread w([&] { wrote.store(static_cast<bool>(sb->write_all(data))); });
  std::vector<std::uint8_t> back(data.size());
  const bool read = static_cast<bool>(
      sp::read_exact_until(*sa, back, std::chrono::steady_clock::now() + std::chrono::seconds(10)));
  w.join();
  REQUIRE(wrote.load());
  REQUIRE(read);
  CHECK(back == data);
  // The others closed; a send on a closed one says so, and the stream lives on.
  std::vector<std::uint32_t> rest(ids.begin(), ids.end());
  rest.erase(rest.begin() + 4);
  h.b->close_extras(rest);
  CHECK(h.b->extra_count() == 1);
  CHECK(h.b->send(probe, h.at_a(), ids[0]).error() == mv::status::not_found);
  const std::uint8_t more[] = {7, 7};
  REQUIRE(sb->write_all(more));
  std::uint8_t two[2];
  REQUIRE(sp::read_exact_until(*sa, two, std::chrono::steady_clock::now() + std::chrono::seconds(5)));
  CHECK(two[0] == 7);
  // The stream ended: its extra closes with it.
  sb.reset();
  CHECK(h.b->extra_count() == 0);
}

// ---- two engines over four words ----------------------------------------------------------------

namespace {
struct word_client {
  fake_host host;
  sp::engine eng;
  // `secrets`: a folder for the secret store (#472 PR 4); empty: none.
  // `tweak` (#472 relays): changes to the options before the engine starts.
  word_client(const fs::path& received, const std::string& name, const swarm& s, const fs::path& secrets = {},
              const std::function<void(sp::engine::options&)>& tweak = {})
      : host(received), eng(host, tweaked(options(name, s, secrets), tweak)) {
    eng.screen_opened("");
    eng.on_event("internet", 2, "true");
    eng.on_event("dht", 2, "true");
  }
  static sp::engine::options tweaked(sp::engine::options o, const std::function<void(sp::engine::options&)>& tweak) {
    if (tweak) tweak(o);
    return o;
  }
  static sp::engine::options options(const std::string& name, const swarm& s, const fs::path& secrets) {
    sp::engine::options o;
    if (!secrets.empty()) {
      fs::create_directories(secrets);
      o.make_secrets = [dir = utf8(secrets)] { return mv::driftwire::secrets::file_store(dir); };
    }
    o.known_poll_ms = 300;
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

TEST_CASE("driftwire dht: four words send files between two engines, through the DHT and a punched path",
          "[driftwire]") {
  scratch_dir dir("driftwire_words_send");
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
// mv_import_tests "[.driftwire-dht-live]": joins the public BitTorrent DHT
// from this machine (its routers, then a saved table under MV_DHT_TABLE if
// set), and times MV_DHT_LOOKUPS gets of random targets (default 10). Read
// only: nothing is put. DHT nodes see this machine's address taking part.
// Prints one JSON line: start time, nodes known, lookup p50 / p95, and the
// public address votes (how many distinct ports: a hard NAT shows several).
TEST_CASE("driftwire dht: lookups on the public DHT (harness)", "[.driftwire-dht-live]") {
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

// ---- known devices (#472 PR 4) ----------------------------------------------------------------

namespace {
// Ann sends `f` to Bea with four words, and both see the Remember toggle.
void send_with_words(word_client& ann, word_client& bea, const fs::path& f) {
  ann.eng.set_files({utf8(f)});
  REQUIRE(eventually([&] { return ann.eng.snapshot().file_count == 1; }));
  bea.eng.on_event("make_code", 1, "");
  std::string words;
  REQUIRE(eventually([&] {
    const auto v = bea.eng.snapshot();
    words = v.code_text;
    return v.code_phase == static_cast<std::uint8_t>(sp::internet_receiver::phase::ready) && !words.empty();
  }, 20));
  ann.eng.on_event("send_code", 2, "\"" + words + "\"");
  ann.eng.on_event("send_to_code", 1, "");
  const bool named = eventually([&] { return ann.eng.snapshot().out == sp::view::out_phase::naming; }, 30);
  INFO("out " << static_cast<int>(ann.eng.snapshot().out) << ": " << ann.eng.snapshot().out_note);
  REQUIRE(named);
  ann.eng.on_event("out_confirm", 1, "");
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  bea.eng.on_event("accept", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  REQUIRE(ann.eng.snapshot().out == sp::view::out_phase::done);
  REQUIRE(bea.eng.snapshot().in == sp::view::in_phase::done);
}

fs::path file_of(const scratch_dir& dir, const char* name, std::size_t size, std::uint32_t seed) {
  const fs::path f = dir / "out" / name;
  write_bytes(f, pattern(size, seed));
  return f;
}
}  // namespace

TEST_CASE("driftwire known: two devices that remember each other send again with no code", "[driftwire]") {
  scratch_dir dir("driftwire_known_send");
  swarm s(16);
  word_client bea(dir / "bea_received", "Bea", s, dir / "bea_secrets");
  word_client ann(dir / "ann_received", "Ann", s, dir / "ann_secrets");
  CHECK(ann.eng.snapshot().can_remember);
  send_with_words(ann, bea, file_of(dir, "first.jpg", 300 * 1024, 3));

  // Both offer Remember, named for the other; ticking keeps it.
  CHECK(ann.eng.snapshot().remember_offer);
  CHECK(ann.eng.snapshot().remember_peer == "Bea");
  CHECK_FALSE(ann.eng.snapshot().remembered);
  CHECK(bea.eng.snapshot().remember_offer);
  CHECK(sp::build_tree(ann.eng.snapshot()).find("\"remember\"") != std::string::npos);
  ann.eng.on_event("remember", 2, "true");
  bea.eng.on_event("remember", 2, "true");
  CHECK(ann.eng.snapshot().remembered);
  CHECK(ann.eng.snapshot().known == std::vector<std::string>{"Bea"});
  CHECK(bea.eng.snapshot().known == std::vector<std::string>{"Ann"});

  // A send to Bea now: no code, no name to confirm.
  const fs::path second = file_of(dir, "second.mov", 1024 * 1024 + 7, 4);
  ann.eng.set_files({utf8(second)});
  REQUIRE(eventually([&] {
    const auto v = ann.eng.snapshot();
    return !v.reading_folder && v.file_names == std::vector<std::string>{"second.mov"};
  }));
  ann.eng.on_event("known", 5, "\"dev" + ann.eng.snapshot().known_ids.at(0) + "\"");
  CHECK(ann.eng.snapshot().out_known);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }, 60));
  CHECK(bea.eng.snapshot().in_known);
  CHECK(bea.eng.snapshot().in_peer == "Ann");
  bea.eng.on_event("accept", 1, "");  // Bea still decides
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  INFO(ann.eng.snapshot().out_note);
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::done);
  CHECK(read_bytes(dir / "bea_received" / "second.mov") == read_bytes(second));
}

// #507's sibling: two networks where UDP cannot be punched (two hard NATs
// that keep TCP ports) met on four words over TCP, and both remembered each
// other; a send to the remembered device then found the other's item, punched
// UDP alone, and waited forever. Its meeting must try TCP as the words' did.
TEST_CASE("driftwire known: two devices behind hard NATs send again over TCP as words do", "[driftwire]") {
  scratch_dir dir("driftwire_known_hard_nats");
  swarm s(16);
  {
    word_client bea(dir / "bea_received", "Bea", s, dir / "bea_secrets");
    word_client ann(dir / "ann_received", "Ann", s, dir / "ann_secrets");
    send_with_words(ann, bea, file_of(dir, "first.jpg", 100 * 1024, 31));
    ann.eng.on_event("remember", 2, "true");
    bea.eng.on_event("remember", 2, "true");
  }
  // The same two computers, each now behind a NAT that maps every
  // destination apart (UDP never meets) and leaves TCP alone.
  stand_in_nat bea_nat(true), ann_nat(true);
  const auto behind = [](stand_in_nat& nat) {
    return [&nat](sp::engine::options& o) { o.wrap_udp = nat.wrap(); };
  };
  word_client bea(dir / "bea_received", "Bea", s, dir / "bea_secrets", behind(bea_nat));
  word_client ann(dir / "ann_received", "Ann", s, dir / "ann_secrets", behind(ann_nat));
  REQUIRE(ann.eng.snapshot().known == std::vector<std::string>{"Bea"});
  REQUIRE(bea.eng.snapshot().known == std::vector<std::string>{"Ann"});
  const fs::path second = file_of(dir, "second.mov", 512 * 1024 + 3, 32);
  ann.eng.set_files({utf8(second)});
  REQUIRE(eventually([&] {
    const auto v = ann.eng.snapshot();
    return !v.reading_folder && v.file_names == std::vector<std::string>{"second.mov"};
  }));
  ann.eng.on_event("known", 5, "\"dev0\"");
  const bool asked = eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }, 45);
  INFO("Ann: " << ann.eng.snapshot().out_note);
  if (!asked) ann.eng.on_event("out_cancel", 1, "");
  REQUIRE(asked);
  CHECK(bea.eng.snapshot().in_known);
  bea.eng.on_event("accept", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::done);
  CHECK(read_bytes(dir / "bea_received" / "second.mov") == read_bytes(second));
}

TEST_CASE("driftwire known: the list and the identity outlive the engine, and Forget forgets", "[driftwire]") {
  scratch_dir dir("driftwire_known_kept");
  swarm s(12);
  {
    word_client bea(dir / "bea_received", "Bea", s, dir / "bea_secrets");
    word_client ann(dir / "ann_received", "Ann", s, dir / "ann_secrets");
    send_with_words(ann, bea, file_of(dir, "a.jpg", 100 * 1024, 5));
    ann.eng.on_event("remember", 2, "true");
  }
  word_client ann(dir / "ann_received", "Ann", s, dir / "ann_secrets");
  CHECK(ann.eng.snapshot().known == std::vector<std::string>{"Bea"});
  const std::string tree = sp::build_tree(ann.eng.snapshot());
  CHECK(tree.find("Remembered devices") != std::string::npos);
  CHECK(tree.find("Forget Bea") != std::string::npos);
  ann.eng.on_event("forget" + ann.eng.snapshot().known_ids.at(0), 1, "");
  CHECK(ann.eng.snapshot().known.empty());
  word_client again(dir / "ann_received", "Ann", s, dir / "ann_secrets");
  CHECK(again.eng.snapshot().known.empty());
  // Without a store nothing can be remembered: no toggle.
  word_client none(dir / "none_received", "Cid", s);
  CHECK_FALSE(none.eng.snapshot().can_remember);
}

TEST_CASE("driftwire known: a remembered device with another key is refused, loudly", "[driftwire]") {
  scratch_dir dir("driftwire_known_changed");
  swarm s(16);
  {
    word_client bea(dir / "bea_received", "Bea", s, dir / "bea_secrets");
    word_client ann(dir / "ann_received", "Ann", s, dir / "ann_secrets");
    send_with_words(ann, bea, file_of(dir, "a.jpg", 100 * 1024, 6));
    ann.eng.on_event("remember", 2, "true");
    bea.eng.on_event("remember", 2, "true");
  }
  // Someone holding the pair's secret (a copied record) answers for Bea,
  // under Bea's direction, with another key.
  namespace known = mv::driftwire::known;
  auto bea_store = mv::driftwire::secrets::file_store(utf8(dir / "bea_secrets"));
  auto ann_store = mv::driftwire::secrets::file_store(utf8(dir / "ann_secrets"));
  const auto pair = known::load(*bea_store);
  const auto bea_key = known::load(*ann_store);
  REQUIRE(pair.size() == 1);
  REQUIRE(bea_key.size() == 1);
  const auto k = known::presence_keys(pair[0], known::hour_of(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                  std::chrono::system_clock::now().time_since_epoch())
                                                                  .count()),
                                      bea_key[0].key);
  auto hub = sp::udp_hub::open(0, true);
  REQUIRE(hub);
  dht::client client(*hub, test_options(s));
  REQUIRE(client.start({}));
  std::atomic<bool> done{false};
  std::atomic<bool> opened{false};
  std::thread impostor([&] {
    dht::meet_options mo;
    mo.loopback = true;
    mo.wait_ms = 60000;
    auto met = dht::meet(*hub, client, k, dht::role::receiver, mo, [&] { return done.load(); }, {});
    if (!met) return;
    std::shared_ptr<sp::link> l(std::move(*met));
    auto h = sp::channel::read_hello(*l, std::chrono::steady_clock::now() + std::chrono::seconds(10));
    if (!h) return;
    sp::channel::options o;
    std::array<std::uint8_t, 32> other{};
    randombytes_buf(other.data(), other.size());
    o.static_key = other;
    o.internet_secret = std::vector<std::uint8_t>(k.password.begin(), k.password.end());
    auto ch = sp::channel::open_server(l, *h, "cccccccccccccccc", "Bea", o);
    opened.store(static_cast<bool>(ch));
    if (ch) (void)eventually([&] { return done.load(); }, 30);
  });
  word_client ann(dir / "ann_received", "Ann", s, dir / "ann_secrets");
  const fs::path f = file_of(dir, "b.jpg", 100 * 1024, 7);
  ann.eng.set_files({utf8(f)});
  REQUIRE(eventually([&] { return ann.eng.snapshot().file_count == 1; }));
  ann.eng.on_event("known", 5, "\"dev" + ann.eng.snapshot().known_ids.at(0) + "\"");
  const bool ended = eventually([&] { return ann.eng.snapshot().out == sp::view::out_phase::ended; }, 60);
  done.store(true);
  impostor.join();
  REQUIRE(ended);
  CHECK(opened.load());  // it held the secret: the handshake worked, the key did not
  INFO(ann.eng.snapshot().out_note);
  CHECK(ann.eng.snapshot().out_note.find("security key has changed") != std::string::npos);
  CHECK(ann.eng.snapshot().out_note.find("Nothing was sent") != std::string::npos);
}

TEST_CASE("driftwire known: a send to a device that does not answer waits, and Cancel ends it", "[driftwire]") {
  scratch_dir dir("driftwire_known_waits");
  swarm s(12);
  {
    word_client bea(dir / "bea_received", "Bea", s, dir / "bea_secrets");
    word_client ann(dir / "ann_received", "Ann", s, dir / "ann_secrets");
    send_with_words(ann, bea, file_of(dir, "a.jpg", 100 * 1024, 8));
    ann.eng.on_event("remember", 2, "true");  // Bea does not remember Ann
  }
  word_client ann(dir / "ann_received", "Ann", s, dir / "ann_secrets");
  const fs::path f = file_of(dir, "b.jpg", 100 * 1024, 9);
  ann.eng.set_files({utf8(f)});
  REQUIRE(eventually([&] { return ann.eng.snapshot().file_count == 1; }));
  ann.eng.on_event("known", 5, "\"dev" + ann.eng.snapshot().known_ids.at(0) + "\"");
  const bool waits =
      eventually([&] { return ann.eng.snapshot().out_note.find("Waiting for Bea to come online") == 0; }, 20);
  INFO(ann.eng.snapshot().out_note << " / " << ann.eng.snapshot().send_code_note << " / "
                                   << static_cast<int>(ann.eng.snapshot().out));
  REQUIRE(waits);
  std::this_thread::sleep_for(std::chrono::seconds(2));
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::reaching);
  CHECK(sp::build_tree(ann.eng.snapshot()).find("Waiting for Bea") != std::string::npos);
  ann.eng.on_event("out_cancel", 1, "");
  ann.eng.wait_idle();
  CHECK(ann.eng.snapshot().out_note == "Cancelled. Nothing was sent.");
}

// ---- relays through the person's own computers (#472, option 9) ------------------------------

namespace {
// Bea's code, asked for until the receiver of codes is up.
std::string direct_code(word_client& c) {
  std::string code;
  auto asked = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  REQUIRE(eventually([&] {
    const auto v = c.eng.snapshot();
    if (v.code_phase == 0 && std::chrono::steady_clock::now() - asked > std::chrono::milliseconds(300)) {
      c.eng.on_event("make_code", 1, "");
      asked = std::chrono::steady_clock::now();
    }
    code = v.code_text;
    return v.code_phase == static_cast<std::uint8_t>(sp::internet_receiver::phase::ready) && !code.empty();
  }, 20));
  REQUIRE(std::count(code.begin(), code.end(), ' ') == 0);  // a direct code, not words
  return code;
}

// Ann's connects all time out: Bea's places are out of her reach.
void cut_off(sp::engine::options& o) {
  o.relay_meet_ms = 60000;
  o.connect_wifi = [](const sp::net::endpoint&, int) -> mv::result<std::unique_ptr<sp::link>> {
    return mv::err(mv::status::timeout);
  };
}

void direct_codes(sp::engine::options& o) { o.internet_force_words = false; }
}  // namespace

TEST_CASE("driftwire relay: a send goes through another of the person's own computers, sealed end to end",
          "[driftwire]") {
  scratch_dir dir("driftwire_own_relay");
  swarm s(16);
  std::mutex m;
  std::vector<std::uint8_t> seen;
  word_client rob(dir / "rob_received", "Rob", s, dir / "rob_secrets", [&](sp::engine::options& o) {
    o.relay_limits.tap = [&](bool, std::span<const std::uint8_t> b) {
      std::lock_guard lock(m);
      seen.insert(seen.end(), b.begin(), b.end());
    };
  });
  word_client ann(dir / "ann_received", "Ann", s, dir / "ann_secrets", cut_off);
  send_with_words(ann, rob, file_of(dir, "first.jpg", 100 * 1024, 30));
  ann.eng.on_event("remember", 2, "true");
  rob.eng.on_event("remember", 2, "true");
  for (word_client* c : {&ann, &rob}) {
    c->eng.on_event("relay", 2, "true");
    c->eng.on_event("own" + c->eng.snapshot().known_ids.at(0), 2, "true");
  }
  CHECK(ann.eng.snapshot().known_own == std::vector<bool>{true});
  CHECK(sp::build_tree(ann.eng.snapshot()).find("Rob is my computer") != std::string::npos);
  rob.eng.on_event("in_dismiss", 1, "");  // the pairing send's result

  // Bea, somewhere else, with a direct code Ann cannot reach.
  word_client bea(dir / "bea_received", "Bea", s, {}, direct_codes);
  const std::string code = direct_code(bea);
  const fs::path f = dir / "out" / "clip.mov";
  const auto bytes = pattern(1536 * 1024 + 77, 31);
  write_bytes(f, bytes);
  ann.eng.set_files({utf8(f)});
  REQUIRE(eventually([&] {
    const auto v = ann.eng.snapshot();
    return !v.reading_folder && v.file_names == std::vector<std::string>{"clip.mov"};
  }));
  ann.eng.on_event("send_code", 2, "\"" + code + "\"");
  ann.eng.on_event("send_to_code", 1, "");
  const bool named = eventually([&] { return ann.eng.snapshot().out == sp::view::out_phase::naming; }, 90);
  INFO(ann.eng.snapshot().out_note);
  REQUIRE(named);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::connecting; }));
  CHECK(ann.eng.snapshot().out_relay == "Rob");
  CHECK(rob.eng.snapshot().relaying_for == "Ann");
  CHECK(sp::build_tree(rob.eng.snapshot()).find("Relaying for Ann") != std::string::npos);
  CHECK(sp::build_tree(ann.eng.snapshot()).find("Through Rob (your computer)") != std::string::npos);
  CHECK(ann.eng.snapshot().out_check == bea.eng.snapshot().in_code);  // one session, A to B
  CHECK(bea.eng.snapshot().in_peer == "Ann");
  ann.eng.on_event("out_confirm", 1, "");
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  bea.eng.on_event("accept", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  INFO(ann.eng.snapshot().out_note);
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::done);
  CHECK(read_bytes(dir / "bea_received" / "clip.mov") == bytes);
  REQUIRE(eventually([&] { return rob.eng.snapshot().relaying_for.empty(); }));
  CHECK(rob.eng.snapshot().relayed == 1);
  CHECK(rob.eng.snapshot().in == sp::view::in_phase::none);  // it received nothing itself
  std::lock_guard lock(m);
  CHECK(seen.size() > bytes.size());
  for (std::size_t at = 0; at + 32 <= bytes.size(); at += 4096) {
    CHECK(std::search(seen.begin(), seen.end(), bytes.begin() + at, bytes.begin() + at + 32) == seen.end());
  }
}

TEST_CASE("driftwire relay: a computer not marked as the person's own there does not relay", "[driftwire]") {
  scratch_dir dir("driftwire_own_relay_off");
  swarm s(12);
  word_client rob(dir / "rob_received", "Rob", s, dir / "rob_secrets");
  word_client ann(dir / "ann_received", "Ann", s, dir / "ann_secrets", cut_off);
  send_with_words(ann, rob, file_of(dir, "first.jpg", 100 * 1024, 32));
  ann.eng.on_event("remember", 2, "true");
  rob.eng.on_event("remember", 2, "true");
  ann.eng.on_event("relay", 2, "true");
  ann.eng.on_event("own" + ann.eng.snapshot().known_ids.at(0), 2, "true");
  rob.eng.on_event("relay", 2, "true");  // but Ann is not marked as Rob's person's own there
  word_client bea(dir / "bea_received", "Bea", s, {}, direct_codes);
  const std::string code = direct_code(bea);
  const fs::path f = file_of(dir, "b.jpg", 100 * 1024, 33);
  ann.eng.set_files({utf8(f)});
  REQUIRE(eventually([&] {
    const auto v = ann.eng.snapshot();
    return !v.reading_folder && v.file_names == std::vector<std::string>{"b.jpg"};
  }));
  ann.eng.on_event("send_code", 2, "\"" + code + "\"");
  ann.eng.on_event("send_to_code", 1, "");
  const bool said =
      eventually([&] { return ann.eng.snapshot().out_note.find("Rob does not relay") != std::string::npos; }, 90);
  INFO(ann.eng.snapshot().out_note);
  CHECK(said);
  CHECK(rob.eng.snapshot().relayed == 0);
  CHECK(bea.eng.snapshot().in == sp::view::in_phase::none);
  ann.eng.on_event("out_cancel", 1, "");
  ann.eng.wait_idle();
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::ended);
}
