// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/mobile.md §5 rule 5, §7.4, §7.5: moving the signing key, the QR
// string, the local copy a nearby phone reads. Filter with "[phone-chain]".
#include <catch2/catch_test_macros.hpp>

#include <sodium.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "addons/ai/catalog.h"
#include "catalogue/catalog_chain.h"
#include "core/json.h"

namespace cat = mv::ai::catalog;
namespace fs = std::filesystem;

namespace {

struct temp_dir {
  fs::path p;
  temp_dir() {
    p = fs::temp_directory_path() / ("mv_phone_chain_" + std::to_string(randombytes_random()));
    fs::create_directories(p);
  }
  ~temp_dir() {
    std::error_code ec;
    fs::remove_all(p, ec);
  }
};

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

void spit(const fs::path& p, std::string_view text) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

struct keys {
  std::vector<std::uint8_t> pub, sec;
  keys() { std::tie(pub, sec) = cat::make_keypair(); }
};

std::string first_library(const fs::path& share, const keys& k, std::uint64_t epoch = 1) {
  mv::json::writer w;
  w.begin_object();
  w.key("format").integer(cat::kFormat);
  w.key("library_id").string("lib-1");
  w.key("public_key").string(cat::to_hex(k.pub));
  if (epoch != 1) w.key("epoch").integer(static_cast<std::int64_t>(epoch));
  w.end_object();
  spit(share / ".mediaviewer/library.json", w.str());
  return w.str();
}

std::string base64url_decode(std::string_view in) {
  std::string out(in.size(), '\0');
  std::size_t len = 0;
  REQUIRE(sodium_base642bin(reinterpret_cast<unsigned char*>(out.data()), out.size(), in.data(), in.size(), nullptr, &len,
                            nullptr, sodium_base64_VARIANT_URLSAFE_NO_PADDING) == 0);
  out.resize(len);
  return out;
}

}  // namespace

TEST_CASE("phone chain: a takeover signed by the old key is followed from the pin", "[phone-chain]") {
  REQUIRE(sodium_init() >= 0);
  temp_dir t;
  const keys a, b;
  (void)first_library(t.p, a);
  auto moved = cat::hand_over(t.p.string(), a.sec, b.pub);
  REQUIRE(moved);
  CHECK(moved->epoch == 2);
  CHECK(moved->public_key == cat::to_hex(b.pub));
  CHECK_FALSE(moved->restarted);

  const std::string doc = slurp(t.p / ".mediaviewer/library.json");
  auto f = cat::follow_chain(doc, "lib-1", a.pub, 1);
  REQUIRE(f);
  CHECK(f->public_key == b.pub);
  CHECK(f->epoch == 2);

  // A phone already on the new key stays there.
  auto same = cat::follow_chain(doc, "lib-1", b.pub, 2);
  REQUIRE(same);
  CHECK(same->public_key == b.pub);
}

TEST_CASE("phone chain: two moves are followed from the first key", "[phone-chain]") {
  REQUIRE(sodium_init() >= 0);
  temp_dir t;
  const keys a, b, c;
  (void)first_library(t.p, a);
  REQUIRE(cat::hand_over(t.p.string(), a.sec, b.pub));
  auto twice = cat::hand_over(t.p.string(), b.sec, c.pub);
  REQUIRE(twice);
  CHECK(twice->epoch == 3);
  auto f = cat::follow_chain(slurp(t.p / ".mediaviewer/library.json"), "lib-1", a.pub, 1);
  REQUIRE(f);
  CHECK(f->public_key == c.pub);
  CHECK(f->epoch == 3);
}

TEST_CASE("phone chain: only the key in charge can hand over", "[phone-chain]") {
  REQUIRE(sodium_init() >= 0);
  temp_dir t;
  const keys a, b, thief;
  (void)first_library(t.p, a);
  auto r = cat::hand_over(t.p.string(), thief.sec, b.pub);
  REQUIRE_FALSE(r);
  CHECK(r.error() == mv::status::permission_denied);
}

TEST_CASE("phone chain: an unsigned takeover is refused and needs a new scan", "[phone-chain]") {
  REQUIRE(sodium_init() >= 0);
  temp_dir t;
  const keys a, b;
  (void)first_library(t.p, a);
  auto r = cat::restart_publisher(t.p.string(), b.pub);
  REQUIRE(r);
  CHECK(r->restarted);
  CHECK(r->epoch == 2);
  CHECK(r->public_key == cat::to_hex(b.pub));

  const std::string doc = slurp(t.p / ".mediaviewer/library.json");
  auto f = cat::follow_chain(doc, "lib-1", a.pub, 1);
  REQUIRE_FALSE(f);
  CHECK(f.error() == mv::status::permission_denied);
  // The new scan pins the new key, and that works.
  auto scanned = cat::follow_chain(doc, "lib-1", b.pub, 2);
  REQUIRE(scanned);
  CHECK(scanned->public_key == b.pub);
}

TEST_CASE("phone chain: a forged, altered or rolled-back record is refused", "[phone-chain]") {
  REQUIRE(sodium_init() >= 0);
  temp_dir t;
  const keys a, b, mallory;
  (void)first_library(t.p, a);
  REQUIRE(cat::hand_over(t.p.string(), a.sec, b.pub));
  const std::string good = slurp(t.p / ".mediaviewer/library.json");

  SECTION("the record's new key is swapped") {
    std::string bad = good;
    for (std::size_t at = 0; (at = bad.find(cat::to_hex(b.pub), at)) != std::string::npos;) bad.replace(at, 64, cat::to_hex(mallory.pub));
    CHECK(cat::follow_chain(bad, "lib-1", a.pub, 1).error() == mv::status::permission_denied);
  }
  SECTION("the epoch is lowered onto the pin") {
    std::string bad = good;
    const auto at = bad.find("\"epoch\":2");
    REQUIRE(at != std::string::npos);
    auto chain_at = bad.find("\"epoch\":2", at + 1);
    if (chain_at == std::string::npos) chain_at = at;
    bad.replace(chain_at, 9, "\"epoch\":1");
    CHECK_FALSE(cat::follow_chain(bad, "lib-1", a.pub, 1));
  }
  SECTION("another library's chain") {
    CHECK(cat::follow_chain(good, "lib-2", a.pub, 1).error() == mv::status::permission_denied);
  }
  SECTION("a phone already past that record is not walked back") {
    const keys c;
    REQUIRE(cat::hand_over(t.p.string(), b.sec, c.pub));
    // What a restored share or an attacker would show a phone that moved on to c.
    CHECK(cat::follow_chain(good, "lib-1", c.pub, 3).error() == mv::status::permission_denied);
  }
  SECTION("not a library.json") {
    CHECK(cat::follow_chain("nonsense", "lib-1", a.pub, 1).error() == mv::status::corrupt);
  }
}

TEST_CASE("phone chain: a new desktop publishes after a takeover, the old one is locked out", "[phone-chain]") {
  REQUIRE(sodium_init() >= 0);
  temp_dir t;
  const keys a, b;
  (void)first_library(t.p, a);
  auto before = cat::published_generation(t.p.string(), "lib-1", a.pub);
  REQUIRE(before);
  REQUIRE(cat::hand_over(t.p.string(), a.sec, b.pub));
  CHECK(cat::published_generation(t.p.string(), "lib-1", b.pub));
  auto old = cat::published_generation(t.p.string(), "lib-1", a.pub);
  REQUIRE_FALSE(old);
  CHECK(old.error() == mv::status::permission_denied);
}

TEST_CASE("phone chain: a takeover request sits on the share until cleared", "[phone-chain]") {
  REQUIRE(sodium_init() >= 0);
  temp_dir t;
  const keys a, b;
  (void)first_library(t.p, a);
  CHECK(cat::read_takeover(t.p.string()).error() == mv::status::not_found);
  REQUIRE(cat::request_takeover(t.p.string(), b.pub, "Studio Mac"));
  auto r = cat::read_takeover(t.p.string());
  REQUIRE(r);
  CHECK(r->public_key == cat::to_hex(b.pub));
  CHECK(r->name == "Studio Mac");
  cat::clear_takeover(t.p.string());
  CHECK(cat::read_takeover(t.p.string()).error() == mv::status::not_found);
}

TEST_CASE("phone chain: the QR string is the shared format", "[phone-chain]") {
  REQUIRE(sodium_init() >= 0);
  const keys a;
  cat::pair_payload p;
  p.library_id = "lib-1";
  p.library_name = "Photos \xC3\xA9";
  p.publisher_key = a.pub;
  p.service_local = "192.168.1.5:7878";
  p.service_public = "nas.example.com:7878";
  p.service_identity_hex = std::string(64, 'a');
  p.access_key_hex = std::string(64, 'b');
  p.code = "0123456789abcdef0123456789abcdef";
  p.expires = 1800000000;
  const std::string url = cat::pair_url(p);
  const std::string prefix = "mediaviewer://pair#";
  REQUIRE(url.starts_with(prefix));
  CHECK(url.find('=') == std::string::npos);
  CHECK(url.find('+') == std::string::npos);
  CHECK(url.find('/', prefix.size()) == std::string::npos);
  const auto doc = mv::json::parse(base64url_decode(std::string_view(url).substr(prefix.size())), 4);
  REQUIRE(doc);
  CHECK(doc->integer("v") == 1);
  CHECK(*doc->str("library_id") == "lib-1");
  CHECK(*doc->str("library_name") == "Photos \xC3\xA9");
  CHECK(*doc->str("publisher_key") == cat::to_hex(a.pub));
  const auto* s = doc->find("service");
  REQUIRE(s);
  CHECK(*s->str("local") == "192.168.1.5:7878");
  CHECK(*s->str("public") == "nas.example.com:7878");
  CHECK(*s->str("identity") == std::string(64, 'a'));
  CHECK(*doc->str("access_key") == std::string(64, 'b'));
  CHECK(*doc->str("code") == p.code);
  CHECK(doc->integer("expires") == 1800000000);
}

TEST_CASE("phone chain: a nearby copy is signed, whole, and keeps the one before", "[phone-chain]") {
  REQUIRE(sodium_init() >= 0);
  temp_dir t;
  const keys a;
  const fs::path gen = t.p / "gen7";
  spit(gen / "manifest.json", "{\"generation\":7}");
  spit(gen / "catalog.db", "database");
  spit(gen / "tiny.pack", "tiny");
  spit(t.p / "tower/text.onnx", "model");
  spit(t.p / "pack.bin", "thumbs");

  cat::nearby_options o;
  o.dir = (t.p / "nearby").string();
  o.gen_dir = gen.string();
  o.library_id = "lib-1";
  o.generation = 7;
  o.public_key = a.pub;
  o.secret_key = a.sec;
  o.library_json = "{\"library_id\":\"lib-1\"}";
  o.shared = {{(t.p / "tower/text.onnx").string(), "models/b32/text.onnx", 5, "x"}, {(t.p / "pack.bin").string(), "thumbs/t-1.pack", 6, "y"}};
  REQUIRE(cat::prepare_nearby(o));

  const fs::path n = t.p / "nearby";
  CHECK(slurp(n / "gen/000007/catalog.db") == "database");
  CHECK(slurp(n / "models/b32/text.onnx") == "model");
  CHECK_FALSE(fs::exists(n / "thumbs/t-1.pack"));  // from the NAS (§7.5)
  CHECK(slurp(n / "library.json") == o.library_json);
  auto cur = cat::verify_current(slurp(n / "current.json"), a.pub);
  REQUIRE(cur);
  CHECK(cur->generation == 7);
  CHECK(cur->epoch == 1);

  // Two more: the previous one stays for a phone mid-copy; older ones go.
  for (std::uint64_t g : {8, 9}) {
    o.generation = g;
    REQUIRE(cat::prepare_nearby(o));
  }
  CHECK_FALSE(fs::exists(n / "gen/000007"));
  CHECK(fs::exists(n / "gen/000008"));
  CHECK(fs::exists(n / "gen/000009"));
  auto last = cat::verify_current(slurp(n / "current.json"), a.pub);
  REQUIRE(last);
  CHECK(last->generation == 9);

  // An older generation never replaces a newer one.
  o.generation = 8;
  CHECK_FALSE(cat::prepare_nearby(o));
  CHECK(cat::verify_current(slurp(n / "current.json"), a.pub)->generation == 9);
}

