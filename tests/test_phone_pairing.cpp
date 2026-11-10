// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/mobile.md §7.2, §7.4, §7.5 over loopback: claiming the service,
// single-use pairing codes, revoking on both sides, the nearby service.
// Filter with "[phone-pairing]". Nothing here goes beyond ::1.
#include <sodium.h>

#include "driftwire_fixture.h"

#include "addons/driftwire/advert.h"
#include "addons/driftwire/desk.h"
#include "addons/driftwire/known.h"
#include "addons/driftwire/pair_check.h"
#include "addons/driftwire/serve.h"
#include "addons/driftwire/serve_client.h"

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
  CHECK(roots[1].path == utf8(n.share / serve::kCatalogueFolder));
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
  auto r = desk::route_request(*mac.d, "ai", "pair", request);
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

TEST_CASE("phone pairing: only Local search can use the route", "[phone-pairing]") {
  scratch_dir dir{"phone_route"};
  auto store = folder_store(dir.root() / "secrets");
  desk::desk::options o;
  o.store = store.get();
  o.advertise = false;
  desk::desk d(std::move(o));
  auto stranger = desk::route_request(d, "someone-elses-addon", "status", "{}");
  REQUIRE_FALSE(stranger);
  CHECK(stranger.error() == mv::status::permission_denied);
  auto unknown = desk::route_request(d, "ai", "no-such-method", "{}");
  REQUIRE_FALSE(unknown);
  CHECK(unknown.error() == mv::status::not_found);
  auto ok = desk::route_request(d, "ai", "status", "{}");
  REQUIRE(ok);
  const auto doc = mv::json::parse(*ok, 4);
  REQUIRE(doc);
  CHECK(doc->boolean("claimed") == false);
  // Nothing about the access key leaves except on a request for a code.
  CHECK(ok->find("access_key") == std::string::npos);
}

TEST_CASE("phone pairing: forgetting the add-on's secrets takes the phone's too", "[phone-pairing]") {
  paired_setup s;
  CHECK(s.mac.store->exists(desk::kKeyName));
  CHECK(s.mac.store->exists(desk::kServiceName));
  CHECK(s.mac.store->exists(desk::kDevicesName));
  (void)known::forget(*s.mac.store);
  CHECK_FALSE(s.mac.store->exists(desk::kKeyName));
  CHECK_FALSE(s.mac.store->exists(desk::kServiceName));
  CHECK_FALSE(s.mac.store->exists(desk::kDevicesName));
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
  (void)run_line("init");

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
