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
