// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/mobile.md §7.2, §7.4, §7.5 over loopback: claiming the service,
// single-use pairing codes, revoking on both sides, the nearby service.
// Filter with "[phone-pairing]". Nothing here goes beyond ::1.
#include <sodium.h>

#include "driftwire_fixture.h"

#include <ctime>

#include "core/json.h"

#include "wire/advert.h"
#include "catalogue/catalog_chain.h"
#include "catalogue/publisher.h"
#include "core/json.h"
#include "phone/desk.h"
#include "phone/link.h"
#include "wire/grants.h"
#include "wire/known.h"
#include "wire/pair_check.h"
#include "wire/serve.h"
#include "wire/serve_client.h"

#if defined(__APPLE__)
#include <dns_sd.h>
#include <poll.h>
#endif

namespace serve = mv::driftwire::serve;
namespace known = mv::driftwire::known;
namespace desk = mv::driftwire::desk;
namespace fs = std::filesystem;
using key32 = std::array<std::uint8_t, 32>;

namespace {

key32 random_key() {
  key32 k;
  randombytes_buf(k.data(), k.size());
  return k;
}

std::chrono::steady_clock::time_point soon() { return std::chrono::steady_clock::now() + std::chrono::seconds(25); }

key32 public_of(const key32& priv) {
  key32 pub;
  crypto_scalarmult_base(pub.data(), priv.data());
  return pub;
}

void put_file(const fs::path& p, std::string_view text) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// A folder of files, as `driftwire serve` keeps; a secrets::store for the desktop too.
std::unique_ptr<sp::secrets::store> folder_store(const fs::path& p) {
  REQUIRE(sp::secrets::owner_only_folder(utf8(p)));
  auto s = sp::secrets::file_store(utf8(p));
  REQUIRE(s);
  return s;
}

struct nas {
  scratch_dir dir{"phone_pairing"};
  fs::path share = dir.root() / "share";
  std::unique_ptr<sp::secrets::store> store = folder_store(dir.root() / "nas-secrets");
  key32 identity{}, access{};
  std::atomic<bool> own{true};
  std::atomic<std::int64_t> fake_now{0};  // unix seconds the service sees (0: the clock)
  std::atomic<std::int64_t> steady_skew_s{0};  // added to the service's steady clock
  int answer_gap_ms = 0;
  std::vector<serve::config> saved;
  std::mutex saved_m;
  std::unique_ptr<serve::server> srv;

  nas() {
    fs::create_directories(share);
    bool made = false;
    REQUIRE(serve::set_up(*store, identity, access, made));
    start();
  }
  void start() {
    serve::server::options o;
    o.cfg.port = 0;
    o.cfg.name = "Test NAS";
    o.cfg.roots = {{"a1a1a1a1a1a1a1a1", "Photos", utf8(share)}};
    o.store = store.get();
    o.loopback_only = true;
    o.per_minute = 100000;
    o.claimable = true;
    o.own_network = [this](const sp::net::address&) { return own.load(); };
    o.now_unix = [this] {
      const auto t = fake_now.load();
      return t != 0 ? t : static_cast<std::int64_t>(std::time(nullptr));
    };
    o.now_steady = [this] { return std::chrono::steady_clock::now() + std::chrono::seconds(steady_skew_s.load()); };
    o.min_answer_gap_ms = answer_gap_ms;
    o.save_config = [this](const serve::config& c) {
      std::lock_guard lock(saved_m);
      saved.push_back(c);
      return true;
    };
    auto s = serve::server::start(o);
    REQUIRE(s);
    srv = std::move(*s);
  }
  std::string local() const { return "[::1]:" + std::to_string(srv->port()); }
  std::shared_ptr<sp::link> connect() const {
    auto l = sp::net::connect({"::1", srv->port()}, 3000);
    REQUIRE(l);
    return std::shared_ptr<sp::link>(std::move(*l));
  }
  // A phone's connection: `code` redeemed if given.
  mv::result<std::unique_ptr<serve::client>> phone(const key32& device, const std::string& code = {},
                                                   serve::refusal* why = nullptr, const std::string& name = "iPhone") const {
    serve::client::options o;
    o.access_key = access;
    o.device_key = device;
    o.service_key = identity;
    o.id = "bbbbbbbbbbbbbbbb";
    o.name = "phone";
    o.pair_code = code;
    o.device_name = name;
    return serve::client::open(connect(), o, why);
  }
  bool has_device(const key32& device) const {
    const auto list = known::load(*store);
    const key32 pub = public_of(device);
    return std::any_of(list.begin(), list.end(), [&](const known::device& d) { return d.key == pub; });
  }
};

struct desktop {
  scratch_dir dir{"phone_desk"};
  std::unique_ptr<sp::secrets::store> store = folder_store(dir.root() / "secrets");
  std::unique_ptr<desk::desk> d;
  explicit desktop(std::string name = "Studio Mac") {
    desk::desk::options o;
    o.store = store.get();
    o.device_name = std::move(name);
    o.nearby_loopback = true;
    o.advertise = false;
    o.own_network = [](const sp::net::address&) { return true; };
    // The owner's passphrase at the cheapest cost, so the tests stay quick.
    o.pass_ops = o.floor_ops = mv::driftwire::serve::owner::kMinOps;
    o.pass_mem = o.floor_mem = mv::driftwire::serve::owner::kMinMem;
    d = std::make_unique<desk::desk>(std::move(o));
  }
};

std::string read_whole(serve::client& c, const std::string& root, const std::string& rel, std::uint64_t at = 0) {
  auto s = c.read_range(root, rel, at, UINT64_MAX);
  REQUIRE(s);
  std::string out;
  for (;;) {
    auto n = (*s)->next(8000);
    REQUIRE(n);
    if (!*n) return out;
    out.append(reinterpret_cast<const char*>((*n)->bytes.data()), (*n)->bytes.size());
  }
}

}  // namespace

// ---- claiming -------------------------------------------------------------------------------

TEST_CASE("phone pairing: a desktop that can write the share claims the service", "[phone-pairing]") {
  nas n;
  desktop mac;
  REQUIRE(mac.d->claim(utf8(n.share), n.local(), "nas.example.com:7878"));
  const auto s = mac.d->service();
  REQUIRE(s);
  CHECK(s->access_key == n.access);
  CHECK(s->identity == n.identity);
  CHECK(s->local == n.local());
  CHECK(s->public_name == "nas.example.com:7878");
  // The token is gone from the share.
  CHECK_FALSE(fs::exists(n.share / serve::kClaimTokenFile));
  // The catalogue became a named folder, written before it took effect.
  const auto roots = n.srv->roots();
  REQUIRE(roots.size() == 2);
  CHECK(roots[1].label == serve::kCatalogueLabel);
  // Compared as paths: the claim joins with "/", and on Windows fs::path takes either separator.
  const auto& claimed = roots[1].path;
  CHECK(fs::path(std::u8string(claimed.begin(), claimed.end())) == n.share / serve::kCatalogueFolder);
  std::lock_guard lock(n.saved_m);
  REQUIRE(n.saved.size() == 1);
  CHECK(n.saved[0].roots.size() == 2);
}

TEST_CASE("phone pairing: a service is claimed once", "[phone-pairing]") {
  nas n;
  desktop first, second("Other Mac");
  REQUIRE(first.d->claim(utf8(n.share), n.local(), ""));
  const auto owner = n.store->get(serve::kOwnerName);
  REQUIRE(owner);
  const auto again = second.d->claim(utf8(n.share), n.local(), "");
  REQUIRE_FALSE(again);
  CHECK(again.error() == mv::status::permission_denied);
  CHECK_FALSE(second.d->service());
  CHECK(n.store->get(serve::kOwnerName) == owner);
  CHECK_FALSE(fs::exists(n.share / serve::kClaimTokenFile));
  // And the first still administers it.
  CHECK(first.d->refresh_devices());
}

TEST_CASE("phone pairing: a claim from outside its own network gets nothing", "[phone-pairing]") {
  nas n;
  n.own = false;
  desktop mac;
  const auto r = mac.d->claim(utf8(n.share), n.local(), "");
  REQUIRE_FALSE(r);
  CHECK(r.error() == mv::status::permission_denied);
  CHECK_FALSE(n.store->exists(serve::kOwnerName));
  CHECK_FALSE(fs::exists(n.share / serve::kClaimTokenFile));
  CHECK(n.srv->stats().sessions == 0);
  // From inside, the same desktop can.
  n.own = true;
  CHECK(mac.d->claim(utf8(n.share), n.local(), ""));
}

TEST_CASE("phone pairing: a claim needs the token that is on the share", "[phone-pairing]") {
  nas n;
  const std::string token(64, 'c');
  // A claimant that cannot write the share: nothing is there for its key.
  // Writing the share's answer: what the desktop does with the service's nonce.
  const auto write_proof = [&](std::string_view nonce) {
    put_file(n.share / serve::kClaimProofFile, nonce);
    return true;
  };
  auto no_file = serve::claim(n.connect(), serve::claim_key(token), random_key(), "Thief", write_proof, soon());
  CHECK_FALSE(no_file);
  CHECK_FALSE(n.store->exists(serve::kOwnerName));
  // The right file, a wrong token.
  put_file(n.share / serve::kClaimTokenFile, token);
  auto wrong = serve::claim(n.connect(), serve::claim_key(std::string(64, 'd')), random_key(), "Thief", write_proof, soon());
  CHECK_FALSE(wrong);
  CHECK_FALSE(n.store->exists(serve::kOwnerName));
  // And what the file says, works.
  const key32 device = random_key();
  // Someone who can read the share but not write it: holds the token, cannot answer.
  auto reader = serve::claim(n.connect(), serve::claim_key(token), random_key(), "Reader",
                             [](std::string_view) { return true; }, soon());
  CHECK_FALSE(reader);
  CHECK_FALSE(n.store->exists(serve::kOwnerName));
  // An answer that is not the nonce asked for.
  auto stale = serve::claim(n.connect(), serve::claim_key(token), random_key(), "Reader", [&](std::string_view) {
    put_file(n.share / serve::kClaimProofFile, std::string(64, '0'));
    return true;
  }, soon());
  CHECK_FALSE(stale);
  CHECK_FALSE(n.store->exists(serve::kOwnerName));
  auto right = serve::claim(n.connect(), serve::claim_key(token), device, "Mac", write_proof, soon());
  REQUIRE(right);
  CHECK(right->access_key == n.access);
  CHECK(right->identity == n.identity);
  auto owner = n.store->get(serve::kOwnerName);
  REQUIRE(owner);
  const key32 pub = public_of(device);
  CHECK(std::equal(owner->begin(), owner->end(), pub.begin(), pub.end()));
}

TEST_CASE("phone pairing: a service the desktop never reaches is a plain failure", "[phone-pairing]") {
  scratch_dir share{"phone_share"};
  desktop mac;
  const auto r = mac.d->claim(utf8(share.root()), "127.0.0.1:1", "");
  REQUIRE_FALSE(r);
  CHECK(r.error() == mv::status::io);
  CHECK_FALSE(fs::exists(share.root() / serve::kClaimTokenFile));
}

// ---- the owner's passphrase (docs/plans/nas-owner-and-guests.md §3) ---------------------

namespace owner = mv::driftwire::serve::owner;

constexpr const char* kPass = "a long owner passphrase";

// What a desktop that can take the passphrase step hands claim(): the
// passphrase it was given, at the cheapest cost the tests allow.
serve::claim_passphrase with_passphrase(std::string pass, bool set_if_none = true) {
  serve::claim_passphrase p;
  p.sign = [pass](const owner::salt_t& salt, std::uint64_t ops, std::uint64_t mem) { return owner::derive(pass, salt, ops, mem); };
  p.min_ops = owner::kMinOps;
  p.min_mem = owner::kMinMem;
  if (set_if_none) p.set = owner::make(pass, 0, owner::kMinOps, owner::kMinMem);
  return p;
}

// `driftwire serve init` with a passphrase: the verifier in the NAS's store.
void init_passphrase(nas& n, const std::string& pass = kPass) {
  const auto v = owner::make(pass, 1, owner::kMinOps, owner::kMinMem);
  REQUIRE(v);
  REQUIRE(owner::save(*n.store, *v));
}

struct claimer {
  nas& n;
  std::string token = std::string(64, 'e');
  key32 device = random_key();
  explicit claimer(nas& in) : n(in) {}
  mv::result<serve::claim_answer> claim(const serve::claim_passphrase* pass, std::string* why = nullptr,
                                       const key32* as = nullptr) {
    put_file(n.share / serve::kClaimTokenFile, token);
    return serve::claim(n.connect(), serve::claim_key(token), as ? *as : device, "Mac",
                        [&](std::string_view nonce) {
                          put_file(n.share / serve::kClaimProofFile, nonce);
                          return true;
                        },
                        soon(), pass, why);
  }
  mv::result<std::unique_ptr<serve::client>> session() {
    serve::client::options o;
    o.access_key = n.access;
    o.device_key = device;
    o.service_key = n.identity;
    o.id = "aaaaaaaaaaaaaaaa";
    o.ops = serve::kAllOps | serve::kAdminOp;
    return serve::client::open(n.connect(), o);
  }
};

std::string why_in(const std::string& json) {
  const auto doc = mv::json::parse(json, 2);
  const std::string* w = doc ? doc->str("why") : nullptr;
  return w ? *w : std::string();
}

TEST_CASE("phone pairing: a passphrase set at init is needed to claim", "[phone-pairing]") {
  nas n;
  init_passphrase(n);
  claimer m(n);
  // A desktop from before the passphrase: it can write the share, and still gets nothing.
  std::string why;
  auto old = m.claim(nullptr, &why);
  CHECK_FALSE(old);
  CHECK(why == "passphrase");
  CHECK_FALSE(n.store->exists(serve::kOwnerName));
  // A wrong one.
  const auto wrong = with_passphrase("not the passphrase at all");
  auto bad = m.claim(&wrong, &why);
  CHECK_FALSE(bad);
  CHECK(why == "passphrase");
  CHECK_FALSE(n.store->exists(serve::kOwnerName));
  CHECK(n.srv->stats().wrong_passphrase == 1);
  // The right one.
  const auto right = with_passphrase(kPass);
  auto ok = m.claim(&right, &why);
  REQUIRE(ok);
  CHECK(ok->access_key == n.access);
  CHECK(n.store->exists(serve::kOwnerName));
  // init's passphrase is kept; the claim did not choose another.
  owner::verifier v;
  REQUIRE(owner::load(*n.store, v) == owner::stored::set);
  CHECK(v.set == 1);
  // The owner's session says what this service can do; the passphrase is set.
  auto s = m.session();
  REQUIRE(s);
  CHECK((*s)->caps().passphrase == "set");
  CHECK(std::find((*s)->caps().features.begin(), (*s)->caps().features.end(), serve::kFeaturePassphrase) !=
        (*s)->caps().features.end());
}

TEST_CASE("phone pairing: a claim with none set chooses it, and only a public key is kept", "[phone-pairing]") {
  nas n;
  claimer m(n);
  // A claim that can take the step but brings nothing to set fails, and claims nothing.
  const auto empty = with_passphrase(kPass, false);
  std::string why;
  CHECK_FALSE(m.claim(&empty, &why));
  CHECK(why == "no-passphrase");
  CHECK_FALSE(n.store->exists(serve::kOwnerName));

  const auto pass = with_passphrase(kPass);
  REQUIRE(m.claim(&pass, &why));
  owner::verifier v;
  REQUIRE(owner::load(*n.store, v) == owner::stored::set);
  CHECK(v.pk == pass.set->pk);
  // Nothing in the store is the passphrase or the key that answers for it.
  const auto signer = owner::derive(kPass, v.salt, v.ops, v.mem);
  REQUIRE(signer);
  for (const auto& e : fs::directory_iterator(n.dir.root() / "nas-secrets")) {
    std::ifstream f(e.path(), std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(bytes.find(kPass) == std::string::npos);
    CHECK(bytes.find(std::string(reinterpret_cast<const char*>(signer->sk.data()), 32)) == std::string::npos);
    CHECK(bytes.find(mv::driftwire::key_hex([&] {
      key32 seed;
      std::memcpy(seed.data(), signer->sk.data(), 32);
      return seed;
    }())) == std::string::npos);
  }
  // A claim again from the owner (its answer lost) needs it now, and is counted on the owner's counter.
  const auto wrong = with_passphrase("another long passphrase", false);
  CHECK_FALSE(m.claim(&wrong, &why));
  CHECK(why == "passphrase");
  const auto l = owner::load_lockout(*n.store, std::time(nullptr));
  CHECK(l.owner.misses == 1);
  CHECK(l.claim.misses == 0);
  auto again = m.claim(&pass, &why);
  REQUIRE(again);
  CHECK(again->passphrase_step);
  CHECK(owner::load_lockout(*n.store, std::time(nullptr)).owner.misses == 0);
}

TEST_CASE("phone pairing: an older desktop claims a NAS with none, and the owner sets one later", "[phone-pairing]") {
  nas n;
  claimer m(n);
  REQUIRE(m.claim(nullptr));
  owner::verifier v;
  CHECK(owner::load(*n.store, v) == owner::stored::none);
  auto s = m.session();
  REQUIRE(s);
  CHECK((*s)->caps().passphrase == "none");
  // With none set, the owner's key is enough for everything, as before (call C4).
  CHECK((*s)->admin("{\"do\":\"pair_code\",\"code\":\"" + std::string(32, 'a') + "\",\"ttl_ms\":60000}"));
  // The first passphrase needs only the device key.
  const auto made = owner::make(kPass, 0, owner::kMinOps, owner::kMinMem);
  REQUIRE(made);
  const auto set_request = [](const owner::verifier& x) {
    mv::json::writer w;
    w.begin_object().key("do").string("set_passphrase").key("set").begin_object();
    std::string salt(owner::kSaltBytes * 2, '0');
    sodium_bin2hex(salt.data(), salt.size() + 1, x.salt.data(), x.salt.size());
    w.key("salt").string(salt);
    w.key("ops").integer(static_cast<std::int64_t>(x.ops)).key("mem").integer(static_cast<std::int64_t>(x.mem));
    w.key("pk").string(mv::driftwire::key_hex(x.pk));
    w.end_object().end_object();
    return w.take();
  };
  REQUIRE((*s)->admin(set_request(*made)));
  REQUIRE(owner::load(*n.store, v) == owner::stored::set);
  // A second one needs it unlocked: a thief with the Mac cannot replace it.
  const auto other = owner::make("a thief's passphrase", 0, owner::kMinOps, owner::kMinMem);
  REQUIRE(other);
  std::string refusal;
  CHECK_FALSE((*s)->admin(set_request(*other), nullptr, &refusal));
  CHECK(why_in(refusal) == "needs-unlock");
  REQUIRE(owner::load(*n.store, v) == owner::stored::set);
  CHECK(v.pk == made->pk);
  // A claim from an older desktop is refused from now on.
  std::string why;
  CHECK_FALSE(m.claim(nullptr, &why));
  CHECK(why == "passphrase");
}

TEST_CASE("phone pairing: what adds access needs an unlock; what takes it away does not", "[phone-pairing]") {
  nas n;
  init_passphrase(n);
  claimer m(n);
  const auto pass = with_passphrase(kPass);
  REQUIRE(m.claim(&pass));
  // A phone, paired from the NAS's own command line.
  const key32 phone = random_key();
  REQUIRE(known::save(*n.store, {known::device{"iPhone", public_of(phone), {}, false}}));
  auto s = m.session();
  REQUIRE(s);
  const std::string code_request = "{\"do\":\"pair_code\",\"code\":\"" + std::string(32, 'b') + "\",\"ttl_ms\":60000}";
  std::string refusal;
  for (const std::string& r : {code_request, std::string("{\"do\":\"add_device\",\"name\":\"X\",\"key\":\"") +
                                                 mv::driftwire::key_hex(random_key()) + "\"}",
                               std::string("{\"do\":\"remove_root\",\"id\":\"a1a1a1a1a1a1a1a1\"}")}) {
    CHECK_FALSE((*s)->admin(r, nullptr, &refusal));
    CHECK(why_in(refusal) == "needs-unlock");
  }
  // Reading and taking away work on the device key alone.
  CHECK((*s)->admin("{\"do\":\"devices\"}"));
  CHECK((*s)->admin("{\"do\":\"pair_cancel\"}"));
  CHECK((*s)->admin("{\"do\":\"revoke\",\"key\":\"" + mv::driftwire::key_hex(public_of(phone)) + "\"}"));
  CHECK_FALSE(n.has_device(phone));

  // A wrong passphrase does not unlock.
  CHECK_FALSE((*s)->unlock([](const owner::salt_t& salt, std::uint64_t ops, std::uint64_t mem) {
    return owner::derive("the wrong passphrase", salt, ops, mem);
  }, &refusal, owner::kMinOps, owner::kMinMem));
  CHECK(why_in(refusal) == "passphrase");
  // An answer with no challenge before it is refused.
  CHECK_FALSE((*s)->admin("{\"do\":\"unlock\",\"sig\":\"" + std::string(128, '0') + "\"}", nullptr, &refusal));
  CHECK(why_in(refusal) == "no-challenge");
  // The right one does, for this session only.
  auto unlocked = (*s)->unlock(pass.sign, &refusal, owner::kMinOps, owner::kMinMem);
  REQUIRE(unlocked);
  CHECK(unlocked->find("\"unlocked_s\":600") != std::string::npos);
  CHECK((*s)->admin(code_request));
  auto other = m.session();
  REQUIRE(other);
  CHECK_FALSE((*other)->admin(code_request, nullptr, &refusal));
  CHECK(why_in(refusal) == "needs-unlock");
  // Lock ends it; and the ten minutes run out on the service's clock.
  REQUIRE((*s)->admin("{\"do\":\"lock\"}"));
  CHECK_FALSE((*s)->admin(code_request));
  REQUIRE((*s)->unlock(pass.sign, nullptr, owner::kMinOps, owner::kMinMem));
  CHECK((*s)->admin(code_request));
  n.steady_skew_s = owner::kUnlockSeconds + 1;
  CHECK_FALSE((*s)->admin(code_request, nullptr, &refusal));
  CHECK(why_in(refusal) == "needs-unlock");
  // A wall clock set back does not stretch it: the ten minutes are steady time.
  n.steady_skew_s = 0;
  REQUIRE((*s)->unlock(pass.sign, nullptr, owner::kMinOps, owner::kMinMem));
  n.fake_now = std::time(nullptr) - 3600;
  n.steady_skew_s = owner::kUnlockSeconds + 1;
  CHECK_FALSE((*s)->admin(code_request));
  // A passphrase changed on the NAS (the CLI) ends every unlock at once.
  n.fake_now = 0;
  n.steady_skew_s = 0;
  REQUIRE((*s)->unlock(pass.sign, nullptr, owner::kMinOps, owner::kMinMem));
  CHECK((*s)->admin(code_request));
  init_passphrase(n, "the replacement passphrase");
  CHECK_FALSE((*s)->admin(code_request, nullptr, &refusal));
  CHECK(why_in(refusal) == "needs-unlock");
  CHECK(n.srv->stats().unlocks >= 3);
}

TEST_CASE("phone pairing: wrong passphrases lock the owner out, across a restart, until reset", "[phone-pairing]") {
  nas n;
  init_passphrase(n);
  claimer m(n);
  const auto pass = with_passphrase(kPass);
  REQUIRE(m.claim(&pass));
  const std::int64_t t0 = 2'000'000'000;
  n.fake_now = t0;
  auto s = m.session();
  REQUIRE(s);
  const auto wrong = [](const owner::salt_t& salt, std::uint64_t ops, std::uint64_t mem) {
    return owner::derive("the wrong passphrase", salt, ops, mem);
  };
  std::string refusal;
  for (int i = 0; i < owner::kFreeMisses; ++i) {
    CHECK_FALSE((*s)->unlock(wrong, &refusal, owner::kMinOps, owner::kMinMem));
    CHECK(why_in(refusal) == "passphrase");
  }
  CHECK_FALSE((*s)->unlock(wrong, &refusal, owner::kMinOps, owner::kMinMem));
  CHECK(why_in(refusal) == "locked-out");
  // Locked: even the right one is not checked, and no challenge is given.
  CHECK_FALSE((*s)->unlock(pass.sign, &refusal, owner::kMinOps, owner::kMinMem));
  CHECK(why_in(refusal) == "locked-out");
  CHECK(refusal.find("\"until\":" + std::to_string(t0 + owner::kFirstLockSeconds)) != std::string::npos);
  // A restart does not forget it.
  n.srv->stop();
  n.srv.reset();
  n.start();
  auto again = m.session();
  REQUIRE(again);
  CHECK_FALSE((*again)->unlock(pass.sign, &refusal, owner::kMinOps, owner::kMinMem));
  CHECK(why_in(refusal) == "locked-out");
  // The owner's lock does not stop a claim being counted apart, and vice versa.
  CHECK(owner::load_lockout(*n.store, t0).claim.misses == 0);
  // Past the lock the right one works and clears the count.
  n.fake_now = t0 + owner::kFirstLockSeconds;
  REQUIRE((*again)->unlock(pass.sign, nullptr, owner::kMinOps, owner::kMinMem));
  CHECK(owner::load_lockout(*n.store, t0).owner.misses == 0);
  // Locked again, `driftwire serve passphrase reset` on the NAS clears it.
  for (int i = 0; i <= owner::kFreeMisses; ++i) (void)(*again)->unlock(wrong, nullptr, owner::kMinOps, owner::kMinMem);
  CHECK(owner::load_lockout(*n.store, n.fake_now).owner.locked(n.fake_now));
  REQUIRE(owner::reset(*n.store));
  init_passphrase(n);
  REQUIRE((*again)->unlock(pass.sign, nullptr, owner::kMinOps, owner::kMinMem));
}

TEST_CASE("phone pairing: guesses at claims cannot lock the owner out", "[phone-pairing]") {
  nas n;
  init_passphrase(n);
  // Someone on the network who can write the share, before the owner claims.
  claimer guesser(n);
  const auto wrong = with_passphrase("not the passphrase at all");
  for (int i = 0; i <= owner::kFreeMisses; ++i) (void)guesser.claim(&wrong);
  std::string why;
  CHECK_FALSE(guesser.claim(&wrong, &why));
  CHECK(why == "locked-out");
  // The claim counter is locked: the owner's first claim waits for it, or for a reset on the NAS.
  claimer mac(n);
  const auto pass = with_passphrase(kPass);
  CHECK_FALSE(mac.claim(&pass, &why));
  CHECK(why == "locked-out");
  REQUIRE(owner::reset(*n.store));
  init_passphrase(n);
  REQUIRE(mac.claim(&pass));
  // Owned now: more guesses never reach the passphrase step, and the owner's re-claim and session go on.
  for (int i = 0; i <= owner::kFreeMisses + 1; ++i) (void)guesser.claim(&wrong);
  CHECK(owner::load_lockout(*n.store, std::time(nullptr)).owner.misses == 0);
  REQUIRE(mac.claim(&pass));
  auto s = mac.session();
  REQUIRE(s);
  REQUIRE((*s)->unlock(pass.sign, nullptr, owner::kMinOps, owner::kMinMem));
}

TEST_CASE("phone pairing: answers come at most one a second, and a cheap challenge is never answered", "[phone-pairing]") {
  nas n;
  n.answer_gap_ms = 60000;  // far more than a loaded machine takes between two answers
  n.srv->stop();
  n.srv.reset();
  n.start();
  init_passphrase(n);
  claimer m(n);
  const auto pass = with_passphrase(kPass);
  REQUIRE(m.claim(&pass));
  auto s = m.session();
  REQUIRE(s);
  const auto wrong = [](const owner::salt_t& salt, std::uint64_t ops, std::uint64_t mem) {
    return owner::derive("the wrong passphrase", salt, ops, mem);
  };
  std::string refusal;
  CHECK_FALSE((*s)->unlock(wrong, &refusal, owner::kMinOps, owner::kMinMem));
  CHECK(why_in(refusal) == "passphrase");
  CHECK_FALSE((*s)->unlock(wrong, &refusal, owner::kMinOps, owner::kMinMem));
  CHECK(why_in(refusal) == "slow-down");
  // Not counted: one miss so far.
  CHECK(owner::load_lockout(*n.store, std::time(nullptr)).owner.misses == 1);
  // A client that expects the real cost refuses one far below it, and sends nothing a fake could attack cheaply.
  auto cheap = (*s)->unlock(pass.sign, &refusal, owner::kOps, owner::kMem);
  REQUIRE_FALSE(cheap);
  CHECK(cheap.error() == mv::status::corrupt);
  // A claim pinned to another identity says nothing to this one.
  auto pinned = with_passphrase(kPass);
  pinned.service_key = random_key();
  std::string why;
  CHECK_FALSE(m.claim(&pinned, &why));
  CHECK(why == "wrong-service");
  // (The pinned claim's own answer would wait out the gap: its passing is the claim tests'.)
}

// ---- timed and scoped phones (docs/plans/nas-owner-and-guests.md §5) ------------------------

namespace grants = mv::driftwire::serve::grants;

std::string guest_code_request(const std::string& code, std::int64_t for_s) {
  return "{\"do\":\"guest_code\",\"code\":\"" + code + "\",\"ttl_ms\":60000,\"for_s\":" + std::to_string(for_s) + "}";
}

bool on_grants(nas& n, const key32& device) {
  std::vector<grants::grant> list;
  REQUIRE(grants::load(*n.store, list));
  const key32 pub = public_of(device);
  return std::any_of(list.begin(), list.end(), [&](const grants::grant& g) { return g.key == pub; });
}

TEST_CASE("phone pairing: a guest code pairs a phone for a time, then it is cut and refused as expired", "[phone-pairing]") {
  nas n;
  claimer m(n);
  REQUIRE(m.claim(nullptr));
  auto owner_session = m.session();
  REQUIRE(owner_session);
  const std::int64_t t0 = std::time(nullptr);
  n.fake_now = t0;
  const std::string code(32, 'c');
  REQUIRE((*owner_session)->admin(guest_code_request(code, 3600)));
  const key32 guest = random_key();
  auto phone = n.phone(guest, code, nullptr, "Guest phone");
  REQUIRE(phone);
  // Told its limit, so it can say so.
  CHECK((*phone)->caps().has_grant);
  CHECK((*phone)->caps().grant_expires == t0 + 3600);
  CHECK_FALSE((*phone)->caps().grant_scoped);
  // On the grants, never on the paired list (an older NAS, or an older Mac, would let it in for ever).
  CHECK(on_grants(n, guest));
  CHECK_FALSE(n.has_device(guest));
  auto devices = (*owner_session)->admin("{\"do\":\"devices\"}");
  REQUIRE(devices);
  CHECK(devices->find("Guest phone") == std::string::npos);
  auto listed = (*owner_session)->admin("{\"do\":\"grants\"}");
  REQUIRE(listed);
  CHECK(listed->find("Guest phone") != std::string::npos);
  CHECK(listed->find("\"guest\":true") != std::string::npos);
  // It reads like any phone meanwhile.
  CHECK((*phone)->roots());
  // The hour runs out: the running session is cut within the watch's 2 s, as a revoke is.
  n.fake_now = t0 + 3600;
  CHECK(eventually([&] { return (*phone)->closed(); }, 6));
  CHECK((*phone)->ended_why() == "expired");
  // And refused, saying why, distinct from a revoke.
  serve::refusal why = serve::refusal::none;
  CHECK_FALSE(n.phone(guest, {}, &why));
  CHECK(why == serve::refusal::expired);
  CHECK(n.srv->stats().expired >= 2);
  // Listed as expired for a week, for Extend; then dropped.
  listed = (*owner_session)->admin("{\"do\":\"grants\"}");
  REQUIRE(listed);
  CHECK(listed->find("\"expired\":true") != std::string::npos);
  n.fake_now = t0 + 3600 + grants::kExpiredKeptSeconds + 1;
  listed = (*owner_session)->admin("{\"do\":\"grants\"}");
  REQUIRE(listed);
  CHECK(listed->find("Guest phone") == std::string::npos);
}

TEST_CASE("phone pairing: an expired guest is let back in by a new code, or by an extension", "[phone-pairing]") {
  nas n;
  init_passphrase(n);
  claimer m(n);
  const auto pass = with_passphrase(kPass);
  REQUIRE(m.claim(&pass));
  auto s = m.session();
  REQUIRE(s);
  // A guest code adds access: it needs the passphrase.
  std::string refusal;
  CHECK_FALSE((*s)->admin(guest_code_request(std::string(32, 'd'), 60), nullptr, &refusal));
  CHECK(why_in(refusal) == "needs-unlock");
  REQUIRE((*s)->unlock(pass.sign, nullptr, owner::kMinOps, owner::kMinMem));
  REQUIRE((*s)->admin(guest_code_request(std::string(32, 'd'), 60)));
  const key32 guest = random_key();
  const std::int64_t t0 = std::time(nullptr);
  n.fake_now = t0;
  REQUIRE(n.phone(guest, std::string(32, 'd')));
  n.fake_now = t0 + 61;
  serve::refusal why = serve::refusal::none;
  CHECK_FALSE(n.phone(guest, {}, &why));
  CHECK(why == serve::refusal::expired);
  // Re-invited: a new code redeemed by the same phone replaces its grant.
  REQUIRE((*s)->admin(guest_code_request(std::string(32, 'e'), 3600)));
  auto back = n.phone(guest, std::string(32, 'e'));
  REQUIRE(back);
  CHECK((*back)->caps().grant_expires == t0 + 61 + 3600);
  (*back)->close();
  const std::string key = mv::driftwire::key_hex(public_of(guest));
  // Shortening takes no passphrase and takes effect now; extending needs it.
  REQUIRE((*s)->admin("{\"do\":\"lock\"}"));
  REQUIRE((*s)->admin("{\"do\":\"grant\",\"key\":\"" + key + "\",\"for_s\":10}"));
  n.fake_now = t0 + 61 + 11;
  CHECK_FALSE(n.phone(guest, {}, &why));
  CHECK(why == serve::refusal::expired);
  CHECK_FALSE((*s)->admin("{\"do\":\"grant\",\"key\":\"" + key + "\",\"for_s\":86400}", nullptr, &refusal));
  CHECK(why_in(refusal) == "needs-unlock");
  CHECK_FALSE((*s)->admin("{\"do\":\"grant\",\"key\":\"" + key + "\",\"forever\":true}", nullptr, &refusal));
  CHECK(why_in(refusal) == "needs-unlock");
  REQUIRE((*s)->unlock(pass.sign, nullptr, owner::kMinOps, owner::kMinMem));
  REQUIRE((*s)->admin("{\"do\":\"grant\",\"key\":\"" + key + "\",\"for_s\":86400}"));
  REQUIRE(n.phone(guest));
  // Made unlimited: it moves to the paired list, and off the grants.
  REQUIRE((*s)->admin("{\"do\":\"grant\",\"key\":\"" + key + "\",\"forever\":true}"));
  CHECK(n.has_device(guest));
  CHECK_FALSE(on_grants(n, guest));
  auto unlimited = n.phone(guest);
  REQUIRE(unlimited);
  CHECK_FALSE((*unlimited)->caps().has_grant);
  // Revoke takes it off whichever list it is on.
  REQUIRE((*s)->admin("{\"do\":\"revoke\",\"key\":\"" + key + "\"}"));
  CHECK_FALSE(n.has_device(guest));
  CHECK_FALSE(on_grants(n, guest));
}

TEST_CASE("phone pairing: an extension reaches a live session, and a stale code does not shut a live guest out", "[phone-pairing]") {
  nas n;
  claimer m(n);
  REQUIRE(m.claim(nullptr));
  auto s = m.session();
  REQUIRE(s);
  const std::int64_t t0 = std::time(nullptr);
  n.fake_now = t0;
  REQUIRE((*s)->admin(guest_code_request(std::string(32, 'a'), 3600)));
  const key32 guest = random_key();
  auto phone = n.phone(guest, std::string(32, 'a'));
  REQUIRE(phone);
  CHECK((*phone)->caps().grant_expires == t0 + 3600);
  // Extended while it reads: the session is refreshed so the phone learns the new end.
  const std::string key = mv::driftwire::key_hex(public_of(guest));
  REQUIRE((*s)->admin("{\"do\":\"grant\",\"key\":\"" + key + "\",\"for_s\":86400}"));
  CHECK(eventually([&] { return (*phone)->closed(); }, 6));
  CHECK((*phone)->ended_why() == "folders-changed");
  auto again = n.phone(guest);
  REQUIRE(again);
  CHECK((*again)->caps().grant_expires == t0 + 86400);
  // A guest that scans an old, spent code is still let in on the grant it has.
  auto rescanned = n.phone(guest, std::string(32, 'a'));
  REQUIRE(rescanned);
  CHECK((*rescanned)->caps().grant_expires == t0 + 86400);
  // Made unlimited while connected: refreshed, and it comes back with no limit.
  REQUIRE((*s)->admin("{\"do\":\"grant\",\"key\":\"" + key + "\",\"forever\":true}"));
  CHECK(eventually([&] { return (*rescanned)->closed(); }, 6));
  auto unlimited = n.phone(guest);
  REQUIRE(unlimited);
  CHECK_FALSE((*unlimited)->caps().has_grant);
  // Nothing in this was a revoke.
  CHECK((*again)->ended_why() != "revoked");
}

TEST_CASE("phone pairing: an unlimited phone that scans a guest code stays unlimited", "[phone-pairing]") {
  nas n;
  claimer m(n);
  REQUIRE(m.claim(nullptr));
  auto s = m.session();
  REQUIRE(s);
  const key32 phone = random_key();
  REQUIRE(known::save(*n.store, {known::device{"iPhone", public_of(phone), {}, false}}));
  REQUIRE((*s)->admin(guest_code_request(std::string(32, 'f'), 60)));
  auto c = n.phone(phone, std::string(32, 'f'));
  REQUIRE(c);
  CHECK_FALSE((*c)->caps().has_grant);
  CHECK(n.has_device(phone));
  CHECK_FALSE(on_grants(n, phone));
  // The code is spent: it pairs nothing else.
  serve::refusal why = serve::refusal::none;
  CHECK_FALSE(n.phone(random_key(), std::string(32, 'f'), &why));
  CHECK(why == serve::refusal::bad_code);
}

TEST_CASE("phone pairing: a scoped phone sees only its folders and the Catalogue; an emptied list is none", "[phone-pairing]") {
  nas n;
  claimer m(n);
  REQUIRE(m.claim(nullptr));
  put_file(n.share / "a.txt", "photo");
  put_file(n.share / ".mediaviewer" / "library.json", "{}");
  auto s = m.session();
  REQUIRE(s);
  const key32 phone = random_key();
  REQUIRE(known::save(*n.store, {known::device{"iPhone", public_of(phone), {}, false}}));
  const std::string key = mv::driftwire::key_hex(public_of(phone));
  auto c = n.phone(phone);
  REQUIRE(c);
  // Narrowed to no folders: cut at once with folders-changed, and back with the Catalogue alone.
  REQUIRE((*s)->admin("{\"do\":\"grant\",\"key\":\"" + key + "\",\"forever\":true,\"roots\":[]}"));
  CHECK(eventually([&] { return (*c)->closed(); }, 6));
  CHECK((*c)->ended_why() == "folders-changed");
  CHECK(on_grants(n, phone));
  auto back = n.phone(phone);
  REQUIRE(back);
  CHECK((*back)->caps().grant_scoped);
  CHECK((*back)->caps().grant_expires == 0);
  auto roots = (*back)->roots();
  REQUIRE(roots);
  REQUIRE(roots->size() == 1);
  CHECK(roots->front().label == serve::kCatalogueLabel);
  mv::driftwire::serve::end_code code{};
  CHECK_FALSE((*back)->stat("a1a1a1a1a1a1a1a1", "a.txt", &code));
  CHECK(code == mv::driftwire::serve::end_code::refused);
  CHECK((*back)->stat(roots->front().id, "library.json"));
  (*back)->close();
  // Given Photos back (wider: with no passphrase set, the owner key is enough).
  REQUIRE((*s)->admin("{\"do\":\"grant\",\"key\":\"" + key + "\",\"forever\":true,\"roots\":[\"a1a1a1a1a1a1a1a1\"]}"));
  auto photos = n.phone(phone);
  REQUIRE(photos);
  CHECK((*photos)->stat("a1a1a1a1a1a1a1a1", "a.txt"));
}

TEST_CASE("phone pairing: a guest code from the NAS's command line is kept apart and pairs a timed phone", "[phone-pairing]") {
  nas n;
  const std::int64_t t0 = std::time(nullptr);
  n.fake_now = t0;
  grants::limit gives;
  gives.for_s = 7 * 24 * 3600;
  const std::string code = grants::issue_guest_code(*n.store, 600, gives, t0);
  REQUIRE(code.size() == 32);
  // Not in the old value: a NAS rolled back to before this would pair it unlimited.
  CHECK_FALSE(n.store->exists(serve::kPairCodesName));
  const key32 guest = random_key();
  auto c = n.phone(guest, code);
  REQUIRE(c);
  CHECK((*c)->caps().grant_expires == t0 + gives.for_s);
  CHECK(on_grants(n, guest));
  serve::refusal why = serve::refusal::none;
  CHECK_FALSE(n.phone(random_key(), code, &why));
  CHECK(why == serve::refusal::bad_code);
  // An older binary reads only the paired list: this phone is not on it, so it would be refused, not let in.
  CHECK_FALSE(n.has_device(guest));
}

TEST_CASE("phone pairing: grants and durations parse strictly", "[phone-pairing]") {
  CHECK(grants::parse_duration("24h") == 86400);
  CHECK(grants::parse_duration("7d") == 7 * 86400);
  CHECK(grants::parse_duration("90m") == 5400);
  CHECK(grants::parse_duration("3600") == 3600);
  CHECK_FALSE(grants::parse_duration("0"));
  CHECK_FALSE(grants::parse_duration("-1h"));
  CHECK_FALSE(grants::parse_duration("366d"));
  CHECK_FALSE(grants::parse_duration("1w"));
  CHECK_FALSE(grants::parse_duration("h"));
  grants::grant g;
  g.name = "x";
  g.expires = 0;
  CHECK_FALSE(grants::decode(grants::encode({g})));  // no limit at all belongs in the paired list
  g.roots = std::vector<std::string>{};
  const auto back = grants::decode(grants::encode({g}));
  REQUIRE(back);
  REQUIRE(back->front().roots);
  CHECK(back->front().roots->empty());  // empty stays empty: none, never all
  CHECK_FALSE(back->front().allows("a1a1a1a1a1a1a1a1", false));
  CHECK(back->front().allows("a1a1a1a1a1a1a1a1", true));
}

// ---- the Mac's side of the passphrase and timed phones (desk) ---------------------------------

std::string why_desk(desk::desk& d) { return why_in(d.last_refusal()); }

TEST_CASE("phone pairing: the Mac chooses the passphrase at its claim, then unlocks, locks and adds a guest", "[phone-pairing]") {
  nas n;
  desktop mac;
  // A NAS with none: the claim chooses one, and the Mac keeps the key it made for ten minutes.
  REQUIRE(mac.d->claim(utf8(n.share), n.local(), "", kPass, true));
  owner::verifier v;
  REQUIRE(owner::load(*n.store, v) == owner::stored::set);
  auto status = mac.d->owner_status();
  REQUIRE(status);
  CHECK(status->passphrase == "set");
  CHECK(std::find(status->features.begin(), status->features.end(), "passphrase") != status->features.end());
  CHECK(status->unlocked_s > 0);
  // Adding needs an unlock: the kept key does it, with nobody asked.
  REQUIRE(mac.d->start_pairing());
  CHECK(mac.d->last_refusal() == "{}");
  // Locked: the next addition is refused, and says why, until the person unlocks again.
  REQUIRE(mac.d->lock());
  CHECK_FALSE(mac.d->start_pairing());
  CHECK(why_desk(*mac.d) == "needs-unlock");
  CHECK_FALSE(mac.d->unlock("not the passphrase at all"));
  CHECK(why_desk(*mac.d) == "passphrase");
  REQUIRE(mac.d->unlock(kPass));
  // A guest code: its phone is on the NAS's grants and the Mac's own, never on either paired list.
  auto guest_code = mac.d->start_pairing(desk::kDefaultPairTtlMs, 24 * 3600);
  REQUIRE(guest_code);
  const key32 guest = random_key();
  auto phone = n.phone(guest, guest_code->code, nullptr, "Visitor");
  REQUIRE(phone);
  CHECK((*phone)->caps().has_grant);
  auto rows = mac.d->refresh_devices();
  REQUIRE(rows);
  const key32 pub = public_of(guest);
  const auto it = std::find_if(rows->begin(), rows->end(), [&](const desk::device_row& r) { return r.key == pub; });
  REQUIRE(it != rows->end());
  CHECK(it->limited);
  CHECK(it->guest);
  CHECK(it->expires > 0);
  CHECK(known::load_named(*mac.store, desk::kDevicesName).empty());  // the Mac's paired list: unlimited only
  {
    // The timed list the nearby service is given: the guest, with its end.
    auto bytes = mac.store->get(desk::kGrantsName);
    REQUIRE(bytes);
    auto kept = mv::driftwire::serve::grants::decode(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    REQUIRE(kept);
    REQUIRE(kept->size() == 1);
    CHECK(kept->front().key == pub);
    CHECK(kept->front().guest);
    CHECK(kept->front().expires == it->expires);
  }
  // Removing is the device key's alone, even locked.
  REQUIRE(mac.d->lock());
  REQUIRE(mac.d->revoke(pub));
  rows = mac.d->refresh_devices();
  REQUIRE(rows);
  CHECK(std::none_of(rows->begin(), rows->end(), [&](const desk::device_row& r) { return r.key == pub; }));
}

TEST_CASE("phone pairing: the Mac proves a passphrase set at init, extends a guest, and changes it", "[phone-pairing]") {
  nas n;
  init_passphrase(n);
  desktop mac;
  // No passphrase: refused, and it says so.
  CHECK_FALSE(mac.d->claim(utf8(n.share), n.local(), ""));
  CHECK(why_desk(*mac.d) == "passphrase");
  // Choosing a new one on a NAS that has one never replaces it: it is checked, and wrong.
  CHECK_FALSE(mac.d->claim(utf8(n.share), n.local(), "", "a different long passphrase", true));
  CHECK(why_desk(*mac.d) == "passphrase");
  // Too short is said as such, before anything is written to the share.
  CHECK_FALSE(mac.d->claim(utf8(n.share), n.local(), "", "short", true));
  CHECK(why_desk(*mac.d) == "passphrase-short");
  CHECK_FALSE(fs::exists(n.share / serve::kClaimTokenFile));
  REQUIRE(mac.d->claim(utf8(n.share), n.local(), "", kPass, false));
  auto code = mac.d->start_pairing(desk::kDefaultPairTtlMs, 3600);
  REQUIRE(code);
  const key32 guest = random_key();
  const std::int64_t t0 = std::time(nullptr);
  n.fake_now = t0;
  REQUIRE(n.phone(guest, code->code));
  const key32 pub = public_of(guest);
  // Extended from the card: a longer end.
  REQUIRE(mac.d->grant(pub, 7 * 24 * 3600));
  auto rows = mac.d->refresh_devices();
  REQUIRE(rows);
  const auto row = std::find_if(rows->begin(), rows->end(), [&](const desk::device_row& r) { return r.key == pub; });
  REQUIRE(row != rows->end());
  CHECK(row->expires == t0 + 7 * 24 * 3600);
  // Made unlimited: it moves to the paired lists.
  REQUIRE(mac.d->grant(pub, 0));
  rows = mac.d->refresh_devices();
  REQUIRE(rows);
  CHECK(std::any_of(rows->begin(), rows->end(), [&](const desk::device_row& r) { return r.key == pub && !r.limited; }));
  CHECK(known::load_named(*mac.store, desk::kDevicesName).size() == 1);
  // A new passphrase, and the old one no longer unlocks.
  REQUIRE(mac.d->set_passphrase("the replacement passphrase"));
  REQUIRE(mac.d->lock());
  CHECK_FALSE(mac.d->unlock(kPass));
  REQUIRE(mac.d->unlock("the replacement passphrase"));
  REQUIRE(mac.d->start_pairing());
}

TEST_CASE("phone pairing: a slow-down keeps the Mac's key, and the reason reaches the card", "[phone-pairing]") {
  nas n;
  n.answer_gap_ms = 60000;  // one owner answer a minute: the second always comes too soon
  n.srv->stop();
  n.srv.reset();
  n.start();
  desktop mac;
  REQUIRE(mac.d->claim(utf8(n.share), n.local(), "", kPass, true));
  REQUIRE(mac.d->unlock(kPass));  // the owner counter's one answer this minute
  mac.d->release();               // the card lets the session go between requests
  // A new session needs an unlock: the kept key is too soon, and is not forgotten for that.
  CHECK_FALSE(mac.d->start_pairing());
  CHECK(why_desk(*mac.d) == "slow-down");
  auto status = mac.d->owner_status();
  REQUIRE(status);
  CHECK(status->unlocked_s > 0);
  // A wrong key is forgotten: the passphrase changed on the NAS meanwhile.
  init_passphrase(n, "the replacement passphrase");
  n.srv->stop();
  n.srv.reset();
  n.answer_gap_ms = 0;
  n.start();
  REQUIRE(mac.d->set_local(n.local()));  // the restarted service listens on a new port
  CHECK_FALSE(mac.d->start_pairing());
  status = mac.d->owner_status();
  REQUIRE(status);
  CHECK(status->unlocked_s == 0);
}

TEST_CASE("phone pairing: the route carries the passphrase and the limits to the card", "[phone-pairing]") {
  nas n;
  desktop mac;
  auto claimed = desk::route_request(*mac.d, "phone", "claim",
                                     "{\"share\":\"" + utf8(n.share) + "\",\"local\":\"" + n.local() +
                                         "\",\"passphrase\":\"" + kPass + "\",\"new_passphrase\":true}");
  REQUIRE(claimed);
  auto status = desk::route_request(*mac.d, "phone", "owner_status", "{}");
  REQUIRE(status);
  CHECK(status->find("\"passphrase\":\"set\"") != std::string::npos);
  auto pair = desk::route_request(*mac.d, "phone", "pair", "{\"for_s\":3600}");
  REQUIRE(pair);
  const auto doc = mv::json::parse(*pair, 2);
  REQUIRE(doc);
  REQUIRE(n.phone(random_key(), *doc->str("code")));
  auto devices = desk::route_request(*mac.d, "phone", "devices", "{}");
  REQUIRE(devices);
  CHECK(devices->find("\"limited\":true") != std::string::npos);
  CHECK(devices->find("\"guest\":true") != std::string::npos);
  REQUIRE(desk::route_request(*mac.d, "phone", "lock", "{}"));
  CHECK_FALSE(desk::route_request(*mac.d, "phone", "pair", "{}"));
  auto why = desk::route_request(*mac.d, "phone", "last_refusal", "{}");
  REQUIRE(why);
  CHECK(why_in(*why) == "needs-unlock");
  REQUIRE(desk::route_request(*mac.d, "phone", "unlock", std::string("{\"passphrase\":\"") + kPass + "\"}"));
  CHECK(desk::route_request(*mac.d, "phone", "pair", "{}"));
}

TEST_CASE("phone pairing: a phone is never told the salt", "[phone-pairing]") {
  nas n;
  init_passphrase(n);
  claimer m(n);
  const auto pass = with_passphrase(kPass);
  REQUIRE(m.claim(&pass));
  const key32 phone = random_key();
  REQUIRE(known::save(*n.store, {known::device{"iPhone", public_of(phone), {}, false}}));
  serve::client::options o;
  o.access_key = n.access;
  o.device_key = phone;
  o.service_key = n.identity;
  o.ops = serve::kAllOps | serve::kAdminOp;
  auto c = serve::client::open(n.connect(), o);
  REQUIRE(c);
  CHECK((*c)->caps().passphrase.empty());
  CHECK((*c)->caps().features.empty());
  std::string refusal;
  auto challenge = (*c)->admin("{\"do\":\"challenge\"}", nullptr, &refusal);
  CHECK_FALSE(challenge);
  CHECK(refusal.find("salt") == std::string::npos);
}

TEST_CASE("phone pairing: a claim whose answer was lost is answered again for its owner", "[phone-pairing]") {
  nas n;
  const key32 device = random_key();
  const auto write_proof = [&](std::string_view nonce) {
    put_file(n.share / serve::kClaimProofFile, nonce);
    return true;
  };
  const auto with_token = [&](const std::string& token) {
    put_file(n.share / serve::kClaimTokenFile, token);
    return serve::claim_key(token);
  };
  // A link that goes deaf once the proof is written: the service's answer never arrives here.
  class deafening final : public sp::link {
   public:
    explicit deafening(std::shared_ptr<sp::link> in) : in_(std::move(in)) {}
    mv::expected write_all(std::span<const std::uint8_t> b) override { return in_->write_all(b); }
    mv::result<std::size_t> read_some(std::span<std::uint8_t> into, int ms) override {
      auto n = in_->read_some(into, ms);
      if (deaf.load()) return mv::err(mv::status::io);
      return n;
    }
    void shutdown() noexcept override { in_->shutdown(); }
    sp::transport via() const noexcept override { return in_->via(); }
    std::atomic<bool> deaf{false};

   private:
    std::shared_ptr<sp::link> in_;
  };
  auto drop = std::make_shared<deafening>(n.connect());
  auto lost = serve::claim(drop, with_token(std::string(64, 'a')), device, "Mac", [&](std::string_view nonce) {
    write_proof(nonce);
    drop->deaf = true;
    return true;
  }, soon());
  REQUIRE_FALSE(lost);
  CHECK(lost.error() == mv::status::timeout);  // "it may have gone through": not a refusal
  for (int i = 0; i < 250 && !n.store->exists(serve::kOwnerName); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  REQUIRE(n.store->exists(serve::kOwnerName));  // the service took it
  // The same Mac claims again: same keys back, nothing changed on the service.
  const auto roots_before = n.srv->roots().size();
  auto again = serve::claim(n.connect(), with_token(std::string(64, 'b')), device, "Mac", write_proof, soon());
  REQUIRE(again);
  CHECK(again->access_key == n.access);
  CHECK(again->identity == n.identity);
  CHECK(n.srv->roots().size() == roots_before);
  // Anyone else with a token and a proof is still refused.
  auto other = serve::claim(n.connect(), with_token(std::string(64, 'c')), random_key(), "Other", write_proof, soon());
  REQUIRE_FALSE(other);
  CHECK(other.error() == mv::status::permission_denied);
}

// ---- pairing codes --------------------------------------------------------------------------

TEST_CASE("phone pairing: a code pairs a phone once", "[phone-pairing]") {
  nas n;
  desktop mac;
  REQUIRE(mac.d->claim(utf8(n.share), n.local(), ""));
  auto p = mac.d->start_pairing();
  REQUIRE(p);
  CHECK(p->code.size() == serve::kPairCodeChars);
  CHECK(p->expires_unix > std::time(nullptr));
  CHECK(p->service.access_key == n.access);

  const key32 phone = random_key();
  auto c = n.phone(phone, p->code);
  REQUIRE(c);
  CHECK((*c)->roots());  // a session like any paired device's
  (*c).reset();
  REQUIRE(n.has_device(phone));

  // No code needed any more.
  CHECK(n.phone(phone));

  // The Mac shows the phone it added, by the name the phone gave.
  auto list = mac.d->refresh_devices();
  REQUIRE(list);
  REQUIRE(list->size() == 1);
  CHECK((*list)[0].name == "iPhone");
  CHECK((*list)[0].key == public_of(phone));
  CHECK(mac.d->devices().size() == 1);

  // A second phone cannot use it.
  serve::refusal why = serve::refusal::none;
  const key32 other = random_key();
  auto used = n.phone(other, p->code, &why);
  REQUIRE_FALSE(used);
  CHECK(why == serve::refusal::bad_code);
  CHECK_FALSE(n.has_device(other));
}

TEST_CASE("phone pairing: an expired code pairs nothing", "[phone-pairing]") {
  nas n;
  desktop mac;
  REQUIRE(mac.d->claim(utf8(n.share), n.local(), ""));
  auto p = mac.d->start_pairing(40);
  REQUIRE(p);
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  serve::refusal why = serve::refusal::none;
  const key32 phone = random_key();
  auto late = n.phone(phone, p->code, &why);
  REQUIRE_FALSE(late);
  CHECK(why == serve::refusal::bad_code);
  CHECK_FALSE(n.has_device(phone));
}

TEST_CASE("phone pairing: a made-up code is refused and no code is not a pairing", "[phone-pairing]") {
  nas n;
  desktop mac;
  REQUIRE(mac.d->claim(utf8(n.share), n.local(), ""));
  auto p = mac.d->start_pairing();
  REQUIRE(p);
  serve::refusal why = serve::refusal::none;
  const key32 phone = random_key();
  auto wrong = n.phone(phone, std::string(32, 'f'), &why);
  REQUIRE_FALSE(wrong);
  CHECK(why == serve::refusal::bad_code);
  auto none = n.phone(phone, {}, &why);
  REQUIRE_FALSE(none);
  CHECK(why == serve::refusal::not_paired);
  CHECK_FALSE(n.has_device(phone));
  // The real one was not spent by either.
  CHECK(n.phone(phone, p->code));
}

TEST_CASE("phone pairing: a phone already paired that scans again goes on, and spends that code", "[phone-pairing]") {
  nas n;
  desktop mac;
  REQUIRE(mac.d->claim(utf8(n.share), n.local(), ""));
  auto first = mac.d->start_pairing();
  REQUIRE(first);
  const key32 phone = random_key();
  REQUIRE(n.phone(phone, first->code));
  // A second scan, from the same phone, with a new code.
  auto again = mac.d->start_pairing();
  REQUIRE(again);
  auto c = n.phone(phone, again->code);
  REQUIRE(c);
  CHECK((*c)->roots());
  // That code is gone: no other device can use it.
  serve::refusal why = serve::refusal::none;
  const key32 other = random_key();
  CHECK_FALSE(n.phone(other, again->code, &why));
  CHECK(why == serve::refusal::bad_code);
  CHECK_FALSE(n.has_device(other));
  // A paired phone holding a stale code is still a paired phone.
  CHECK(n.phone(phone, first->code));
}

TEST_CASE("phone pairing: a new code replaces the one waiting, and cancelling withdraws it", "[phone-pairing]") {
  nas n;
  desktop mac;
  REQUIRE(mac.d->claim(utf8(n.share), n.local(), ""));
  auto first = mac.d->start_pairing();
  REQUIRE(first);
  auto second = mac.d->start_pairing();
  REQUIRE(second);
  serve::refusal why = serve::refusal::none;
  const key32 a = random_key(), b = random_key();
  CHECK_FALSE(n.phone(a, first->code, &why));  // a photo of the first QR
  CHECK(why == serve::refusal::bad_code);
  // Cancel on the card: the one showing is withdrawn too.
  REQUIRE(mac.d->stop_pairing());
  why = serve::refusal::none;
  CHECK_FALSE(n.phone(b, second->code, &why));
  CHECK(why == serve::refusal::bad_code);
  CHECK_FALSE(n.has_device(a));
  CHECK_FALSE(n.has_device(b));
}

TEST_CASE("phone pairing: the Mac can tell when its code has been used", "[phone-pairing]") {
  nas n;
  desktop mac;
  REQUIRE(mac.d->claim(utf8(n.share), n.local(), ""));
  const key32 phone = random_key();
  auto first = mac.d->start_pairing();
  REQUIRE(first);
  REQUIRE(n.phone(phone, first->code));
  auto again = mac.d->start_pairing();
  REQUIRE(again);
  CHECK(mac.d->pairing_waiting() == true);
  REQUIRE(n.phone(phone, again->code));  // a re-scan by a phone already paired: nothing new in the list
  auto list = mac.d->refresh_devices();  // what the card's poll reads
  REQUIRE(list);
  CHECK(list->size() == 1);
  CHECK(mac.d->pairing_waiting() == false);
}

// ---- who may administer ----------------------------------------------------------------------

TEST_CASE("phone pairing: only the owner's session can administer", "[phone-pairing]") {
  nas n;
  desktop mac;
  REQUIRE(mac.d->claim(utf8(n.share), n.local(), ""));
  auto p = mac.d->start_pairing();
  REQUIRE(p);
  const key32 phone = random_key();
  REQUIRE(n.phone(phone, p->code));

  serve::client::options o;
  o.access_key = n.access;
  o.device_key = phone;
  o.service_key = n.identity;
  o.id = "bbbbbbbbbbbbbbbb";
  o.ops = serve::kAllOps | serve::kAdminOp;
  auto c = serve::client::open(n.connect(), o);
  REQUIRE(c);
  CHECK(((*c)->caps().ops & serve::kAdminOp) == 0);  // never offered to a phone
  auto refused = (*c)->admin("{\"do\":\"devices\"}");
  REQUIRE_FALSE(refused);
  CHECK(refused.error() == mv::status::permission_denied);
}

TEST_CASE("phone pairing: the owner adds and removes named folders", "[phone-pairing]") {
  nas n;
  desktop mac;
  REQUIRE(mac.d->claim(utf8(n.share), n.local(), ""));
  const fs::path video = n.dir.root() / "video";
  fs::create_directories(video);
  put_file(video / "clip.txt", "hello");

  auto id = mac.d->add_root("Video", utf8(video));
  REQUIRE(id);
  auto roots = mac.d->roots();
  REQUIRE(roots);
  REQUIRE(roots->size() == 3);

  auto p = mac.d->start_pairing();
  REQUIRE(p);
  const key32 phone = random_key();
  auto c = n.phone(phone, p->code);
  REQUIRE(c);
  auto seen = (*c)->roots();
  REQUIRE(seen);
  CHECK(seen->size() == 3);
  CHECK(read_whole(**c, *id, "clip.txt") == "hello");

  // Taking it away ends what was using it, and it is gone for the next session.
  REQUIRE(mac.d->remove_root(*id));
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (!(*c)->closed() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK((*c)->closed());
  auto next = n.phone(phone);
  REQUIRE(next);
  mv::driftwire::serve::end_code code = serve::end_code::ok;
  CHECK_FALSE((*next)->stat(*id, "clip.txt", &code));
  CHECK(code == serve::end_code::refused);

  // A folder that is not there is refused, and the last one cannot go.
  CHECK_FALSE(mac.d->add_root("Nope", utf8(n.dir.root() / "missing")));
  CHECK_FALSE(mac.d->add_root("Relative", "some/where"));
  auto left = mac.d->roots();
  REQUIRE(left);
  for (std::size_t i = 0; i + 1 < left->size(); ++i) REQUIRE(mac.d->remove_root((*left)[i].id));
  CHECK_FALSE(mac.d->remove_root(left->back().id));
}

// ---- revoking on both sides -------------------------------------------------------------------

namespace {

struct paired_setup {
  nas n;
  desktop mac;
  scratch_dir cat{"phone_nearby"};
  key32 phone = random_key();
  paired_setup() {
    REQUIRE(mac.d->claim(utf8(n.share), n.local(), ""));
    auto p = mac.d->start_pairing();
    REQUIRE(p);
    REQUIRE(n.phone(phone, p->code));
    REQUIRE(mac.d->refresh_devices());
    put_file(cat.root() / "current.json", "{\"generation\":5}");
    std::string big(3'000'000, '\0');
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 197 >> 3);
    put_file(cat.root() / "gen/000005/catalog.db", big);
    REQUIRE(mac.d->start_nearby(utf8(cat.root()), "lib-1"));
    REQUIRE(mac.d->nearby_port() != 0);
  }
  serve::client::options nearby_options(const key32& device) const {
    serve::client::options o;
    o.access_key = n.access;
    o.device_key = device;
    o.id = "bbbbbbbbbbbbbbbb";
    o.name = "phone";
    return o;
  }
  std::shared_ptr<sp::link> nearby_link() const {
    auto l = sp::net::connect({"::1", mac.d->nearby_port()}, 3000);
    REQUIRE(l);
    return std::shared_ptr<sp::link>(std::move(*l));
  }
  std::unique_ptr<serve::client> nearby(const key32& device) const {
    auto c = serve::client::open(nearby_link(), nearby_options(device));
    REQUIRE(c);
    return std::move(*c);
  }
  std::string catalogue_root(serve::client& c) const {
    auto r = c.roots();
    REQUIRE(r);
    REQUIRE(r->size() == 1);
    CHECK((*r)[0].label == serve::kCatalogueLabel);
    return (*r)[0].id;
  }
};

}  // namespace

TEST_CASE("phone pairing: revoking removes a phone from the service and the Mac", "[phone-pairing]") {
  paired_setup s;
  auto live_nas = s.n.phone(s.phone);
  REQUIRE(live_nas);
  auto live_nearby = s.nearby(s.phone);

  REQUIRE(s.mac.d->revoke(public_of(s.phone)));
  CHECK(s.mac.d->devices().empty());
  CHECK_FALSE(s.n.has_device(s.phone));

  // Both running sessions are cut, within the service's recheck.
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(6);
  while ((!(*live_nas)->closed() || !live_nearby->closed()) && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK((*live_nas)->closed());
  CHECK(live_nearby->closed());

  // And neither takes it back.
  serve::refusal why = serve::refusal::none;
  CHECK_FALSE(s.n.phone(s.phone, {}, &why));
  CHECK(why == serve::refusal::not_paired);
  why = serve::refusal::none;
  CHECK_FALSE(serve::client::open(s.nearby_link(), s.nearby_options(s.phone), &why));
  CHECK(why == serve::refusal::not_paired);
}

TEST_CASE("phone pairing: a revoke made while the NAS is away reaches it later", "[phone-pairing]") {
  paired_setup s;
  const std::string address = s.n.local();
  s.n.srv->stop();
  const auto r = s.mac.d->revoke(public_of(s.phone));
  REQUIRE_FALSE(r);
  CHECK(r.error() == mv::status::io);
  CHECK(s.mac.d->devices().empty());  // this desktop stops serving it at once
  CHECK(s.n.has_device(s.phone));

  s.n.start();
  auto svc = s.mac.d->service();
  REQUIRE(svc);
  // The service came back on another port in this test.
  (void)address;
  // Its owner claiming again is answered with the same keys (and the new address kept).
  REQUIRE(s.mac.d->claim(utf8(s.n.share), s.n.local(), ""));
  REQUIRE(s.mac.d->set_local(s.n.local()));
  auto list = s.mac.d->refresh_devices();
  REQUIRE(list);
  CHECK(list->empty());
  CHECK_FALSE(s.n.has_device(s.phone));
}

// ---- nearby ----------------------------------------------------------------------------------

TEST_CASE("phone pairing: a paired phone reads the newest generation nearby with no Accept", "[phone-pairing]") {
  paired_setup s;
  auto c = s.nearby(s.phone);
  const std::string root = s.catalogue_root(*c);
  CHECK(read_whole(*c, root, "current.json") == "{\"generation\":5}");
  auto st = c->stat(root, "gen/000005/catalog.db");
  REQUIRE(st);
  CHECK(st->size == 3'000'000);
  CHECK(read_whole(*c, root, "gen/000005/catalog.db").size() == 3'000'000);
}

TEST_CASE("phone pairing: a stranger, or a phone without the key, gets nothing nearby", "[phone-pairing]") {
  paired_setup s;
  serve::refusal why = serve::refusal::none;
  CHECK_FALSE(serve::client::open(s.nearby_link(), s.nearby_options(random_key()), &why));
  CHECK(why == serve::refusal::not_paired);
  auto o = s.nearby_options(s.phone);
  o.access_key = random_key();
  CHECK_FALSE(serve::client::open(s.nearby_link(), o));
}

TEST_CASE("phone pairing: a nearby transfer cut halfway resumes where it stopped", "[phone-pairing]") {
  paired_setup s;
  const std::string want = [&] {
    std::ifstream f(s.cat.root() / "gen/000005/catalog.db", std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  }();

  std::string got;
  {
    auto l = s.nearby_link();
    auto c = serve::client::open(l, s.nearby_options(s.phone));
    REQUIRE(c);
    const std::string root = s.catalogue_root(**c);
    auto rs = (*c)->read_range(root, "gen/000005/catalog.db", 0, UINT64_MAX, 256 * 1024);
    REQUIRE(rs);
    while (got.size() < want.size() / 2) {
      auto n = (*rs)->next(8000);
      REQUIRE(n);
      REQUIRE(*n);
      got.append(reinterpret_cast<const char*>((*n)->bytes.data()), (*n)->bytes.size());
    }
    l->shutdown();  // the Wi-Fi drops
    for (;;) {
      auto n = (*rs)->next(2000);
      if (!n || !*n) break;
      got.append(reinterpret_cast<const char*>((*n)->bytes.data()), (*n)->bytes.size());
    }
  }
  REQUIRE(got.size() < want.size());

  auto c = s.nearby(s.phone);
  const std::string root = s.catalogue_root(*c);
  got += read_whole(*c, root, "gen/000005/catalog.db", got.size());
  CHECK(got.size() == want.size());
  CHECK(got == want);
}

TEST_CASE("phone pairing: the nearby service closes anyone not on this network", "[phone-pairing]") {
  nas n;
  scratch_dir dir{"phone_lan"};
  auto store = folder_store(dir.root() / "secrets");
  desk::desk::options o;
  o.store = store.get();
  o.nearby_loopback = true;
  o.advertise = false;
  o.own_network = [](const sp::net::address&) { return false; };
  desk::desk d(std::move(o));
  REQUIRE(d.claim(utf8(n.share), n.local(), ""));
  const key32 phone = random_key();
  auto p = d.start_pairing();
  REQUIRE(p);
  REQUIRE(n.phone(phone, p->code));
  REQUIRE(d.refresh_devices());
  put_file(dir.root() / "cat/current.json", "{}");
  REQUIRE(d.start_nearby(utf8(dir.root() / "cat"), "lib-1"));
  serve::client::options c;
  c.access_key = n.access;
  c.device_key = phone;
  auto l = sp::net::connect({"::1", d.nearby_port()}, 3000);
  REQUIRE(l);
  CHECK_FALSE(serve::client::open(std::shared_ptr<sp::link>(std::move(*l)), c));
  auto st = d.nearby_stats();
  REQUIRE(st);
  CHECK(st->sessions == 0);
}

// ---- small pure pieces ------------------------------------------------------------------------

TEST_CASE("phone pairing: the check code is the shared one, over every field that decides trust", "[phone-pairing]") {
  key32 pub, id, access;
  for (std::size_t i = 0; i < 32; ++i) {
    pub[i] = static_cast<std::uint8_t>(i);
    id[i] = static_cast<std::uint8_t>(0x40 + i);
    access[i] = static_cast<std::uint8_t>(0x80 + i);
  }
  // Computed apart from this code (Python's hashlib), and the phone checks the same vector.
  const std::string code = mv::driftwire::pairing_check("lib-1", pub, id, access);
  CHECK(code == "4M00-H4RW-ERX1-DFY2");
  auto flip = [](key32 k) {
    k[31] ^= 1;
    return k;
  };
  CHECK(mv::driftwire::pairing_check("lib-2", pub, id, access) != code);
  CHECK(mv::driftwire::pairing_check("lib-1", flip(pub), id, access) != code);
  CHECK(mv::driftwire::pairing_check("lib-1", pub, flip(id), access) != code);
  CHECK(mv::driftwire::pairing_check("lib-1", pub, id, flip(access)) != code);
}

TEST_CASE("phone pairing: the route gives the check code for the service it claimed", "[phone-pairing]") {
  nas n;
  desktop mac;
  REQUIRE(mac.d->claim(utf8(n.share), n.local(), ""));
  key32 publisher;
  randombytes_buf(publisher.data(), publisher.size());
  const std::string request = "{\"library_id\":\"lib-1\",\"publisher_key\":\"" + sp::key_hex(publisher) + "\"}";
  auto r = desk::route_request(*mac.d, "phone", "pair", request);
  REQUIRE(r);
  const auto doc = mv::json::parse(*r, 4);
  REQUIRE(doc);
  CHECK(*doc->str("check") == mv::driftwire::pairing_check("lib-1", publisher, n.identity, n.access));
}

TEST_CASE("phone pairing: addresses a phone could never reach are named", "[phone-pairing]") {
  const auto a = [](const char* text) {
    auto v = sp::net::parse_address(text);
    REQUIRE(v);
    return *v;
  };
  using w = desk::address_warning;
  std::vector<sp::net::address> none;
  CHECK(desk::classify_addresses(none) == w::unresolved);
  CHECK(desk::classify_addresses(std::vector{a("192.168.1.10")}) == w::private_range);
  CHECK(desk::classify_addresses(std::vector{a("10.0.0.2")}) == w::private_range);
  CHECK(desk::classify_addresses(std::vector{a("fd12::1")}) == w::private_range);
  CHECK(desk::classify_addresses(std::vector{a("100.64.3.1")}) == w::carrier_nat);
  CHECK(desk::classify_addresses(std::vector{a("100.127.255.254")}) == w::carrier_nat);
  CHECK(desk::classify_addresses(std::vector{a("8.8.8.8")}) == w::none);
  CHECK(desk::classify_addresses(std::vector{a("2001:4860:4860::8888")}) == w::none);
  // A public IPv6 beside a carrier IPv4 is reachable for a phone that has IPv6.
  CHECK(desk::classify_addresses(std::vector{a("100.64.3.1"), a("2001:4860:4860::8888")}) == w::none);
  CHECK(desk::classify_addresses(std::vector{a("100.64.3.1"), a("192.168.1.1")}) == w::carrier_nat);
}

TEST_CASE("phone pairing: host and port are read strictly", "[phone-pairing]") {
  CHECK(desk::split_host_port("nas.local:7878") == std::pair<std::string, std::uint16_t>{"nas.local", 7878});
  CHECK(desk::split_host_port("[2001:db8::1]:80") == std::pair<std::string, std::uint16_t>{"2001:db8::1", 80});
  for (const char* bad : {"", "nas.local", "nas.local:", ":80", "nas:0", "nas:70000", "2001:db8::1", "[::1]", "a b:1", "nas:80x"}) {
    INFO(bad);
    CHECK_FALSE(desk::split_host_port(bad));
  }
}

TEST_CASE("phone pairing: only the phone link can use the route", "[phone-pairing]") {
  scratch_dir dir{"phone_route"};
  auto store = folder_store(dir.root() / "secrets");
  desk::desk::options o;
  o.store = store.get();
  o.advertise = false;
  desk::desk d(std::move(o));
  auto stranger = desk::route_request(d, "someone-elses-addon", "status", "{}");
  REQUIRE_FALSE(stranger);
  CHECK(stranger.error() == mv::status::permission_denied);
  auto unknown = desk::route_request(d, "phone", "no-such-method", "{}");
  REQUIRE_FALSE(unknown);
  CHECK(unknown.error() == mv::status::not_found);
  auto ok = desk::route_request(d, "phone", "status", "{}");
  REQUIRE(ok);
  const auto doc = mv::json::parse(*ok, 4);
  REQUIRE(doc);
  CHECK(doc->boolean("claimed") == false);
  // Nothing about the access key leaves except on a request for a code.
  CHECK(ok->find("access_key") == std::string::npos);
}

#if defined(__APPLE__)
TEST_CASE("phone pairing: the nearby service is findable by Bonjour", "[phone-pairing-bonjour]") {
  // The system's own mDNSResponder, on this machine's link: no DHT, no router.
  auto reg = mv::driftwire::advert::publish(54321, {{"v", "1"}, {"lib", "0011223344556677"}});
  REQUIRE(reg);
  struct found_t {
    bool seen = false;
  } found;
  DNSServiceRef ref = nullptr;
  REQUIRE(DNSServiceBrowse(&ref, 0, 0, mv::driftwire::advert::kServiceType, nullptr,
                           [](DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType e, const char*, const char*,
                              const char*, void* ctx) {
                             if (e == kDNSServiceErr_NoError) static_cast<found_t*>(ctx)->seen = true;
                           },
                           &found) == kDNSServiceErr_NoError);
  const int fd = DNSServiceRefSockFD(ref);
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!found.seen && std::chrono::steady_clock::now() < until) {
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, 200) > 0) REQUIRE(DNSServiceProcessResult(ref) == kDNSServiceErr_NoError);
  }
  DNSServiceRefDeallocate(ref);
  CHECK(found.seen);
}
#endif

#if !defined(_WIN32)
#include <signal.h>
#include <sys/stat.h>

TEST_CASE("phone pairing: the real program is claimed, keeps it across a restart, and can be disowned", "[phone-pairing]") {
  scratch_dir s("phone_pairing_cli");
  const fs::path data = s.root() / "svc";
  const fs::path share = s.root() / "share";
  fs::create_directories(data);
  fs::create_directories(share);
  {
    std::ofstream f(data / "serve.json", std::ios::binary);
    f << R"({"port":0,"name":"CLI","roots":[{"id":"cccccccccccccccc","label":"Lib","path":")" << utf8(share) << R"("}]})";
  }
  const std::string program = MV_DRIFTWIRE_CLI_TEST_PATH;
  const auto run_line = [&](const std::string& rest, bool ok = true) {
    FILE* p = popen(("'" + program + "' serve --test-data '" + utf8(data) + "' " + rest + " </dev/null 2>&1").c_str(), "r");
    REQUIRE(p);
    std::string out;
    char buf[256];
    while (fgets(buf, sizeof buf, p)) out += buf;
    CHECK((pclose(p) == 0) == ok);
    return out;
  };
  (void)run_line("init --no-passphrase");

  struct running {
    FILE* p = nullptr;
    int pid = 0;
    std::uint16_t port = 0;
    ~running() {
      if (pid > 0) (void)::kill(pid, SIGKILL);
      if (p) (void)pclose(p);
    }
  };
  const auto start = [&](running& r) {
    r.p = popen(("sh -c 'echo $$; exec \"$0\" \"$@\"' '" + program + "' serve --test-loopback --test-data '" + utf8(data) + "' </dev/null").c_str(), "r");
    REQUIRE(r.p);
    char line[256];
    REQUIRE(fgets(line, sizeof line, r.p));
    r.pid = std::atoi(line);
    REQUIRE(fgets(line, sizeof line, r.p));
    const std::string listening = line;
    REQUIRE(listening.find("listening on port") != std::string::npos);
    r.port = static_cast<std::uint16_t>(std::atoi(listening.substr(listening.rfind(' ') + 1).c_str()));
  };

  desktop mac;
  {
    running r;
    start(r);
    REQUIRE(mac.d->claim(utf8(share), "[::1]:" + std::to_string(r.port), ""));
    CHECK_FALSE(fs::exists(share / serve::kClaimProofFile));
    auto roots = mac.d->roots();
    REQUIRE(roots);
    CHECK(roots->size() == 2);
  }
  // Written to serve.json, this user's alone, so it survives the restart.
  {
    struct stat st{};
    REQUIRE(::stat(utf8(data / "serve.json").c_str(), &st) == 0);
    CHECK((st.st_mode & 077) == 0);
  }
  {
    running r;
    start(r);
    REQUIRE(mac.d->set_local("[::1]:" + std::to_string(r.port)));
    auto roots = mac.d->roots();
    REQUIRE(roots);
    CHECK(roots->size() == 2);
    // A second desktop still cannot take it.
    desktop other("Other");
    CHECK(other.d->claim(utf8(share), "[::1]:" + std::to_string(r.port), "").error() == mv::status::permission_denied);
    mac.d->release();
  }
  CHECK(run_line("disown").find("disowned") != std::string::npos);
  CHECK(run_line("disown", false).find("no owner") != std::string::npos);
  {
    running r;
    start(r);
    desktop next("Next");
    CHECK(next.d->claim(utf8(share), "[::1]:" + std::to_string(r.port), ""));
  }
}
#endif

// ---- Phone access in the app (docs/plans/phone-in-the-app.md) --------------------------------

namespace {

// The JSON inside a pairing QR (catalog::pair_url).
std::string qr_payload(const std::string& url) {
  const std::string prefix = "mediaviewer://pair#";
  REQUIRE(url.starts_with(prefix));
  std::string out(url.size(), '\0');
  std::size_t len = 0;
  REQUIRE(sodium_base642bin(reinterpret_cast<unsigned char*>(out.data()), out.size(), url.data() + prefix.size(),
                            url.size() - prefix.size(), nullptr, &len, nullptr,
                            sodium_base64_VARIANT_URLSAFE_NO_PADDING) == 0);
  out.resize(len);
  return out;
}

// The app's link over a desktop's desk, with no Local search (no source).
struct app_link {
  scratch_dir dir{"phone_link"};
  std::unique_ptr<sp::secrets::store> keys = folder_store(dir.root() / "keys");
  std::unique_ptr<mv::phone::link> l;
  explicit app_link(desktop& mac) {
    mv::phone::link::options o;
    o.data_dir = utf8(dir.root() / "phone");
    o.key_store = [this]() { return sp::secrets::file_store(utf8(dir.root() / "keys")); };
    o.route = [&mac](const char* method, const std::string& json) {
      return desk::route_request(*mac.d, "phone", method, json);
    };
    o.tick_s = 3600;
    l = std::make_unique<mv::phone::link>(std::move(o));
  }
  // The state once the queue is empty.
  mv::json::value done() {
    mv::json::value v;
    REQUIRE(eventually([&] {
      auto t = mv::json::parse(l->state_json(), 6);
      if (!t || !t->str("busy") || !t->str("busy")->empty()) return false;
      v = *t;
      return true;
    }));
    return v;
  }
  mv::json::value ask(const std::string& request) {
    REQUIRE(l->request(request));
    return done();
  }
};

}  // namespace

TEST_CASE("phone pairing: with no Local search a library is set up, claimed, paired and revoked", "[phone-pairing]") {
  nas n;
  desktop mac;
  app_link app(mac);

  // Choosing the folder starts the library on the share: nothing indexed yet.
  mv::json::writer f;
  f.begin_object().key("do").string("folder").key("path").string(utf8(n.share)).end_object();
  auto v = app.ask(f.str());
  CHECK(*v.str("error") == "");
  CHECK(*v.str("folder") == utf8(n.share));
  CHECK(*v.str("library_name") == "share");
  CHECK(v.boolean("local_search") == false);
  auto lib = mv::ai::catalog::read_library(utf8(n.share));
  REQUIRE(lib);
  CHECK(lib->library_id.size() == 32);
  CHECK_FALSE(fs::exists(n.share / ".mediaviewer" / "current.json"));  // no catalogue: the phone browses

  mv::json::writer c;
  c.begin_object().key("do").string("claim").key("local").string(n.local()).key("public").string("").end_object();
  v = app.ask(c.str());
  CHECK(*v.str("error") == "");
  CHECK(v.boolean("claimed") == true);
  REQUIRE(v.find("folders")->a.size() == 2);  // the share and the catalogue's own folder

  v = app.ask("{\"do\":\"pair\"}");
  REQUIRE(*v.str("error") == "");
  const auto qr = mv::json::parse(qr_payload(*v.str("qr")), 4);
  REQUIRE(qr);
  CHECK(*qr->str("library_id") == lib->library_id);
  CHECK(*qr->str("publisher_key") == lib->public_key);
  CHECK_FALSE(v.str("qr_fingerprint")->empty());

  // The phone redeems the code and lists the named folders.
  const key32 phone = random_key();
  auto client = n.phone(phone, *qr->str("code"));
  REQUIRE(client);
  CHECK(n.has_device(phone));
  auto roots = (*client)->roots();
  REQUIRE(roots);
  CHECK(roots->size() == 2);
  (*client)->close();
  // The card hears of it at its next look (every two seconds under a code).
  REQUIRE(eventually([&] {
    auto t = mv::json::parse(app.l->state_json(), 6);
    return t && !t->find("added")->a.empty();
  }));

  // Remove: the NAS refuses it from then on.
  mv::json::writer r;
  r.begin_object().key("do").string("revoke").key("key").string(mv::driftwire::key_hex(public_of(phone))).end_object();
  v = app.ask(r.str());
  CHECK(*v.str("error") == "");
  CHECK(v.find("devices")->a.empty());
  serve::refusal why{};
  CHECK_FALSE(n.phone(phone, {}, &why));
}

TEST_CASE("phone pairing: the publisher key is copied once from Local search's store", "[phone-pairing]") {
  scratch_dir dir{"phone_key"};
  auto now = folder_store(dir.root() / "now");
  auto before = folder_store(dir.root() / "before");
  namespace catalog = mv::ai::catalog;
  // Nothing anywhere, not asked to make one.
  auto none = catalog::publisher_key(now.get(), before.get(), false);
  REQUIRE_FALSE(none);
  CHECK(none.error() == mv::status::not_found);
  // A library Local search published: its key comes over and is gone from there.
  const auto old = catalog::make_keypair().second;
  REQUIRE(before->put(catalog::kPublisherKeyName, old));
  auto got = catalog::publisher_key(now.get(), before.get(), false);
  REQUIRE(got);
  CHECK(*got == old);
  CHECK(now->get(catalog::kPublisherKeyName) == old);
  CHECK(before->get(catalog::kPublisherKeyName) == old);  // left: a Local search from before reads it there
  // And it stays the same key from then on.
  CHECK(*catalog::publisher_key(now.get(), before.get(), true) == old);
  // No store at all.
  CHECK(catalog::publisher_key(nullptr, before.get(), true).error() == mv::status::unsupported_format);
}

TEST_CASE("phone pairing: a library is started once and never over another", "[phone-pairing]") {
  scratch_dir dir{"phone_start"};
  namespace catalog = mv::ai::catalog;
  const fs::path share = dir.root() / "share";
  fs::create_directories(share);
  const auto mine = catalog::public_of(catalog::make_keypair().second);
  const auto theirs = catalog::public_of(catalog::make_keypair().second);
  const std::string id = catalog::new_library_id();
  REQUIRE(catalog::start_library(utf8(share), id, mine));
  CHECK(catalog::start_library(utf8(share), id, mine));  // again: nothing to do
  CHECK(catalog::start_library(utf8(share), id, theirs).error() == mv::status::permission_denied);
  CHECK(catalog::start_library(utf8(share), catalog::new_library_id(), mine).error() == mv::status::permission_denied);
  auto lib = catalog::read_library(utf8(share));
  REQUIRE(lib);
  CHECK(lib->library_id == id);
  CHECK(lib->public_key == catalog::to_hex(mine));
  CHECK(catalog::start_library(utf8(dir.root() / "gone"), id, mine).error() == mv::status::io);
}

TEST_CASE("phone pairing: the desk's items move once from Driftwire's store", "[phone-pairing]") {
  scratch_dir dir{"phone_take"};
  auto from = folder_store(dir.root() / "driftwire");
  auto to = folder_store(dir.root() / "phone");
  const std::vector<std::uint8_t> a{1, 2, 3}, b{4, 5}, kept{9};
  REQUIRE(from->put(desk::kKeyName, a));
  REQUIRE(from->put(desk::kDevicesName, b));
  REQUIRE(from->put(desk::kServiceName, std::vector<std::uint8_t>{7}));
  REQUIRE(to->put(desk::kServiceName, kept));  // already here: left as it is
  desk::take_items(*from, *to);
  CHECK(to->get(desk::kKeyName) == a);
  CHECK(to->get(desk::kDevicesName) == b);
  CHECK(to->get(desk::kServiceName) == kept);
  CHECK_FALSE(from->get(desk::kKeyName));
  CHECK_FALSE(from->get(desk::kDevicesName));
  CHECK(from->get(desk::kServiceName));  // not taken: still there
}

TEST_CASE("phone pairing: with Local search loaded and nothing set up, the link does not wake", "[phone-pairing]") {
  scratch_dir dir{"phone_idle"};
  std::atomic<int> looks{0}, routes{0};
  mv::phone::link::options o;
  o.data_dir = utf8(dir.root() / "phone");
  o.key_store = [&]() { return sp::secrets::file_store(utf8(dir.root() / "keys")); };
  o.route = [&](const char*, const std::string&) -> mv::result<std::string> {
    ++routes;
    return mv::err(mv::status::io);
  };
  o.tick_s = 0.05;
  mv::phone::link l(std::move(o));
  l.resume();
  mv::phone::catalogue_source src;
  src.publish = [](const std::string&) -> mv::expected { return {}; };
  src.cancel = [] {};
  src.json = [&] {
    ++looks;
    return std::string("{\"running\":false}");  // Local search with no folder of its own
  };
  l.set_source(std::move(src));
  REQUIRE(eventually([&] { return looks.load() >= 1; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(600));  // twelve ticks' worth
  CHECK(looks.load() <= 2);  // the one look when it loaded, nothing after
  CHECK(routes.load() == 0);  // and nothing asked of the desk
}

TEST_CASE("phone pairing: a link never used starts nothing at quit", "[phone-pairing]") {
  scratch_dir dir{"phone_quit"};
  std::atomic<int> routes{0}, stores{0};
  mv::phone::link::options o;
  o.data_dir = utf8(dir.root() / "phone");
  o.key_store = [&]() {
    ++stores;
    return sp::secrets::file_store(utf8(dir.root() / "keys"));
  };
  o.route = [&](const char*, const std::string&) -> mv::result<std::string> {
    ++routes;
    return std::string("{}");
  };
  mv::phone::link l(std::move(o));
  l.stop();
  l.set_source({});  // the app's quit order (phone/app.cpp)
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(routes.load() == 0);
  CHECK(stores.load() == 0);
  CHECK_FALSE(l.request("{\"do\":\"refresh\"}"));  // stopped
}
