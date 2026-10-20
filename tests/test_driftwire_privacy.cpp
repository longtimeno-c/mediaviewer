// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Driftwire's privacy fixes (issue #501): the long-term identity never goes
// out in a nearby handshake (only sealed, after the code and Accept), the
// session id changes while idle and the name stays off the air with the
// window closed, and the Mac's store never reads the login keychain.
#include "driftwire_fixture.h"

#include <set>

#include <sodium.h>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

#include "addons/driftwire/known.h"
#include "addons/driftwire/relay.h"
#include "addons/driftwire/secrets.h"

namespace known = mv::driftwire::known;
namespace secrets = mv::driftwire::secrets;

namespace {

// A MediaViewer whose identity is kept (a folder for the secret store).
struct kept_client {
  fake_host host;
  sp::engine eng;
  kept_client(bus& b, const fs::path& received, const std::string& name, const fs::path& store,
              int rotate_ms = sp::kIdRotateMs)
      : host(received), eng(host, options(b, name, store, rotate_ms)) {
    eng.screen_opened("");
  }
  void type_code(const std::string& code) { eng.on_event("out_code_entry", 3, "\"" + code + "\""); }
  static sp::engine::options options(bus& b, const std::string& name, const fs::path& store, int rotate_ms) {
    sp::engine::options o = client::options(b, name);
    fs::create_directories(store);
    o.make_secrets = [dir = utf8(store)] { return secrets::file_store(dir); };
    o.id_rotate_ms = rotate_ms;
    return o;
  }
};

// The public half of the identity an engine keeps in `store`.
std::array<std::uint8_t, 32> identity_of(const fs::path& store) {
  auto st = secrets::file_store(utf8(store));
  bool kept = false;
  auto sk = known::identity(st.get(), kept);
  REQUIRE(kept);
  std::array<std::uint8_t, 32> pk{};
  crypto_scalarmult_base(pk.data(), sk.data());
  sodium_memzero(sk.data(), sk.size());
  return pk;
}

// What the bus hears from the member announcing `id` now.
std::optional<sp::beacon> announced(bus& b, const std::string& id) {
  std::lock_guard lock(b.m);
  for (const auto& [k, m] : b.members) {
    if (m.self.id == id) return m.self;
  }
  return std::nullopt;
}

bool sees_id(const sp::engine& e, const std::string& id) {
  const auto v = e.snapshot();
  return std::any_of(v.peers.begin(), v.peers.end(), [&](const auto& p) { return p.id == id; });
}

}  // namespace

TEST_CASE("driftwire privacy: a nearby connector never sees the receiver's identity key (#501)", "[driftwire]") {
  scratch_dir s("driftwire_privacy_responder");
  bus b;
  kept_client bea(b, s / "bea_received", "Bea", s / "bea_secrets");
  const auto identity = identity_of(s / "bea_secrets");
  std::optional<sp::beacon> self;
  REQUIRE(eventually([&] { return (self = announced(b, bea.eng.id())).has_value(); }));

  // Anyone in range: connect, read message 2 (the handshake's static is in
  // it), go. Twice: each link has a key of its own.
  std::vector<std::array<std::uint8_t, 32>> seen;
  for (int i = 0; i < 2; ++i) {
    auto l = sp::net::connect({"127.0.0.1", self->port}, 5000);
    REQUIRE(l);
    auto ch = sp::channel::open(std::move(*l), true, "0123456789abcdef", "Mallory");
    REQUIRE(ch);
    REQUIRE((*ch)->peer_key());
    seen.push_back(*(*ch)->peer_key());
    (*ch)->shutdown();
    bea.eng.wait_idle();
  }
  CHECK(seen[0] != identity);
  CHECK(seen[1] != identity);
  CHECK(seen[0] != seen[1]);
}

TEST_CASE("driftwire privacy: a sender's identity goes only in the offer, after the right code (#501)",
          "[driftwire]") {
  scratch_dir s("driftwire_privacy_initiator");
  bus b;
  kept_client ann(b, s / "ann_received", "Ann", s / "ann_secrets");
  const auto identity = identity_of(s / "ann_secrets");

  // A tile that is not a MediaViewer of anyone's: a listener and a beacon.
  auto listening = sp::net::listen(true);
  REQUIRE(listening);
  const std::string fake_id = "0123456789abcdef";
  bus_discovery fake(b);
  REQUIRE(fake.start(sp::beacon{fake_id, "Bea", (*listening)->port(), sp::kProtocolVersion},
                     [](const sp::beacon&, const std::string&) {}, {}));

  std::optional<std::array<std::uint8_t, 32>> handshake_key;
  std::string code;
  bool offer_early = true;
  std::optional<sp::incoming_offer> offer;
  std::atomic<bool> opened{false}, code_typed{false};
  std::thread server([&] {
    auto a = (*listening)->accept(15000);
    if (!a) return;
    std::shared_ptr<sp::link> l(std::move(*a));
    auto h = sp::channel::read_hello(*l, std::chrono::steady_clock::now() + std::chrono::seconds(8));
    if (!h) return;
    auto ch = sp::channel::open_server(l, std::move(*h), fake_id, "Bea");
    if (!ch) return;
    handshake_key = (*ch)->peer_key();
    code = (*ch)->code();
    // Nothing comes before the code is typed over there.
    sp::message m;
    offer_early = static_cast<bool>((*ch)->receive(m, 600));
    opened.store(true);
    while (!code_typed.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (auto o = sp::read_offer(**ch, 15000)) offer = std::move(*o);
    (void)sp::send_decline(**ch, sp::outcome::declined);
    (*ch)->flush(2000);
    (*ch)->shutdown();
  });

  write_text(s / "out" / "a.jpg", "abc");
  std::vector<std::string> paths{utf8(s / "out" / "a.jpg")};
  ann.eng.set_files(paths);
  REQUIRE(eventually([&] { return ann.eng.snapshot().file_count == 1; }));
  REQUIRE(eventually([&] { return sees_id(ann.eng, fake_id); }));
  ann.eng.on_event("peers", 5, "\"" + fake_id + "\"");
  const bool got_open = eventually([&] { return opened.load(); });
  if (got_open) ann.type_code(code);
  code_typed.store(true);
  server.join();
  ann.eng.wait_idle();
  fake.stop();
  REQUIRE(got_open);

  REQUIRE(handshake_key);
  CHECK(*handshake_key != identity);  // the link's own key in message 3
  CHECK_FALSE(offer_early);
  REQUIRE(offer);
  REQUIRE(offer->key);
  CHECK(*offer->key == identity);  // said sealed, once the code was right
}

TEST_CASE("driftwire privacy: after a nearby send each side remembers the other's identity (#501)",
          "[driftwire]") {
  scratch_dir s("driftwire_privacy_remember");
  bus b;
  kept_client ann(b, s / "ann_received", "Ann", s / "ann_secrets");
  kept_client bea(b, s / "bea_received", "Bea", s / "bea_secrets");
  write_text(s / "out" / "a.jpg", "abc");
  std::vector<std::string> paths{utf8(s / "out" / "a.jpg")};
  ann.eng.set_files(paths);
  REQUIRE(eventually([&] { return ann.eng.snapshot().file_count == 1; }));
  REQUIRE(eventually([&] { return sees_id(ann.eng, bea.eng.id()); }));
  ann.eng.on_event("peers", 5, "\"" + bea.eng.id() + "\"");
  std::string code;
  REQUIRE(eventually([&] {
    const auto v = bea.eng.snapshot();
    code = v.in_code;
    return v.in == sp::view::in_phase::connecting && !code.empty();
  }));
  ann.type_code(code);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  bea.eng.on_event("accept", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  REQUIRE(ann.eng.snapshot().out == sp::view::out_phase::done);
  REQUIRE(ann.eng.snapshot().remember_offer);
  REQUIRE(bea.eng.snapshot().remember_offer);
  ann.eng.on_event("remember", 2, "true");
  bea.eng.on_event("remember", 2, "true");
  // The keys kept are the identities (which later meetings check), not the
  // keys of the link that carried the files.
  CHECK(ann.eng.snapshot().known_ids == std::vector<std::string>{sp::relay::fingerprint(identity_of(s / "bea_secrets"))});
  CHECK(bea.eng.snapshot().known_ids == std::vector<std::string>{sp::relay::fingerprint(identity_of(s / "ann_secrets"))});
}

TEST_CASE("driftwire privacy: the id changes while idle and the name is off the air with the window closed (#501)",
          "[driftwire]") {
  scratch_dir s("driftwire_privacy_rotate");
  bus b;
  kept_client bea(b, s / "bea_received", "Bea", s / "bea_secrets", 200);
  bea.eng.on_event("receiving", 2, "true");
  std::optional<sp::beacon> now;
  const auto current = [&] {
    now = announced(b, bea.eng.id());
    return now.has_value();
  };

  // Window open: the name, and an id that holds (a tile does not change
  // under the pointer).
  REQUIRE(eventually(current));
  CHECK(now->name == "Bea");
  const std::string open_id = now->id;
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  REQUIRE(current());
  CHECK(now->id == open_id);

  // Closed: no name, a new id at once, and new ones while nothing happens.
  bea.eng.screen_closed();
  REQUIRE(eventually([&] { return current() && now->id != open_id; }));
  CHECK(now->name.empty());
  const std::string wire = sp::encode_beacon(*now);
  CHECK(wire.find("Bea") == std::string::npos);
  CHECK(wire.find("\"name\":\"MediaViewer\"") != std::string::npos);  // a v0.1.35 peer still reads it
  std::set<std::string> closed_ids;
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
  while (std::chrono::steady_clock::now() < until) {
    if (current()) {
      CHECK(now->name.empty());
      closed_ids.insert(now->id);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(closed_ids.size() >= 3);
  CHECK(closed_ids.count(open_id) == 0);

  // Open again: the name, under an id it never had closed.
  bea.eng.screen_opened("");
  REQUIRE(eventually([&] { return current() && now->name == "Bea"; }));
  CHECK(closed_ids.count(now->id) == 0);
  CHECK(now->id != open_id);
}

#if defined(__APPLE__)
// The Mac store (#501): only the data protection keychain, in the app's own
// access group. This test binary is signed with no entitlement, so there is
// no store at all, and an item planted in the login keychain under the
// service is removed, never read. A service name of the run's own; nothing
// of the real one is touched, and nothing here can prompt.
TEST_CASE("driftwire privacy: the Mac store never reads a planted login-keychain item (#501)", "[driftwire]") {
  std::uint8_t r[8];
  randombytes_buf(r, sizeof r);
  std::string service = "MediaViewer Driftwire test ";
  for (const std::uint8_t x : r) service += "0123456789abcdef"[x & 15];
  const auto query = [&](bool with_data) {
    CFMutableDictionaryRef q =
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef svc = CFStringCreateWithCString(nullptr, service.c_str(), kCFStringEncodingUTF8);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, svc);
    CFDictionarySetValue(q, kSecAttrAccount, CFSTR("identity"));
    CFDictionarySetValue(q, kSecUseDataProtectionKeychain, kCFBooleanFalse);
    if (with_data) {
      const std::uint8_t planted[32] = {1, 2, 3};
      CFDataRef d = CFDataCreate(nullptr, planted, sizeof planted);
      CFDictionarySetValue(q, kSecValueData, d);
      CFRelease(d);
    }
    CFRelease(svc);
    return q;
  };
  CFMutableDictionaryRef plant = query(true);
  const bool planted = SecItemAdd(plant, nullptr) == errSecSuccess;  // no login keychain (CI): nothing to plant
  CFRelease(plant);

  CHECK_FALSE(secrets::keychain_store(service));

  if (planted) {
    CFMutableDictionaryRef look = query(false);
    const OSStatus still = SecItemCopyMatching(look, nullptr);
    CHECK(still == errSecItemNotFound);
    if (still == errSecSuccess) (void)SecItemDelete(look);  // never leave it behind
    CFRelease(look);
  }
}
#endif
