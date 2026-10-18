// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Sharepoint's known devices (issue #472, PR 4): the secret store's file
// form, the identity kept in it, the remembered devices' records, the
// hourly presence keys, and what a protocol 2 session gives Remember. The
// OS stores (Keychain, Credential Manager) are not touched here.
#include "sharepoint_fixture.h"

#include <sodium.h>

#include "addons/sharepoint/known.h"
#include "addons/sharepoint/secrets.h"

namespace known = mv::sharepoint::known;
namespace secrets = mv::sharepoint::secrets;

namespace {
std::vector<std::uint8_t> bytes_of(std::string_view s) { return {s.begin(), s.end()}; }
known::device device(const std::string& name, std::uint8_t fill) {
  known::device d;
  d.name = name;
  d.key.fill(fill);
  d.secret.fill(static_cast<std::uint8_t>(fill + 1));
  return d;
}
}  // namespace

TEST_CASE("sharepoint known: the file store keeps, replaces and forgets values by name", "[sharepoint]") {
  scratch_dir s("sharepoint_secret_store");
  auto store = secrets::file_store(utf8(s.root()));
  REQUIRE(store);
  CHECK_FALSE(store->get("identity"));
  REQUIRE(store->put("identity", bytes_of("first")));
  REQUIRE(store->put("devices/odd name", bytes_of("other")));
  CHECK(store->get("identity") == bytes_of("first"));
  REQUIRE(store->put("identity", bytes_of("second")));  // replaced, not appended
  CHECK(store->get("identity") == bytes_of("second"));
  CHECK(store->get("devices/odd name") == bytes_of("other"));
  CHECK_FALSE(store->put("big", std::vector<std::uint8_t>(secrets::kMaxValue + 1, 7)));
  CHECK(store->erase("identity"));
  CHECK_FALSE(store->get("identity"));
  CHECK_FALSE(store->erase("identity"));
}

TEST_CASE("sharepoint known: the identity is made once and kept; without a store it is this run's", "[sharepoint]") {
  scratch_dir s("sharepoint_identity");
  auto store = secrets::file_store(utf8(s.root()));
  bool kept = false;
  const auto first = known::identity(store.get(), kept);
  CHECK(kept);
  const auto again = known::identity(store.get(), kept);
  CHECK(kept);
  CHECK(first == again);
  // No store: a fresh key each time, and nothing is kept.
  const auto a = known::identity(nullptr, kept);
  CHECK_FALSE(kept);
  const auto b = known::identity(nullptr, kept);
  CHECK(a != b);
  CHECK(a != first);
}

TEST_CASE("sharepoint known: the remembered devices round-trip and a damaged record is refused", "[sharepoint]") {
  std::vector<known::device> list{device("Ann", 1), device("Bea\xE2\x80\x99s Mac", 9)};
  const auto bytes = known::encode(list);
  auto back = known::decode(bytes);
  REQUIRE(back);
  REQUIRE(back->size() == 2);
  CHECK((*back)[1].name == "Bea\xE2\x80\x99s Mac");
  CHECK((*back)[1].key == list[1].key);
  CHECK((*back)[1].secret == list[1].secret);
  for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
    CHECK_FALSE(known::decode(std::span(bytes).first(cut)));
  }
  auto longer = bytes;
  longer.push_back(0);
  CHECK_FALSE(known::decode(longer));
  // A long name is cut on a character boundary; the list holds kMaxDevices.
  std::string name;
  for (int i = 0; i < 40; ++i) name += "\xC3\xA9";  // 80 bytes of "é"
  std::vector<known::device> many;
  for (std::size_t i = 0; i < known::kMaxDevices + 3; ++i) many.push_back(device(name, static_cast<std::uint8_t>(i)));
  const auto big = known::encode(many);
  CHECK(big.size() <= secrets::kMaxValue);
  auto kept = known::decode(big);
  REQUIRE(kept);
  CHECK(kept->size() == known::kMaxDevices);
  CHECK((*kept)[0].name.size() == known::kMaxName);
  CHECK((*kept)[0].name == name.substr(0, known::kMaxName));
}

TEST_CASE("sharepoint known: the list is kept in the store, and an empty one leaves nothing", "[sharepoint]") {
  scratch_dir s("sharepoint_known_list");
  auto store = secrets::file_store(utf8(s.root()));
  CHECK(known::load(*store).empty());
  REQUIRE(known::save(*store, {device("Ann", 3)}));
  const auto list = known::load(*store);
  REQUIRE(list.size() == 1);
  CHECK(list[0].name == "Ann");
  REQUIRE(known::save(*store, {}));
  CHECK_FALSE(store->get(known::kDevicesName));
}

TEST_CASE("sharepoint known: each hour and each direction the pair gets new DHT keys, the same on both sides", "[sharepoint]") {
  const auto d = device("Ann", 4);
  const auto now = known::hour_of(1'800'000'000'000);
  CHECK(known::hour_of(1'800'000'000'000 + known::kHourMs) == now + 1);
  CHECK(known::hour_of(0) == 0);
  CHECK(known::hour_of(-1) == -1);
  std::array<std::uint8_t, 32> me{}, them{};
  me.fill(7);
  them.fill(8);
  const auto a = known::presence_keys(d, now, me);
  const auto b = known::presence_keys(d, now, me);
  CHECK(a.public_key == b.public_key);
  CHECK(a.password == b.password);
  CHECK(a.salt_base == b.salt_base);
  const auto next = known::presence_keys(d, now + 1, me);
  // The other direction: other keys.
  CHECK(known::presence_keys(d, now, them).public_key != a.public_key);
  CHECK(a.public_key != next.public_key);
  CHECK(a.box != next.box);
  CHECK(a.password != next.password);
  auto other = d;
  other.secret[0] ^= 1;
  CHECK(known::presence_keys(other, now, me).public_key != a.public_key);
  // Four different parts, none of them the secret.
  CHECK(a.box != a.salt_base);
  CHECK(a.password != a.box);
  CHECK(std::memcmp(a.password.data(), d.secret.data(), 32) != 0);
}

TEST_CASE("sharepoint known: a protocol 2 session names each side's key and gives both one pairing secret",
          "[sharepoint]") {
  std::array<std::uint8_t, 32> ka{}, kb{}, pa{}, pb{};
  randombytes_buf(ka.data(), ka.size());
  randombytes_buf(kb.data(), kb.size());
  crypto_scalarmult_base(pa.data(), ka.data());
  crypto_scalarmult_base(pb.data(), kb.data());
  auto open_with = [&](std::uint32_t version = sp::kProtocolVersion) {
    auto [a, b] = loopback_pair();
    mv::result<std::unique_ptr<sp::channel>> server = mv::err(mv::status::internal);
    sp::channel::options ob;
    ob.static_key = kb;
    std::thread t([&, link = std::move(b)]() mutable {
      server = sp::channel::open(std::move(link), false, "bbbbbbbbbbbbbbbb", "Bea", ob);
    });
    sp::channel::options oa;
    oa.static_key = ka;
    oa.version = version;
    auto client = sp::channel::open(std::move(a), true, "aaaaaaaaaaaaaaaa", "Ann", oa);
    t.join();
    REQUIRE(client);
    REQUIRE(server);
    return std::make_pair(std::move(*client), std::move(*server));
  };
  auto [ann, bea] = open_with();
  REQUIRE(ann->peer_key());
  REQUIRE(bea->peer_key());
  CHECK(*ann->peer_key() == pb);
  CHECK(*bea->peer_key() == pa);
  CHECK(ann->pairing_secret() == bea->pairing_secret());
  // Another session of the same two keys: another secret.
  auto [ann2, bea2] = open_with();
  CHECK(ann2->pairing_secret() == bea2->pairing_secret());
  CHECK(ann2->pairing_secret() != ann->pairing_secret());
  // Protocol 1 names no key.
  auto [p1a, p1b] = open_with(sp::kProtocol1);
  CHECK_FALSE(p1a->peer_key());
  CHECK_FALSE(p1b->peer_key());
}
