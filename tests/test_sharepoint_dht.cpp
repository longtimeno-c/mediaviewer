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
