// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Driftwire's known devices (issue #472, PR 4): the secret store's file
// form, the identity kept in it, the remembered devices' records, the
// hourly presence keys, and what a protocol 2 session gives Remember. Of the
// OS stores, Credential Manager (on Windows) and the Secret Service (on Linux)
// are touched, under a name of the run's own; the Keychain is not.
#include "driftwire_fixture.h"

#include <sodium.h>

#include <cstdlib>
#include <string_view>

#include "addons/driftwire/known.h"
#include "addons/driftwire/secrets.h"

namespace known = mv::driftwire::known;
namespace secrets = mv::driftwire::secrets;

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

TEST_CASE("driftwire known: the file store keeps, replaces and forgets values by name", "[driftwire]") {
  scratch_dir s("driftwire_secret_store");
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

#if defined(_WIN32)
// The Windows half of the port: Credential Manager itself (secrets_win.cpp),
// under a name of this run's own that the test removes again. The Keychain
// is not touched on the Mac (a test there could stop at an access prompt).
TEST_CASE("driftwire known: Credential Manager keeps, replaces and forgets a value", "[driftwire]") {
  auto store = secrets::os_store();
  REQUIRE(store);
  std::uint8_t r[8];
  randombytes_buf(r, sizeof r);
  std::string name = "test/";
  for (const std::uint8_t b : r) name += "0123456789abcdef"[b & 15];
  CHECK_FALSE(store->get(name));
  REQUIRE(store->put(name, bytes_of("first")));
  CHECK(store->get(name) == bytes_of("first"));
  REQUIRE(store->put(name, bytes_of("second")));  // replaced
  CHECK(store->get(name) == bytes_of("second"));
  const std::vector<std::uint8_t> biggest(secrets::kMaxValue, 9);
  REQUIRE(store->put(name, biggest));
  CHECK(store->get(name) == biggest);
  CHECK_FALSE(store->put(name, std::vector<std::uint8_t>(secrets::kMaxValue + 1, 7)));
  CHECK(store->get(name) == biggest);  // a refused value leaves the old one
  CHECK(store->erase(name));
  CHECK_FALSE(store->get(name));
  CHECK_FALSE(store->erase(name));
}
#endif

#if defined(__linux__)
// The Linux half of the port: the Secret Service itself (secrets_linux.cpp,
// libsecret loaded at run time), under a service of this run's own whose
// items the test removes again. It needs a session bus, a keyring daemon on
// it and an unlocked default collection (a desktop login has all three);
// where one is missing it skips and says which. CI makes all three (a bus of
// the job's own, gnome-keyring's login keyring unlocked), checks them with
// secret-tool, and then sets MV_SECRET_SERVICE=required, so there a missing
// store fails instead of skipping.
TEST_CASE("driftwire known: the Secret Service keeps, replaces and forgets a value, apart per service",
          "[driftwire]") {
  const char* required_env = std::getenv("MV_SECRET_SERVICE");
  const bool required = required_env && std::string_view(required_env) == "required";
  std::uint8_t r[8];
  randombytes_buf(r, sizeof r);
  std::string service = "Driftwire test ";
  for (const std::uint8_t b : r) service += "0123456789abcdef"[b & 15];
  auto store = secrets::os_store(service);
  if (!store) {
    if (required) FAIL("MV_SECRET_SERVICE=required, but no Secret Service answered");
    SKIP("no Secret Service here (no session bus, or no keyring daemon on it)");
  }
  auto other = secrets::os_store(service + " other");
  REQUIRE(other);
  struct cleanup {
    secrets::store& a;
    secrets::store& b;
    ~cleanup() {
      for (const char* n : {"value", "planted", known::kIdentityName}) {
        a.erase(n);
        b.erase(n);
      }
    }
  } gone{*store, *other};
  const std::string name = "value";
  CHECK_FALSE(store->get(name));
  CHECK_FALSE(store->erase(name));
  if (!store->put(name, bytes_of("first"))) {
    // The service answers, but its default collection takes nothing without
    // an unlock prompt nobody can answer here (gnome-keyring with no login
    // keyring made, or one locked): in the product that is "not kept", and
    // the identity is that run's (known::identity reads back).
    if (required) FAIL("MV_SECRET_SERVICE=required, but the default collection took no item");
    SKIP("the Secret Service's default collection is locked or missing here (no login keyring unlocked)");
  }
  CHECK(store->get(name) == bytes_of("first"));
  REQUIRE(store->put(name, bytes_of("second")));  // replaced, not a second item
  CHECK(store->get(name) == bytes_of("second"));
  const std::vector<std::uint8_t> biggest(secrets::kMaxValue, 9);
  REQUIRE(store->put(name, biggest));
  CHECK(store->get(name) == biggest);
  CHECK_FALSE(store->put(name, std::vector<std::uint8_t>(secrets::kMaxValue + 1, 7)));
  CHECK(store->get(name) == biggest);  // a refused value leaves the old one
  // The app's and the command line's items never meet: another service has
  // nothing under the same name, and keeps its own.
  CHECK_FALSE(other->get(name));
  REQUIRE(other->put(name, bytes_of("other's")));
  CHECK(store->get(name) == biggest);
  CHECK(other->get(name) == bytes_of("other's"));
  // The identity is kept there, as in Credential Manager and the Keychain.
  bool kept = false;
  const auto id = known::identity(store.get(), kept);
  CHECK(kept);
  CHECK(known::identity(store.get(), kept) == id);
  CHECK(kept);
  // An item another program wrote in plain where Driftwire looks (the same
  // schema, service and name) is refused, never believed: values go in
  // sealed, as with DPAPI on Windows. secret-tool plants it, where installed.
  if (std::system("command -v secret-tool >/dev/null 2>&1") == 0) {
    const std::string plant = "printf planted | secret-tool store --label=planted xdg:schema "
                              "io.github.longtimeno_c.Driftwire service '" +
                              service + "' name planted >/dev/null 2>&1";
    REQUIRE(std::system(plant.c_str()) == 0);
    CHECK_FALSE(store->get("planted"));
    CHECK(store->erase("planted"));  // it was there, under our schema
  } else {
    WARN("secret-tool is not installed: the planted plain item was not tried");
  }
  CHECK(store->erase(name));
  CHECK_FALSE(store->get(name));
  CHECK_FALSE(store->erase(name));
  CHECK(other->get(name) == bytes_of("other's"));
}
#endif

TEST_CASE("driftwire known: the identity is made once and kept; without a store it is this run's", "[driftwire]") {
  scratch_dir s("driftwire_identity");
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

TEST_CASE("driftwire known: the remembered devices round-trip and a damaged record is refused", "[driftwire]") {
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

TEST_CASE("driftwire known: the list is kept in the store, and an empty one leaves nothing", "[driftwire]") {
  scratch_dir s("driftwire_known_list");
  auto store = secrets::file_store(utf8(s.root()));
  CHECK(known::load(*store).empty());
  REQUIRE(known::save(*store, {device("Ann", 3)}));
  const auto list = known::load(*store);
  REQUIRE(list.size() == 1);
  CHECK(list[0].name == "Ann");
  REQUIRE(known::save(*store, {}));
  CHECK_FALSE(store->get(known::kDevicesName));
}

TEST_CASE("driftwire known: Remove everything forgets the devices and the identity (#507)", "[driftwire]") {
  scratch_dir s("driftwire_known_forget");
  auto store = secrets::file_store(utf8(s.root()));
  bool kept = false;
  const auto before = known::identity(store.get(), kept);
  REQUIRE(kept);
  REQUIRE(known::save(*store, {device("Ann", 3), device("Bea", 5)}));
  CHECK(known::forget(*store));
  CHECK(known::load(*store).empty());
  CHECK_FALSE(store->get(known::kIdentityName));
  // Installed again: a new identity, and nobody remembered.
  CHECK(known::identity(store.get(), kept) != before);
  // Nothing to forget is fine.
  CHECK(known::forget(*store));
  CHECK(known::forget(*store));
}

TEST_CASE("driftwire known: each hour and each direction the pair gets new DHT keys, the same on both sides", "[driftwire]") {
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

TEST_CASE("driftwire known: a protocol 2 session names each side's key and gives both one pairing secret",
          "[driftwire]") {
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
