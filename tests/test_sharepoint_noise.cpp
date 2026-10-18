// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Sharepoint's Noise (src/addons/sharepoint/noise.h, issue #472): the core
// checked against the published Cacophony vectors (the same vectors snow,
// noise-c and cacophony itself pass), and the hybrid patterns protocol 2
// speaks: both sides agree, the ML-KEM-768 secret is in the keys, a fixed
// transcript stays fixed, and a tampered message does not open.
#include "catch_compat.h"

#include <sodium.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "addons/sharepoint/cpace.h"
#include "addons/sharepoint/noise.h"

namespace noise = mv::sharepoint::noise;

namespace {

std::vector<std::uint8_t> hex(std::string_view s) {
  std::vector<std::uint8_t> out;
  for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
    out.push_back(static_cast<std::uint8_t>(std::stoi(std::string(s.substr(i, 2)), nullptr, 16)));
  }
  return out;
}

std::array<std::uint8_t, 32> key32(std::string_view s) {
  const auto v = hex(s);
  std::array<std::uint8_t, 32> k{};
  REQUIRE(v.size() == 32);
  std::copy(v.begin(), v.end(), k.begin());
  return k;
}

std::string to_hex(std::span<const std::uint8_t> b) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string s;
  for (auto x : b) {
    s.push_back(kHex[x >> 4]);
    s.push_back(kHex[x & 15]);
  }
  return s;
}

struct vector_message {
  const char* payload;
  const char* ciphertext;
};

// Runs a Cacophony vector: the handshake messages must be byte for byte, h
// at the end must match, and the transport messages after it too.
void run_vector(const noise::pattern& p, std::size_t handshake_messages, std::string_view prologue,
                std::optional<std::string_view> init_static, std::optional<std::string_view> resp_static,
                std::optional<std::string_view> psk, std::string_view init_e, std::string_view resp_e,
                std::string_view handshake_hash, std::span<const vector_message> messages) {
  const auto pro = hex(prologue);
  noise::test_keys ik, rk;
  ik.e = key32(init_e);
  rk.e = key32(resp_e);
  std::optional<std::array<std::uint8_t, 32>> is, rs, ipsk, rpsk;
  if (init_static) is = key32(*init_static);
  if (resp_static) rs = key32(*resp_static);
  if (psk) ipsk = rpsk = key32(*psk);
  noise::handshake init(p, true, pro, is, ipsk, ik);
  noise::handshake resp(p, false, pro, rs, rpsk, rk);
  std::vector<std::uint8_t> out, got;
  for (std::size_t i = 0; i < handshake_messages; ++i) {
    noise::handshake& w = i % 2 == 0 ? init : resp;
    noise::handshake& r = i % 2 == 0 ? resp : init;
    INFO("handshake message " << i);
    REQUIRE(w.write_message(hex(messages[i].payload), out));
    CHECK(to_hex(out) == messages[i].ciphertext);
    REQUIRE(r.read_message(out, got));
    CHECK(to_hex(got) == messages[i].payload);
  }
  REQUIRE(init.done());
  REQUIRE(resp.done());
  CHECK(to_hex(init.hash()) == handshake_hash);
  CHECK(to_hex(resp.hash()) == handshake_hash);
  auto ki = init.split();
  auto kr = resp.split();
  REQUIRE(ki);
  REQUIRE(kr);
  noise::cipher_state i2r, r2i, i2r_in, r2i_in;
  i2r.init(ki->initiator_to_responder);
  r2i.init(ki->responder_to_initiator);
  i2r_in.init(kr->initiator_to_responder);
  r2i_in.init(kr->responder_to_initiator);
  for (std::size_t i = handshake_messages; i < messages.size(); ++i) {
    INFO("transport message " << i);
    // The messages keep alternating after the handshake: the initiator sends
    // the even ones.
    const bool from_initiator = i % 2 == 0;
    noise::cipher_state& tx = from_initiator ? i2r : r2i;
    noise::cipher_state& rx = from_initiator ? i2r_in : r2i_in;
    REQUIRE(tx.encrypt({}, hex(messages[i].payload), out));
    CHECK(to_hex(out) == messages[i].ciphertext);
    REQUIRE(rx.decrypt({}, out, got));
    CHECK(to_hex(got) == messages[i].payload);
  }
}

}  // namespace

TEST_CASE("sharepoint noise: Noise_XX_25519_ChaChaPoly_BLAKE2b matches the Cacophony vector", "[sharepoint]") {
  static constexpr vector_message kMessages[] = {
      {"4c756477696720766f6e204d69736573",
       "ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c79444c756477696720766f6e204d69736573"},
      {"4d757272617920526f746862617264",
       "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f1448088430505b6745ce64a5f33f0e8e3b83f11ce8802bca507f4f2"
       "d8b564dbe277e1966116e132faa2dfd70b8b077b9f94b913df5056ae1319469b824a98d54bbaa82c325595587064f978c4b6d104f7596e"
       "6f"},
      {"462e20412e20486179656b",
       "99579e1c1ee15e422a57ddd6b16d37087b17558e8369c18991b4b2ca3a824abf904cdcf5458b5431a75af034ca9e9b982de039eaaf156"
       "775e2d580cd4e5ebae89c3f8cb2594b556d8a8169"},
      {"4361726c204d656e676572", "fc56eea290b3f3a21aac0c70cd5787b5ee99be37d2f4d751329b55"},
      {"4a65616e2d426170746973746520536179", "bb31c9da10d5639a4cdb88a12f5c61de41bbc7df09bf75d94f8184fe4157f5c68f"},
      {"457567656e2042f6686d20766f6e2042617765726b",
       "f6199cadb152fb27f82be0a0891ec76a33598ae92a46cab2fb5a8ed5bf48b7f267f8370af7"},
  };
  run_vector(noise::xx(), 3, "4a6f686e2047616c74", "e61ef9919cde45dd5f82166404bd08e38bceb5dfdfded0a34c8df7ed542214d1",
             "4a3acbfdb163dec651dfa3194dece676d437029c62a408b4c5ea9114246e4893", std::nullopt,
             "893e28b9dc6ca8d611ab664754b8ceb7bac5117349a4439a6b0569da977c464a",
             "bbdb4cdbd309f1a1f2e1456967fe288cadd6f712d65dc7b7793d5e63da6b375b",
             "8cf47d7b3cb5804c0109d48e8bcdbee2cbb65687d8ea2c92994ca361fb86151ad93627b98936cbb32de56e8abb21def3925011ac3e"
             "35db9cbeea73ab9a4392c2",
             kMessages);
}

TEST_CASE("sharepoint noise: Noise_NNpsk0_25519_ChaChaPoly_BLAKE2b matches the Cacophony vector", "[sharepoint]") {
  static constexpr vector_message kMessages[] = {
      {"4c756477696720766f6e204d69736573",
       "ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c7944ed63df2f5a12aee1185ee9c50305f2ecf12421dcb53c04"
       "7a63b784cf7c54a105"},
      {"4d757272617920526f746862617264",
       "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f1448088430e0fe7d5c0c9b92f7478716c8852f1b4f389edb75e3e"
       "be546fafafd5b6f0f7"},
      {"462e20412e20486179656b", "0fe04b26dcfbb69f8a6a94c61f1a26ef88769218d32f5d17c068a6"},
      {"4361726c204d656e676572", "2239e0c540c01f09eb1f4cc5258fd5acfefff18a5a3773f21b7bfe"},
      {"4a65616e2d426170746973746520536179", "f216cb03d30fb8af1e561fe02a07552d09b416e27a75e62c77d193718b0bdaae9f"},
      {"457567656e2042f6686d20766f6e2042617765726b",
       "6e8baf98b944184dd7dc5ef14d8108cbeca626de118a5460f3308e294d61e17238bce29bd9"},
  };
  run_vector(noise::nn_psk0(), 2, "4a6f686e2047616c74", std::nullopt, std::nullopt,
             "54686973206973206d7920417573747269616e20706572737065637469766521",
             "893e28b9dc6ca8d611ab664754b8ceb7bac5117349a4439a6b0569da977c464a",
             "bbdb4cdbd309f1a1f2e1456967fe288cadd6f712d65dc7b7793d5e63da6b375b",
             "ed5e9692d0ab507b6c2beec3f584fd5b127817a9d20b26cd50aa72c507260fa31aa7d88dd3723316338af37ce0b4cfb2923aeb848b"
             "bf2b934911306f01ffc963",
             kMessages);
}

namespace {

struct hybrid_run {
  std::vector<std::vector<std::uint8_t>> wire;
  std::array<std::uint8_t, noise::kHashBytes> h{};
  std::array<std::uint8_t, 32> i2r{}, r2i{}, extra{};
  bool ok = false;
};

// One hybrid handshake with fixed keys on both sides; `kem_seed` is the
// encapsulation's randomness (the responder's ekem1).
hybrid_run run_hybrid(const noise::pattern& p, std::size_t n, std::uint8_t kem_seed, bool with_static,
                      std::optional<std::array<std::uint8_t, 32>> psk = std::nullopt) {
  noise::test_keys ik, rk;
  ik.e.emplace();
  ik.e->fill(0x11);
  rk.e.emplace();
  rk.e->fill(0x22);
  ik.e1_seed.emplace();
  ik.e1_seed->fill(0x33);
  rk.kem_seed.emplace();
  rk.kem_seed->fill(kem_seed);
  std::optional<std::array<std::uint8_t, 32>> is, rs;
  if (with_static) {
    is.emplace();
    is->fill(0x44);
    rs.emplace();
    rs->fill(0x55);
  }
  const std::vector<std::uint8_t> pro = {'t', 'e', 's', 't'};
  noise::handshake init(p, true, pro, is, psk, ik);
  noise::handshake resp(p, false, pro, rs, psk, rk);
  hybrid_run out;
  std::vector<std::uint8_t> m, payload;
  for (std::size_t i = 0; i < n; ++i) {
    noise::handshake& w = i % 2 == 0 ? init : resp;
    noise::handshake& r = i % 2 == 0 ? resp : init;
    if (!w.write_message({}, m) || !r.read_message(m, payload)) return out;
    out.wire.push_back(m);
  }
  auto ki = init.split();
  auto kr = resp.split();
  if (!ki || !kr) return out;
  out.ok = ki->initiator_to_responder == kr->initiator_to_responder &&
           ki->responder_to_initiator == kr->responder_to_initiator && ki->extra == kr->extra && ki->h == kr->h;
  out.h = ki->h;
  out.i2r = ki->initiator_to_responder;
  out.r2i = ki->responder_to_initiator;
  out.extra = ki->extra;
  return out;
}

}  // namespace

TEST_CASE("sharepoint noise: the hybrid XX handshake agrees, and its keys hold the ML-KEM secret", "[sharepoint]") {
  const hybrid_run a = run_hybrid(noise::xx_hfs(), 3, 0x66, true);
  REQUIRE(a.ok);
  // The sizes on the wire: e and e1 in the clear; then e, the KEM ciphertext
  // and s sealed, the empty payload's tag; then s and the payload's tag.
  REQUIRE(a.wire.size() == 3);
  CHECK(a.wire[0].size() == 32 + noise::kKemPublicBytes);
  CHECK(a.wire[1].size() == 32 + noise::kKemCiphertextBytes + 16 + 32 + 16 + 16);
  CHECK(a.wire[2].size() == 32 + 16 + 16);
  // The same X25519 keys with another KEM encapsulation: other keys. Breaking
  // X25519 alone does not give the session keys.
  const hybrid_run b = run_hybrid(noise::xx_hfs(), 3, 0x67, true);
  REQUIRE(b.ok);
  CHECK(a.i2r != b.i2r);
  CHECK(a.r2i != b.r2i);
  CHECK(a.h != b.h);
  // Fixed keys, fixed transcript: a regression vector for this build (made by
  // this code, so it guards against change, not against a shared mistake;
  // the Cacophony vectors above check the shared core).
  const hybrid_run again = run_hybrid(noise::xx_hfs(), 3, 0x66, true);
  CHECK(again.h == a.h);
  CHECK(to_hex(a.h) ==
        "2454b423f8478cc037b69ec327ebe7f24c51a41f53288789ecb854a8081c14da226e0067c1a038f54c905df74d51ae6ba63326b2e1cf"
        "389c59e032e77d9c3312");
}

TEST_CASE("sharepoint noise: the hybrid psk rejoin agrees only with the same psk", "[sharepoint]") {
  std::array<std::uint8_t, 32> psk;
  psk.fill(0x77);
  const hybrid_run a = run_hybrid(noise::nn_hfs_psk0(), 2, 0x66, false, psk);
  REQUIRE(a.ok);
  CHECK(to_hex(a.h) ==
        "7f297d39b2e2e485ed00fc6899c6a1eac90cdebca6919abd8e0ff2e57df0adf05e3952ced9763b2bdb2bb348d3b435aad4a91edbaf4e"
        "ecf005c7abbb1b03adf7");
  // Another psk on the responder: the first message does not open there.
  noise::test_keys ik;
  ik.e.emplace();
  ik.e->fill(0x11);
  ik.e1_seed.emplace();
  ik.e1_seed->fill(0x33);
  std::array<std::uint8_t, 32> other = psk;
  other[0] ^= 1;
  const std::vector<std::uint8_t> pro = {'t', 'e', 's', 't'};
  noise::handshake init(noise::nn_hfs_psk0(), true, pro, std::nullopt, psk, ik);
  noise::handshake resp(noise::nn_hfs_psk0(), false, pro, std::nullopt, other);
  std::vector<std::uint8_t> m, payload;
  REQUIRE(init.write_message({}, m));
  CHECK_FALSE(resp.read_message(m, payload));
}

TEST_CASE("sharepoint noise: a tampered KEM ciphertext or key does not open", "[sharepoint]") {
  for (int which = 0; which < 2; ++which) {
    noise::handshake init(noise::xx_hfs(), true, {}, std::array<std::uint8_t, 32>{1});
    noise::handshake resp(noise::xx_hfs(), false, {}, std::array<std::uint8_t, 32>{2});
    std::vector<std::uint8_t> m, payload;
    REQUIRE(init.write_message({}, m));
    if (which == 0) m[32 + 100] ^= 1;  // the initiator's ML-KEM key, in the clear
    REQUIRE(resp.read_message(m, payload));
    INFO("tampered part " << which);
    // A flipped key bit may leave a key ML-KEM refuses outright (its modulus
    // check): the responder then cannot answer, which also ends it.
    if (!resp.write_message({}, m)) {
      CHECK(which == 0);
      continue;
    }
    if (which == 1) m[32 + 10] ^= 1;  // the KEM ciphertext, sealed
    CHECK_FALSE(init.read_message(m, payload));
  }
}

TEST_CASE("sharepoint noise: HMAC-BLAKE2b is RFC 2104 over BLAKE2b-512", "[sharepoint]") {
  // libsodium's keyed BLAKE2b is not HMAC; check the construction against
  // one computed from the definition with the unkeyed hash.
  const std::vector<std::uint8_t> key(20, 0x0b);
  const std::string data = "Hi There";
  std::array<std::uint8_t, 64> mac;
  noise::hmac(key, {reinterpret_cast<const std::uint8_t*>(data.data()), data.size()}, mac);
  std::uint8_t k[128]{};
  std::copy(key.begin(), key.end(), k);
  std::uint8_t ipad[128], opad[128];
  for (int i = 0; i < 128; ++i) {
    ipad[i] = static_cast<std::uint8_t>(k[i] ^ 0x36);
    opad[i] = static_cast<std::uint8_t>(k[i] ^ 0x5c);
  }
  crypto_generichash_state st;
  std::uint8_t inner[64], outer[64];
  crypto_generichash_init(&st, nullptr, 0, 64);
  crypto_generichash_update(&st, ipad, 128);
  crypto_generichash_update(&st, reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
  crypto_generichash_final(&st, inner, 64);
  crypto_generichash_init(&st, nullptr, 0, 64);
  crypto_generichash_update(&st, opad, 128);
  crypto_generichash_update(&st, inner, 64);
  crypto_generichash_final(&st, outer, 64);
  CHECK(std::equal(mac.begin(), mac.end(), outer));
}

// ---- CPace (#472 PR 2): draft-irtf-cfrg-cpace-18, Appendix B.3 ------------------------------------

namespace {
std::vector<std::uint8_t> cat(std::initializer_list<const char*> parts) {
  std::string s;
  for (const char* p : parts) s += p;
  return hex(s);
}
}  // namespace

TEST_CASE("sharepoint cpace: ristretto255 / SHA-512 matches the draft's vectors", "[sharepoint]") {
  const std::string prs_text = "Password";
  const std::vector<std::uint8_t> prs(prs_text.begin(), prs_text.end());
  const auto ci = hex("6f630b425f726573706f6e6465720b415f696e69746961746f72");
  const auto sid = hex("7e4b4791d6a8ef019b936c79fb7f2c57");
  // B.3.1: the generator string and its hash.
  const auto gs = mv::sharepoint::cpace::generator_string(prs, ci, sid);
  CHECK(gs.size() == 172);
  CHECK(to_hex(gs) == to_hex(cat({"11435061636552697374726574746f3235350850617373776f726464",
                                   "00000000000000000000000000000000000000000000000000000000",
                                   "00000000000000000000000000000000000000000000000000000000",
                                   "00000000000000000000000000000000000000000000000000000000",
                                   "000000000000000000000000000000001a6f630b425f726573706f6e",
                                   "6465720b415f696e69746961746f72107e4b4791d6a8ef019b936c79", "fb7f2c57"})));
  std::array<std::uint8_t, 64> h;
  crypto_hash_sha512(h.data(), gs.data(), gs.size());
  CHECK(to_hex(h) == to_hex(cat({"c63a5750e2439c17ccd8213be14fde2f87e1bc637001a97f5929c77b",
                                  "30ea0e08afbc75ace5d3d73b2842a79d01488c5fd7ea30d475ee6095", "45af1bfd1ff77c8e"})));
  std::array<std::uint8_t, 32> g;
  REQUIRE(crypto_core_ristretto255_from_hash(g.data(), h.data()) == 0);
  CHECK(to_hex(g) == "a6fc82c3b8968fbb2e06fee81ca858586dea50d248f0c7ca6a18b0902a30b36b");
  // B.3.2-B.3.5: both messages, then the ISK each side computes.
  const auto ya = key32("da3d23700a9e5699258aef94dc060dfda5ebb61f02a5ea77fad53f4ff0976d08");
  const auto yb = key32("d2316b454718c35362d83d69df6320f38578ed5984651435e2949762d900b80d");
  mv::sharepoint::cpace::party a(prs, ci, sid, ya), b(prs, ci, sid, yb);
  REQUIRE(a.ok());
  REQUIRE(b.ok());
  CHECK(to_hex(a.message()) == "d40fb265a7abeaee7939d91a585fe59f7053f982c296ec413c624c669308f87a");
  CHECK(to_hex(b.message()) == "08bcf6e9777a9c313a3db6daa510f2d398403319c2341bd506a92e672eb7e307");
  const std::vector<std::uint8_t> ada = {'A', 'D', 'a'}, adb = {'A', 'D', 'b'};
  const auto a_msg = a.message(), b_msg = b.message();
  auto isk_a = a.finish(b_msg, true, ada, adb);
  auto isk_b = b.finish(a_msg, false, ada, adb);
  REQUIRE(isk_a);
  REQUIRE(isk_b);
  const std::string want = "4c5469a16b2364c4b944ebc1a79e51d1674ad47db26e8718154f59faebfaa52d8346f30aa58377117eb20d527f2cbc5c"
                           "76381f7fd372e89df8239f87f2e02ed1";
  CHECK(to_hex(*isk_a) == want);
  CHECK(to_hex(*isk_b) == want);
}

TEST_CASE("sharepoint cpace: another code gives another key; the identity and non-points are refused",
          "[sharepoint]") {
  const std::vector<std::uint8_t> right = {'1', '2', '3'}, wrong = {'1', '2', '4'}, ci = {'c'}, sid(16, 7);
  mv::sharepoint::cpace::party a(right, ci, sid), b(wrong, ci, sid);
  const auto am = a.message(), bm = b.message();
  auto ka = a.finish(bm, true, {}, {});
  auto kb = b.finish(am, false, {}, {});
  REQUIRE(ka);
  REQUIRE(kb);
  CHECK(*ka != *kb);
  // The identity (all zeros encodes it) and an encoding that is not a point.
  mv::sharepoint::cpace::party c(right, ci, sid), d(right, ci, sid);
  const std::array<std::uint8_t, 32> identity{};
  std::array<std::uint8_t, 32> junk;
  junk.fill(0xff);
  CHECK_FALSE(c.finish(identity, true, {}, {}));
  CHECK_FALSE(d.finish(junk, true, {}, {}));
}
