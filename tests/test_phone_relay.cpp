// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The NAS service waiting at the person's own relay (docs/plans/nas-relay.md):
// a paired phone reaches it with no forwarded port, end to end sealed, and the
// service's admission applies as it does to a direct connection. Loopback only:
// the relay is wire/splice.h's server on 127.0.0.1, never anything public. Filter
// with "[phone-relay]".
#include <sodium.h>

#include "driftwire_fixture.h"

#include "wire/known.h"
#include "wire/serve.h"
#include "wire/serve_client.h"
#include "wire/splice.h"

namespace serve = mv::driftwire::serve;
namespace known = mv::driftwire::known;
namespace relay = mv::driftwire::relay;
namespace fs = std::filesystem;
using key32 = std::array<std::uint8_t, 32>;

namespace {

constexpr const char* kRelayKey = "loopback-relay-key-0123456789";

key32 random_key() {
  key32 k;
  randombytes_buf(k.data(), k.size());
  return k;
}

key32 public_of(const key32& priv) {
  key32 pub;
  crypto_scalarmult_base(pub.data(), priv.data());
  return pub;
}

// What the relay sees of the service: every byte both ways on its link to it.
struct tap {
  std::mutex m;
  std::vector<std::uint8_t> bytes;
};

class tapped_link final : public sp::link {
 public:
  tapped_link(std::unique_ptr<sp::link> inner, std::shared_ptr<tap> t) : inner_(std::move(inner)), t_(std::move(t)) {}
  mv::expected write_all(std::span<const std::uint8_t> b) override {
    {
      std::lock_guard lock(t_->m);
      t_->bytes.insert(t_->bytes.end(), b.begin(), b.end());
    }
    return inner_->write_all(b);
  }
  mv::result<std::size_t> read_some(std::span<std::uint8_t> into, int timeout_ms) override {
    auto r = inner_->read_some(into, timeout_ms);
    if (r && *r > 0) {
      std::lock_guard lock(t_->m);
      t_->bytes.insert(t_->bytes.end(), into.begin(), into.begin() + static_cast<std::ptrdiff_t>(*r));
    }
    return r;
  }
  void shutdown() noexcept override { inner_->shutdown(); }
  sp::transport via() const noexcept override { return inner_->via(); }

 private:
  std::unique_ptr<sp::link> inner_;
  std::shared_ptr<tap> t_;
};

struct relayed_nas {
  scratch_dir dir{"phone_relay"};
  fs::path library = dir.root() / "library";
  std::unique_ptr<sp::secrets::store> store;
  key32 identity{}, access{};
  key32 phone = random_key();
  std::unique_ptr<relay::server> rendezvous;
  std::unique_ptr<serve::server> srv;
  std::shared_ptr<tap> seen = std::make_shared<tap>();

  explicit relayed_nas(const std::string& stored_key = kRelayKey) {
    fs::create_directories(library);
    REQUIRE(sp::secrets::owner_only_folder(utf8(dir.root() / "secrets")));
    store = sp::secrets::file_store(utf8(dir.root() / "secrets"));
    bool made = false;
    REQUIRE(serve::set_up(*store, identity, access, made));
    known::device d;
    d.name = "Phone";
    d.key = public_of(phone);
    REQUIRE(known::save(*store, {d}));
    relay::server::options r;
    r.key = kRelayKey;
    r.loopback_only = true;
    auto started = relay::server::start(r);
    REQUIRE(started);
    rendezvous = std::move(*started);
    const std::string key = stored_key;
    REQUIRE(store->put(serve::kRelayKeyName, std::vector<std::uint8_t>(key.begin(), key.end())));
    serve::server::options o;
    o.cfg.port = 0;
    o.cfg.name = "Test NAS";
    o.cfg.roots = {{"a1a1a1a1a1a1a1a1", "Photos", utf8(library)}};
    o.cfg.relay = "127.0.0.1:" + std::to_string(rendezvous->port());
    o.store = store.get();
    o.loopback_only = true;
    o.per_minute = 100000;
    o.claimable = true;
    o.own_network = [](const sp::net::address&) { return true; };  // everything local but the relay's
    o.relay_connect = [t = seen](const std::string& host, std::uint16_t port, int ms) -> mv::result<std::unique_ptr<sp::link>> {
      auto l = sp::net::connect({host, port}, ms);
      if (!l) return mv::err(l.error());
      return std::unique_ptr<sp::link>(std::make_unique<tapped_link>(std::move(*l), t));
    };
    auto s = serve::server::start(o);
    REQUIRE(s);
    srv = std::move(*s);
  }

  // A phone through the relay: its token from the access key and its own key.
  mv::result<std::unique_ptr<serve::client>> through_relay(const key32& device, const key32& access_key,
                                                           serve::refusal* why = nullptr, bool upgrade = false) {
    auto l = sp::net::connect({"127.0.0.1", rendezvous->port()}, 3000);
    REQUIRE(l);
    const auto t = serve::relay_token(access_key, public_of(device));
    relay::token tok{};
    std::memcpy(tok.data(), t.data(), tok.size());
    auto joined = relay::join(std::move(*l), tok, 5000, [] { return false; });
    if (!joined) return mv::err(joined.error());
    serve::client::options o;
    o.access_key = access_key;
    o.device_key = device;
    o.service_key = identity;
    o.id = "cccccccccccccccc";
    o.name = "phone";
    if (upgrade) o.ops |= serve::kUpgradeOp;
    return serve::client::open(std::shared_ptr<sp::link>(std::move(*joined)), o, why);
  }
};

}  // namespace

TEST_CASE("phone relay: a paired phone reaches the service through the person's own relay", "[phone-relay]") {
  relayed_nas n;
  std::string secret(4096, '\0');
  randombytes_buf(secret.data(), secret.size());
  {
    std::ofstream f(n.library / "clip.mov", std::ios::binary);
    f.write(secret.data(), static_cast<std::streamsize>(secret.size()));
  }
  std::unique_ptr<serve::client> c;
  // The waiter registers a moment after the service starts.
  REQUIRE(eventually([&] {
    auto got = n.through_relay(n.phone, n.access);
    if (!got) return false;
    c = std::move(*got);
    return true;
  }));
  auto roots = c->roots();
  REQUIRE(roots);
  REQUIRE(roots->size() == 1);
  auto s = c->read_range("a1a1a1a1a1a1a1a1", "clip.mov", 0, UINT64_MAX);
  REQUIRE(s);
  std::string got;
  for (;;) {
    auto chunk = (*s)->next(8000);
    REQUIRE(chunk);
    if (!*chunk) break;
    got.append(reinterpret_cast<const char*>((*chunk)->bytes.data()), (*chunk)->bytes.size());
  }
  CHECK(got == secret);
  c->close();
  CHECK(n.srv->stats().relayed >= 1);
  // The relay carried ciphertext only: no 32-byte run of the file passed it in the clear.
  std::lock_guard lock(n.seen->m);
  const std::string_view wire(reinterpret_cast<const char*>(n.seen->bytes.data()), n.seen->bytes.size());
  CHECK(wire.size() > secret.size());
  for (std::size_t at = 0; at + 32 <= secret.size(); at += 32) {
    CHECK(wire.find(std::string_view(secret).substr(at, 32)) == std::string_view::npos);
  }
}

TEST_CASE("phone relay: no access key, or a revoked phone, gets nothing through the relay", "[phone-relay]") {
  relayed_nas n;
  // The waiter is there (a paired phone gets in) ...
  REQUIRE(eventually([&] { return n.through_relay(n.phone, n.access).has_value(); }));
  // ... a wrong access key names no waiter, so the relay pairs it with nobody.
  CHECK_FALSE(n.through_relay(n.phone, random_key()));
  // Revoked: its waiter goes, and nothing waits under its token any more.
  REQUIRE(known::save(*n.store, {}));
  REQUIRE(eventually([&] { return !n.through_relay(n.phone, n.access).has_value(); }, 20));
  CHECK(n.srv->stats().sessions >= 1);
}

TEST_CASE("phone relay: a claim never comes in through the relay", "[phone-relay]") {
  relayed_nas n;
  REQUIRE(eventually([&] { return n.through_relay(n.phone, n.access).has_value(); }));
  // Someone who can write the share, but is not on the NAS's network: the
  // token is there, and the relay is the only way in.
  const std::string token(64, 'a');
  fs::create_directories(n.library / ".mediaviewer");
  { std::ofstream(n.library / serve::kClaimTokenFile) << token; }
  auto l = sp::net::connect({"127.0.0.1", n.rendezvous->port()}, 3000);
  REQUIRE(l);
  const auto t = serve::relay_token(n.access, public_of(n.phone));  // the only waiter there is
  relay::token tok{};
  std::memcpy(tok.data(), t.data(), tok.size());
  auto joined = relay::join(std::move(*l), tok, 5000, [] { return false; });
  REQUIRE(joined);
  auto claimed = serve::claim(std::shared_ptr<sp::link>(std::move(*joined)), serve::claim_key(token), random_key(), "Intruder",
                              [](std::string_view) { return true; }, std::chrono::steady_clock::now() + std::chrono::seconds(5));
  CHECK_FALSE(claimed);
  CHECK_FALSE(n.store->exists(serve::kOwnerName));
}

TEST_CASE("phone relay: a changed relay key reaches the waiters without a restart", "[phone-relay]") {
  relayed_nas n("the-wrong-relay-key-0000000000");  // mistyped: no waiter registers
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  CHECK_FALSE(n.through_relay(n.phone, n.access));
  const std::string right = kRelayKey;
  REQUIRE(n.store->put(serve::kRelayKeyName, std::vector<std::uint8_t>(right.begin(), right.end())));
  // The relay thread reads the key again within its poll and starts fresh waiters with it.
  CHECK(eventually([&] { return n.through_relay(n.phone, n.access).has_value(); }, 20));
}

TEST_CASE("phone relay: only the device a waiter is for gets through it", "[phone-relay]") {
  relayed_nas n;
  key32 other = random_key();
  known::device a, b;
  a.name = "Phone";
  a.key = public_of(n.phone);
  b.name = "Tablet";
  b.key = public_of(other);
  REQUIRE(known::save(*n.store, {a, b}));
  REQUIRE(eventually([&] { return n.through_relay(other, n.access).has_value(); }, 20));
  // The phone, holding the access key, takes the tablet's waiter: refused after the handshake.
  auto l = sp::net::connect({"127.0.0.1", n.rendezvous->port()}, 3000);
  REQUIRE(l);
  const auto t = serve::relay_token(n.access, public_of(other));
  relay::token tok{};
  std::memcpy(tok.data(), t.data(), tok.size());
  auto joined = relay::join(std::move(*l), tok, 5000, [] { return false; });
  REQUIRE(joined);
  serve::client::options o;
  o.access_key = n.access;
  o.device_key = n.phone;
  o.service_key = n.identity;
  o.id = "cccccccccccccccc";
  o.name = "phone";
  CHECK_FALSE(serve::client::open(std::shared_ptr<sp::link>(std::move(*joined)), o));
}

TEST_CASE("phone relay: the relay's address is host:port and nothing else", "[phone-relay]") {
  std::string h;
  std::uint16_t p = 0;
  CHECK(serve::parse_relay_address("relay.example.com:7879", h, p));
  CHECK(h == "relay.example.com");
  CHECK(p == 7879);
  CHECK(serve::parse_relay_address("[::1]:9", h, p));
  CHECK(h == "::1");
  CHECK(serve::parse_relay_address("203.0.113.5:65535", h, p));
  for (const char* bad : {"", "relay.example.com", "relay.example.com:", "relay.example.com:0", ":7879",
                          "relay.example.com:65536", "relay.example.com:79x", "::1:9", "[]:9", "relay example.com:7879",
                          "relay.example.com:7879\n", "relay.example.com:123456"}) {
    CHECK_FALSE(serve::parse_relay_address(bad, h, p));
  }
}

TEST_CASE("phone relay: a relay that never answers does not hold up a stop", "[phone-relay]") {
  scratch_dir dir{"phone_relay_silent"};
  const fs::path library = dir.root() / "library";
  fs::create_directories(library);
  REQUIRE(sp::secrets::owner_only_folder(utf8(dir.root() / "secrets")));
  auto store = sp::secrets::file_store(utf8(dir.root() / "secrets"));
  key32 identity{}, access{};
  bool made = false;
  REQUIRE(serve::set_up(*store, identity, access, made));
  known::device d;
  d.name = "Phone";
  d.key = public_of(random_key());
  REQUIRE(known::save(*store, {d}));
  const std::string key = kRelayKey;
  REQUIRE(store->put(serve::kRelayKeyName, std::vector<std::uint8_t>(key.begin(), key.end())));
  // Accepts and never says a word: the waiter sits in its handshake.
  auto silent = sp::net::listen(true);
  REQUIRE(silent);
  std::vector<std::unique_ptr<sp::link>> held;
  std::atomic<bool> quit{false};
  std::thread acceptor([&] {
    while (!quit.load()) {
      if (auto c = (*silent)->accept(100)) held.push_back(std::move(*c));
    }
  });
  serve::server::options o;
  o.cfg.port = 0;
  o.cfg.name = "Test NAS";
  o.cfg.roots = {{"a1a1a1a1a1a1a1a1", "Photos", utf8(library)}};
  o.cfg.relay = "127.0.0.1:" + std::to_string((*silent)->port());
  o.store = store.get();
  o.loopback_only = true;
  auto s = serve::server::start(o);
  REQUIRE(s);
  std::this_thread::sleep_for(std::chrono::milliseconds(800));  // registering, and stuck there
  const auto t0 = std::chrono::steady_clock::now();
  (*s)->stop();
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));
  quit.store(true);
  acceptor.join();
}

TEST_CASE("phone relay: a direct path is asked only toward somewhere else, and not again at once", "[phone-relay]") {
  using C = serve::candidate;
  // Out on the internet: never this machine, a broadcast, a multicast or a link-local address.
  for (const C& bad : {C{"0.0.0.0", 80}, C{"127.0.0.1", 80}, C{"224.0.0.1", 80}, C{"255.255.255.255", 80},
                       C{"169.254.1.1", 80}, C{"::", 80}, C{"::1", 80}, C{"ff02::1", 80}, C{"fe80::1", 80}, C{"203.0.113.5", 0}}) {
    CHECK_FALSE(serve::usable_candidate(bad, false));
  }
  for (const C& good : {C{"203.0.113.5", 4000}, C{"192.168.1.20", 4000}, C{"2001:db8::5", 4000}}) {
    CHECK(serve::usable_candidate(good, false));
  }
  // The tests' service punches on loopback alone.
  CHECK(serve::usable_candidate(C{"127.0.0.1", 4000}, true));
  CHECK_FALSE(serve::usable_candidate(C{"203.0.113.5", 4000}, true));

  relayed_nas n;
  std::unique_ptr<serve::client> c;
  REQUIRE(eventually([&] {
    auto got = n.through_relay(n.phone, n.access, nullptr, true);
    if (!got) return false;
    c = std::move(*got);
    return true;
  }));
  REQUIRE((c->caps().ops & serve::kUpgradeOp) != 0);  // a relayed session is offered it
  serve::end_code code{};
  // Nothing usable: refused before any port opens.
  CHECK_FALSE(c->admin(serve::candidates_json({{"224.0.0.1", 80}, {"0.0.0.0", 80}}, "upgrade"), &code));
  CHECK(code == serve::end_code::bad_request);
  // A usable one: the service answers with its own (and punches toward nobody here).
  auto answer = c->admin(serve::candidates_json({{"127.0.0.1", 9}}, "upgrade"), &code);
  REQUIRE(answer);
  CHECK_FALSE(serve::parse_candidates(*answer).empty());
  // Asked again at once: the device's cooldown.
  CHECK_FALSE(c->admin(serve::candidates_json({{"127.0.0.1", 9}}, "upgrade"), &code));
  CHECK(code == serve::end_code::too_many);
  c->close();
  // A direct connection is never offered it.
  CHECK(n.srv->stats().punched == 0);
}
