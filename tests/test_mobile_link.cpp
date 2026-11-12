// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The phone's link and sync against a real `driftwire serve` on loopback
// (docs/plans/mobile.md M3, M4): pairing, the route race, signature and
// generation checks, resume, and the JPEG-512 cache. Filter with
// "[mobile-link]"; nothing here leaves 127.0.0.1 / ::1.
#include <sodium.h>

#include "catch_compat.h"

#include <array>
#include <filesystem>
#include <future>
#include <map>
#include <fstream>
#include <memory>
#include <span>
#include <sqlite3.h>

#include "addons/ai/catalog.h"
#include "addons/driftwire/known.h"
#include "addons/driftwire/pair_check.h"
#include "addons/driftwire/secrets.h"
#include "addons/driftwire/serve.h"
#include "addons/driftwire/transfer.h"
#include "mobile/link.h"
#include "mobile/link_generation.h"
#include "mobile/link_payload.h"
#include "mobile/link_thumbs.h"
#include "mobile/mv_mobile_link.h"
#include "mobile/sample.h"
#include "mobile/sound_fetch.h"
#include "mobile/sound_tower.h"
#include "mobile/sync.h"
#include "mobile/testing.h"

namespace fs = std::filesystem;
namespace mob = mv::mobile;
namespace serve = mv::driftwire::serve;
namespace dw = mv::driftwire;
namespace catalog = mv::ai::catalog;

namespace {

using key32 = std::array<std::uint8_t, 32>;

key32 random_key() {
  key32 k;
  randombytes_buf(k.data(), k.size());
  return k;
}

void write_file(const fs::path& p, std::string_view text) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string read_all(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string sha256_hex(std::string_view text) {
  unsigned char h[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(h, reinterpret_cast<const unsigned char*>(text.data()), text.size());
  return catalog::to_hex(std::span<const std::uint8_t>(h, sizeof h));
}

struct scratch {
  fs::path root;
  scratch() {
    root = fs::temp_directory_path() / ("mv_link_" + std::to_string(randombytes_random()));
    fs::create_directories(root);
  }
  ~scratch() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

struct publisher {
  std::vector<std::uint8_t> pub, secret;
  std::string library_id = "11111111-2222-3333-4444-555555555555";
  fs::path nas;  // the destination; .mediaviewer is under it
  fs::path work;
  std::vector<std::pair<std::string, std::string>> gen_files;  // more files in each generation (thumbs.idx)

  publisher(fs::path nas_dir, fs::path work_dir) : nas(std::move(nas_dir)), work(std::move(work_dir)) {
    auto [p, s] = catalog::make_keypair();
    pub = std::move(p);
    secret = std::move(s);
    fs::create_directories(nas);
  }

  // A hand-made generation through the real deliver() (the real signature and
  // layout); catalog.db holds the rows the link side reads.
  void publish(std::uint64_t generation, const std::string& payload = std::string(300000, 'x'),
               const std::vector<std::uint8_t>* other_secret = nullptr) {
    const fs::path gen = work / ("gen-" + std::to_string(generation));
    fs::create_directories(gen);
    make_catalog_db(gen / "catalog.db");
    write_file(gen / "extra.bin", payload + std::to_string(generation));
    for (const auto& [name, bytes] : gen_files) write_file(gen / name, bytes);
    json_manifest(gen, generation);
    catalog::deliver_options o;
    o.gen_dir = gen.string();
    o.destination = nas.string();
    o.library_id = library_id;
    o.generation = generation;
    o.now_unix = 1000000 + static_cast<std::int64_t>(generation);
    o.epoch = 1;
    o.public_key = pub;
    o.secret_key = other_secret ? *other_secret : secret;
    o.copy = [](const std::string& from, const std::string& to) -> mv::expected {
      std::error_code ec;
      fs::copy_file(from, to, ec);
      if (ec) return mv::err(mv::status::io);
      return {};
    };
    o.make_directories = [](const std::string& d) -> mv::expected {
      std::error_code ec;
      fs::create_directories(d, ec);
      if (ec) return mv::err(mv::status::io);
      return {};
    };
    catalog::control c;
    REQUIRE(catalog::deliver(o, c));
  }

  void make_catalog_db(const fs::path& path) {
    std::error_code ec;
    fs::remove(path, ec);
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db,
                         "CREATE TABLE assets(id INTEGER PRIMARY KEY, path TEXT, disk_path TEXT, size INTEGER, kind INTEGER, duration_ms INTEGER);"
                         "CREATE TABLE speech(start_ms INTEGER, end_ms INTEGER, asset_id INTEGER, text TEXT);"
                         "CREATE TABLE cues(start_ms INTEGER, asset_id INTEGER, kind INTEGER, text TEXT);"
                         "INSERT INTO assets VALUES(1, '/root-uuid/2024/pic.bin', NULL, 70000, 0, 0);"
                         "INSERT INTO speech VALUES(4200, 5000, 1, 'hello world');",
                         nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
  }

  void json_manifest(const fs::path& gen, std::uint64_t generation) {
    std::string files = "[";
    bool first = true;
    std::vector<std::string> names{"catalog.db", "extra.bin"};
    for (const auto& [name, bytes] : gen_files) names.push_back(name);
    for (const std::string& name : names) {
      const auto text = read_all(gen / name);
      if (!first) files += ",";
      first = false;
      files += "{\"name\":\"" + std::string(name) + "\",\"size\":" + std::to_string(text.size()) + ",\"sha256\":\"" + sha256_hex(text) + "\"}";
    }
    files += "]";
    const std::string manifest =
        "{\"format\":1,\"library_id\":\"" + library_id + "\",\"epoch\":1,\"generation\":" + std::to_string(generation) +
        ",\"created\":123,\"assets\":1,\"roots\":[{\"id\":\"root-uuid\",\"share\":\"Photos\",\"sub\":\"\"}],\"files\":" + files + "}";
    write_file(gen / "manifest.json", manifest);
  }
};

struct rig {
  scratch dir;
  fs::path data = dir.root / "data", photos = dir.root / "photos", catalogue_home = dir.root / "nas";
  std::unique_ptr<dw::secrets::store> store;
  key32 access{}, identity{}, device = random_key();
  std::unique_ptr<serve::server> srv;
  publisher pub{catalogue_home, dir.root / "work"};
  std::uint16_t port = 0;
  std::string payload;

  std::string photos_label = "Photos";

  explicit rig(std::size_t payload_bytes = 300000, std::string label = "Photos")
      : payload(payload_bytes, 'x'), photos_label(std::move(label)) {
    REQUIRE(sodium_init() >= 0);
    fs::create_directories(data);
    fs::create_directories(photos);
    REQUIRE(dw::secrets::owner_only_folder((data / "secrets").string()));
    store = dw::secrets::file_store((data / "secrets").string());
    REQUIRE(store);
    bool made = false;
    REQUIRE(serve::set_up(*store, identity, access, made));
    REQUIRE(made);
    write_file(photos / "2024" / "pic.bin", std::string(70000, 'p'));
    pub.publish(1, payload);
    start();
  }

  void start() {
    serve::server::options o;
    o.store = store.get();
    o.loopback_only = true;
    o.per_minute = 10000;
    o.per_source_pending = 8;
    o.max_pending = 32;
    o.cfg.port = 0;
    o.cfg.name = "NAS";
    o.cfg.roots.push_back({"a1a1a1a1a1a1a1a1", photos_label, photos.string()});
    o.cfg.roots.push_back({"c2c2c2c2c2c2c2c2", "Catalogue", (catalogue_home / ".mediaviewer").string()});
    o.make_listener = [] { auto l = dw::net::listen_internet(0, true, 32); REQUIRE(l); return std::move(*l); };
    auto s = serve::server::start(o);
    REQUIRE(s);
    srv = std::move(*s);
    port = srv->port();
  }

  void pair_now() {
    const std::string code = serve::issue_pair_code(*store, 600, std::time(nullptr));
    mob::route_report report;
    REQUIRE(mob::pair_device(config(), code, report));
  }

  mob::service_config config() const {
    mob::service_config c;
    c.local = "127.0.0.1:" + std::to_string(port);
    c.public_name = "localhost:" + std::to_string(port);
    c.identity = identity;
    c.access_key = access;
    c.device_key = device;
    c.device_name = "Test iPhone";
    return c;
  }

  mob::sync_config sync_config() {
    mob::sync_config c;
    c.library_dir = (dir.root / "phone").string();
    c.library_id = pub.library_id;
    std::copy(pub.pub.begin(), pub.pub.end(), c.publisher_key.begin());
    return c;
  }
};

std::string payload_text(const rig& r, std::int64_t expires, const std::string& extra_code = "abc") {
  const std::string json =
      "{\"v\":1,\"library_id\":\"" + r.pub.library_id + "\",\"library_name\":\"Family\",\"publisher_key\":\"" +
      catalog::to_hex(r.pub.pub) + "\",\"service\":{\"local\":\"" + r.config().local + "\",\"public\":\"" + r.config().public_name +
      "\",\"identity\":\"" + catalog::to_hex(r.identity) + "\"},\"access_key\":\"" + catalog::to_hex(r.access) + "\",\"code\":\"" +
      extra_code + "\",\"expires\":" + std::to_string(expires) + "}";
  std::string b64(sodium_base64_ENCODED_LEN(json.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING), '\0');
  sodium_bin2base64(b64.data(), b64.size(), reinterpret_cast<const unsigned char*>(json.data()), json.size(),
                    sodium_base64_VARIANT_URLSAFE_NO_PADDING);
  b64.resize(std::strlen(b64.c_str()));
  return "mediaviewer://pair#" + b64;
}

}  // namespace

TEST_CASE("link payload: only a well-formed code of ours parses", "[mobile-link]") {
  rig r;
  const auto good = mob::parse_pair_payload(payload_text(r, 2000000000));
  REQUIRE(good);
  CHECK(good->library_name == "Family");
  key32 pub_key{};
  std::copy(r.pub.pub.begin(), r.pub.pub.end(), pub_key.begin());
  CHECK(good->fingerprint == dw::pairing_check(r.pub.library_id, pub_key, r.identity, r.access));
  CHECK(good->local == r.config().local);
  CHECK(good->identity == r.identity);

  CHECK_FALSE(mob::parse_pair_payload("https://example.com#abc"));
  CHECK_FALSE(mob::parse_pair_payload("mediaviewer://pair#!!!not-base64!!!"));
  CHECK_FALSE(mob::parse_pair_payload("mediaviewer://pair#"));
  // A wrong version, a short key, no service.
  const auto coded = [&](std::string json) {
    std::string b64(sodium_base64_ENCODED_LEN(json.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING), '\0');
    sodium_bin2base64(b64.data(), b64.size(), reinterpret_cast<const unsigned char*>(json.data()), json.size(),
                      sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    b64.resize(std::strlen(b64.c_str()));
    return "mediaviewer://pair#" + b64;
  };
  CHECK_FALSE(mob::parse_pair_payload(coded(R"({"v":2})")));
  CHECK_FALSE(mob::parse_pair_payload(coded(R"({"v":1,"library_id":"x","library_name":"y","publisher_key":"abcd","access_key":"abcd","code":"c","expires":9,"service":{"local":"a:1","public":"b:2","identity":"abcd"}})")));
  // Random bytes never crash the parser.
  for (int i = 0; i < 2000; ++i) {
    std::string junk(randombytes_uniform(200), '\0');
    randombytes_buf(junk.data(), junk.size());
    (void)mob::parse_pair_payload("mediaviewer://pair#" + junk);
    (void)mob::parse_pair_payload(coded(junk));
  }

  mv_pair_info info;
  CHECK(mv_pair_parse(payload_text(r, std::time(nullptr) - 1).c_str(), std::time(nullptr), &info) == MV_LINK_EXPIRED);
  CHECK(mv_pair_parse(payload_text(r, std::time(nullptr) + 60).c_str(), std::time(nullptr), &info) == MV_LINK_OK);
  CHECK(std::string(info.fingerprint) == good->fingerprint);
  // Control and direction characters in a name are refused, as is a port of six digits.
  CHECK_FALSE(mob::parse_pair_payload(coded(R"({"v":1,"library_id":"x","library_name":"a\u202Eb","publisher_key":")" +
                                           catalog::to_hex(r.pub.pub) + R"(","access_key":")" + catalog::to_hex(r.access) +
                                           R"(","code":"c","expires":9,"service":{"local":"a:1","public":"","identity":")" +
                                           catalog::to_hex(r.identity) + R"("}})")));
  std::string host;
  std::uint16_t port = 0;
  CHECK_FALSE(mob::split_host_port("a:000080", host, port));
  CHECK(mob::split_host_port("[::1]:8080", host, port));
  CHECK(mv_pair_parse("nonsense", 0, &info) == MV_LINK_INVALID);
}

TEST_CASE("pairing check: 80 bits over the library, both keys and the access key", "[mobile-link]") {
  key32 a{}, b{}, c{};
  for (std::size_t i = 0; i < 32; ++i) {
    a[i] = static_cast<std::uint8_t>(i);
    b[i] = static_cast<std::uint8_t>(0x40 + i);
    c[i] = static_cast<std::uint8_t>(0x80 + i);
  }
  const std::string base = dw::pairing_check("lib", a, b, c);
  REQUIRE(base.size() == 19);  // 16 characters and three dashes
  CHECK(base[4] == '-');
  CHECK(base == dw::pairing_check("lib", a, b, c));
  CHECK(base == "VPBV-CGPD-2FSJ-CY83");  // the vector the desktop checks too (also from Python hashlib)
  CHECK(dw::pairing_check("lib-1", a, b, c) == "4M00-H4RW-ERX1-DFY2");  // the Mac branch's shared vector
  key32 flip = b;
  flip[31] ^= 1;
  CHECK(dw::pairing_check("lib", a, flip, c) != base);  // another service
  flip = c;
  flip[0] ^= 1;
  CHECK(dw::pairing_check("lib", a, b, flip) != base);  // another access key
  CHECK(dw::pairing_check("lib2", a, b, c) != base);
  CHECK(dw::pairing_check("lib", b, a, c) != base);
}

TEST_CASE("pairing: a good code once; a used code, a wrong service and no access key do not", "[mobile-link]") {
  rig r;
  const std::string code = serve::issue_pair_code(*r.store, 600, std::time(nullptr));
  REQUIRE_FALSE(code.empty());
  mob::route_report report;
  REQUIRE(mob::pair_device(r.config(), code, report));
  CHECK(r.srv->stats().paired == 1);
  CHECK_FALSE(report.address.empty());

  // A device already paired that pairs again (a re-scan) is answered, and spends nothing.
  mob::route_report again;
  CHECK(mob::pair_device(r.config(), "00000000000000000000000000000000", again));

  // Spent.
  mob::route_report twice;
  auto second = r.config();
  second.device_key = random_key();
  CHECK(mob::pair_device(second, code, twice).error() == mv::status::permission_denied);

  // It is on the list now: a real connection works, an unpaired key does not.
  mob::link_session s(r.config());
  mob::route_report connected;
  REQUIRE(s.connect(connected));
  auto roots = s.roots();
  REQUIRE(roots);
  CHECK(roots->size() == 2);

  mob::service_config stranger = r.config();
  stranger.device_key = random_key();
  mob::link_session s2(stranger);
  CHECK(s2.connect(connected).error() == mv::status::permission_denied);

  // A wrong pinned identity is refused and says so.
  mob::service_config wrong_id = r.config();
  wrong_id.identity = random_key();
  mob::link_session s3(wrong_id);
  mob::route_report bad;
  CHECK(s3.connect(bad).error() == mv::status::permission_denied);
  CHECK(std::any_of(bad.failures.begin(), bad.failures.end(),
                    [](const mob::route_failure& f) { return f.problem == mob::route_problem::wrong_service; }));

  // Without the access key: silence.
  mob::service_config no_key = r.config();
  no_key.access_key = random_key();
  mob::link_session s4(no_key);
  mob::route_report silent;
  CHECK(s4.connect(silent, nullptr, 1500).error() == mv::status::io);
}

TEST_CASE("link race: a dead local address loses to the named public route", "[mobile-link]") {
  rig r;
  r.pair_now();
  mob::service_config c = r.config();
  c.local = "127.0.0.1:1";  // nothing listens
  c.public_name = "localhost:" + std::to_string(r.port);
  mob::link_session s(c);
  mob::route_report report;
  REQUIRE(s.connect(report));
  CHECK(report.over_internet);
  CHECK(report.address == c.public_name);
  CHECK_FALSE(report.failures.empty());

  mob::service_config dead = r.config();
  dead.local = "127.0.0.1:1";
  dead.public_name = "localhost:1";
  mob::link_session none(dead);
  mob::route_report nothing;
  CHECK_FALSE(none.connect(nothing));
  CHECK_FALSE(nothing.failures.empty());
}

TEST_CASE("sync: copy, hash check, install, resume, signatures and generations", "[mobile-link]") {
  rig r(6 * 1024 * 1024);  // bigger than the stream window, so a drop can land mid-file
  r.pair_now();
  mob::link_session s(r.config());
  mob::route_report rep;
  REQUIRE(s.connect(rep));

  auto cfg = r.sync_config();
  const fs::path library = cfg.library_dir;

  auto plan = mob::check_catalogue(s, cfg);
  REQUIRE(plan.outcome == mob::sync_outcome::copy_available);
  CHECK(plan.generation == 1);
  CHECK(plan.total_bytes > 0);
  CHECK(plan.done_bytes == 0);
  CHECK_FALSE(fs::exists(library / "installed.json"));  // check touches nothing

  SECTION("cancelled mid-copy resumes without starting again, and nothing is installed meanwhile") {
    mob::cancel_flag stop;
    std::uint64_t seen = 0;
    auto out = mob::copy_catalogue(s, cfg, plan, [&](std::uint64_t done, std::uint64_t) {
      seen = done;
      if (done > 8192) stop.stop.store(true);
    }, &stop);
    CHECK(out == mob::sync_outcome::cancelled);
    CHECK_FALSE(fs::exists(library / "installed.json"));
    auto again = mob::check_catalogue(s, cfg);
    REQUIRE(again.outcome == mob::sync_outcome::copy_available);
    CHECK(again.done_bytes >= 8192);
    CHECK(again.done_bytes <= again.total_bytes);
    CHECK(seen >= 8192);
    std::uint64_t first_done = 0;
    auto finished = mob::copy_catalogue(s, cfg, again, [&](std::uint64_t done, std::uint64_t) {
      if (first_done == 0) first_done = done;
    }, nullptr);
    REQUIRE(finished == mob::sync_outcome::installed);
    CHECK(first_done >= again.done_bytes);
    CHECK(mob::check_catalogue(s, cfg).outcome == mob::sync_outcome::up_to_date);
  }

  SECTION("the service going away mid-copy (airplane mode) leaves nothing half-installed, and the next run resumes") {
    std::atomic<bool> stopped{false};
    auto out = mob::copy_catalogue(s, cfg, plan, [&](std::uint64_t done, std::uint64_t) {
      if (done > 64 * 1024 && !stopped.exchange(true)) r.srv->stop();
    }, nullptr);
    CHECK(out == mob::sync_outcome::unreachable);
    CHECK_FALSE(fs::exists(library / "installed.json"));
    r.start();  // the same keys and files, a new port
    mob::link_session s2(r.config());
    mob::route_report rep2;
    REQUIRE(s2.connect(rep2));
    auto cfg2 = r.sync_config();
    auto again = mob::check_catalogue(s2, cfg2);
    REQUIRE(again.outcome == mob::sync_outcome::copy_available);
    CHECK(again.done_bytes > 0);
    auto finished = mob::copy_catalogue(s2, cfg2, again, nullptr, nullptr);
    REQUIRE(finished == mob::sync_outcome::installed);
  }

  SECTION("installed files match the manifest, and an old or forged current.json is refused") {
    REQUIRE(mob::copy_catalogue(s, cfg, plan, nullptr, nullptr) == mob::sync_outcome::installed);
    auto installed = mob::read_installed(cfg.library_dir);
    REQUIRE(installed);
    CHECK(installed->generation == 1);
    const fs::path gen = library / "gen" / installed->dir;
    CHECK(fs::exists(gen / "catalog.db"));
    CHECK(fs::exists(gen / "manifest.json"));
    const std::string db_text = read_all(gen / "catalog.db");
    CHECK(db_text.size() > 0);

    // A newer generation installs; keep the old signed current.json to replay.
    const fs::path base = r.catalogue_home / ".mediaviewer";
    const std::string old_current = read_all(base / "current.json");
    r.pub.publish(2, r.payload);
    fs::create_directories(library / "engine" / "1-1");  // search's work folder for generation 1
    fs::create_directories(library / "engine" / "1-1+sound");  // and the one with sound search's model
    auto next = mob::check_catalogue(s, cfg);
    REQUIRE(next.outcome == mob::sync_outcome::copy_available);
    REQUIRE(mob::copy_catalogue(s, cfg, next, nullptr, nullptr) == mob::sync_outcome::installed);
    installed = mob::read_installed(cfg.library_dir);
    REQUIRE(installed->generation == 2);
    CHECK(installed->high_generation == 2);
    CHECK_FALSE(fs::exists(library / "gen" / "1-1"));
    CHECK_FALSE(fs::exists(library / "engine" / "1-1"));
    CHECK_FALSE(fs::exists(library / "engine" / "1-1+sound"));

    // Lower generation, correctly signed: refused, and the catalogue stays.
    write_file(base / "current.json", old_current);
    auto refused = mob::check_catalogue(s, cfg);
    CHECK(refused.outcome == mob::sync_outcome::older_refused);
    CHECK(mob::read_installed(cfg.library_dir)->generation == 2);
    CHECK(fs::exists(library / "gen" / "1-2"));

    // Signed by someone else: refused, and it says "rejected".
    r.pub.publish(3, r.payload);  // a fresh real generation
    const std::string good_current = read_all(base / "current.json");
    auto [other_pub, other_secret] = catalog::make_keypair();
    (void)other_pub;
    r.pub.publish(4, std::string(100, 'y'), &other_secret);  // deliver signs what it is given
    auto wrong = mob::check_catalogue(s, cfg);
    CHECK(wrong.outcome == mob::sync_outcome::rejected);
    CHECK(mob::read_installed(cfg.library_dir)->generation == 2);
    write_file(base / "current.json", good_current);
    CHECK(mob::check_catalogue(s, cfg).outcome == mob::sync_outcome::copy_available);
  }

  SECTION("a tampered file in the signed manifest is refused") {
    // current.json and the manifest stay valid; a byte of the file changes.
    const fs::path gen = r.catalogue_home / ".mediaviewer" / "gen" / "000001";
    auto text = read_all(gen / "extra.bin");
    text[10] = 'Z';
    write_file(gen / "extra.bin", text);
    auto out = mob::copy_catalogue(s, cfg, plan, nullptr, nullptr);
    CHECK(out == mob::sync_outcome::rejected);
    CHECK_FALSE(fs::exists(library / "installed.json"));
  }
}

TEST_CASE("the generation view reads files and moments from catalog.db", "[mobile-link]") {
  rig r;
  r.pair_now();
  mob::link_session s(r.config());
  mob::route_report rep;
  REQUIRE(s.connect(rep));
  auto cfg = r.sync_config();
  auto plan = mob::check_catalogue(s, cfg);
  REQUIRE(mob::copy_catalogue(s, cfg, plan, nullptr, nullptr) == mob::sync_outcome::installed);
  const auto dir = (fs::path(cfg.library_dir) / "gen" / mob::read_installed(cfg.library_dir)->dir).string();
  auto view = mob::generation_view::open(dir);
  REQUIRE(view);
  const auto f = (*view)->file(1);
  REQUIRE(f);
  CHECK(f->uuid == "root-uuid");
  CHECK(f->rel == "2024/pic.bin");
  CHECK((*view)->roots().at("root-uuid").share == "Photos");
  const auto moments = (*view)->moments(1);
  REQUIRE(moments.size() == 1);
  CHECK(moments[0].text == "hello world");
  CHECK_FALSE((*view)->file(999));
}

TEST_CASE("the C ABI reads an original, and says when no service folder matches its share", "[mobile-link]") {
  const auto open_synced = [](rig& r) {
    r.pair_now();
    const auto cfg = r.sync_config();
    const auto service = r.config();
    const std::string cache = (r.dir.root / "thumbs").string();
    mv_link_config c{};
    c.library_dir = cfg.library_dir.c_str();
    c.cache_dir = cache.c_str();
    c.library_id = cfg.library_id.c_str();
    c.local = service.local.c_str();
    c.public_name = service.public_name.c_str();
    c.device_name = service.device_name.c_str();
    c.identity = service.identity.data();
    c.access_key = service.access_key.data();
    c.device_key = service.device_key.data();
    c.publisher_key = cfg.publisher_key.data();
    mv_mobile_link* l = mv_link_open(&c);
    REQUIRE(l);
    mv_route_report report{};
    REQUIRE(mv_link_connect(l, nullptr, &report) == MV_LINK_OK);
    mv_sync_report sync{};
    REQUIRE(mv_sync_copy(l, nullptr, nullptr, nullptr, nullptr, &sync) == MV_LINK_OK);
    return l;
  };
  std::string got;
  const mv_sink_fn collect = [](void* ctx, uint64_t, const uint8_t* data, size_t size) -> int32_t {
    static_cast<std::string*>(ctx)->append(reinterpret_cast<const char*>(data), size);
    return 1;
  };

  SECTION("a folder labelled with the share's name") {
    rig r;
    auto* l = open_synced(r);
    CHECK(mv_asset_folder_check(l, 1) == MV_LINK_OK);
    CHECK(mv_file_read(l, 1, 0, UINT64_MAX, 0, nullptr, collect, &got) == MV_LINK_OK);
    CHECK(got == std::string(70000, 'p'));
    mv_link_close(l);
  }
  SECTION("two copies at once: one copies, the other finds it done") {
    rig r(6 * 1024 * 1024);
    r.pair_now();
    const auto cfg = r.sync_config();
    const auto service = r.config();
    const std::string cache = (r.dir.root / "thumbs").string();
    mv_link_config c{};
    c.library_dir = cfg.library_dir.c_str();
    c.cache_dir = cache.c_str();
    c.library_id = cfg.library_id.c_str();
    c.local = service.local.c_str();
    c.public_name = service.public_name.c_str();
    c.device_name = service.device_name.c_str();
    c.identity = service.identity.data();
    c.access_key = service.access_key.data();
    c.device_key = service.device_key.data();
    c.publisher_key = cfg.publisher_key.data();
    mv_mobile_link* l = mv_link_open(&c);
    REQUIRE(l);
    int32_t a = -1, b = -1;
    std::thread first([&] { mv_sync_report rep{}; a = mv_sync_copy(l, nullptr, nullptr, nullptr, nullptr, &rep); });
    std::thread second([&] { mv_sync_report rep{}; b = mv_sync_copy(l, nullptr, nullptr, nullptr, nullptr, &rep); });
    first.join();
    second.join();
    CHECK(((a == MV_LINK_OK && b == MV_LINK_UP_TO_DATE) || (a == MV_LINK_UP_TO_DATE && b == MV_LINK_OK)));
    const auto installed = mob::read_installed(cfg.library_dir);
    REQUIRE(installed);
    CHECK(fs::exists(fs::path(cfg.library_dir) / "gen" / installed->dir / "catalog.db"));
    mv_link_close(l);
  }
  SECTION("a folder labelled something else is said to be missing, not gone") {
    rig r(300000, "Pictures");
    auto* l = open_synced(r);
    CHECK(mv_asset_folder_check(l, 1) == MV_LINK_NO_FOLDER);
    CHECK(mv_file_read(l, 1, 0, UINT64_MAX, 0, nullptr, collect, &got) == MV_LINK_NO_FOLDER);
    CHECK(got.empty());
    mv_link_close(l);
  }
}

TEST_CASE("link: a live link is kept on connect, and a reconnect lets reads finish on the old one", "[mobile-link]") {
  rig r(6 * 1024 * 1024);
  r.pair_now();
  mob::link_session s(r.config());
  mob::route_report rep;
  REQUIRE(s.connect(rep));
  const std::string file = "gen/000001/extra.bin";
  const auto expected_size = r.payload.size() + 1;
  for (const bool swap : {false, true}) {
    std::atomic<std::uint64_t> got{0};
    std::atomic<bool> started{false};
    mv::expected done = mv::err(mv::status::internal);
    std::thread reader([&] {
      done = s.read_range("c2c2c2c2c2c2c2c2", file, 0, UINT64_MAX, [&](std::uint64_t, std::span<const std::uint8_t> b) {
        got += b.size();
        started = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return true;
      }, nullptr, false);
    });
    while (!started) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    mob::route_report again;
    // The foreground and a network change, in the middle of a read.
    if (swap) {
      REQUIRE(s.reconnect(again));
    } else {
      REQUIRE(s.connect(again));
    }
    reader.join();
    CHECK(done);
    CHECK(got.load() == expected_size);
  }
}

TEST_CASE("link: readers after a drop share one new connection", "[mobile-link]") {
  rig r;
  r.pair_now();
  mob::link_session s(r.config());
  mob::route_report rep;
  REQUIRE(s.connect(rep));
  s.disconnect();
  std::atomic<int> ok{0};
  std::vector<std::thread> readers;
  for (int i = 0; i < 60; ++i) {  // more than the service's 32 streams a session
    readers.emplace_back([&] {
      if (s.stat("c2c2c2c2c2c2c2c2", "current.json")) ++ok;
    });
  }
  for (auto& t : readers) t.join();
  CHECK(ok.load() == 60);
}

TEST_CASE("link: Forget cuts reads on a swapped-out client and wakes readers waiting for a stream", "[mobile-link]") {
  rig r(6 * 1024 * 1024);
  r.pair_now();
  mob::link_session s(r.config());
  mob::route_report rep;
  REQUIRE(s.connect(rep));
  const std::string file = "gen/000001/extra.bin";
  std::atomic<int> finished{0}, started{0};
  std::vector<std::thread> readers;
  // 40 slow reads: 31 hold every stream the client allows, the rest wait for one.
  for (int i = 0; i < 40; ++i) {
    readers.emplace_back([&] {
      bool counted = false;
      (void)s.read_range("c2c2c2c2c2c2c2c2", file, 0, UINT64_MAX, [&](std::uint64_t, std::span<const std::uint8_t>) {
        if (!counted) {
          counted = true;
          ++started;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));  // a whole file takes many seconds
        return true;
      }, nullptr, false);
      ++finished;
    });
  }
  for (int i = 0; i < 500 && started.load() < 31; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  CHECK(started.load() == 31);  // never more than 31 at once
  mob::route_report again;
  REQUIRE(s.reconnect(again));  // the 31 now run on a client that is no longer the live one
  const auto t0 = std::chrono::steady_clock::now();
  s.shut_down();
  for (auto& t : readers) t.join();
  CHECK(finished.load() == 40);
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));
  CHECK_FALSE(s.connect(again));  // and nothing connects again
}

TEST_CASE("link: Forget alone wakes a reader waiting for a stream, while every stream is held", "[mobile-link]") {
  rig r;
  r.pair_now();
  mob::link_session s(r.config());
  mob::route_report rep;
  REQUIRE(s.connect(rep));
  // 31 reads whose sinks do not return until released: their streams never end by themselves.
  std::mutex hold_m;
  std::condition_variable hold_cv;
  bool release = false;
  std::atomic<int> holding{0};
  std::vector<std::thread> holders;
  for (int i = 0; i < 31; ++i) {
    holders.emplace_back([&] {
      bool first = true;
      (void)s.read_range("c2c2c2c2c2c2c2c2", "current.json", 0, UINT64_MAX, [&](std::uint64_t, std::span<const std::uint8_t>) {
        if (first) {
          first = false;
          ++holding;
          std::unique_lock lock(hold_m);
          hold_cv.wait(lock, [&] { return release; });
        }
        return true;
      }, nullptr, false);
    });
  }
  for (int i = 0; i < 500 && holding.load() < 31; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  REQUIRE(holding.load() == 31);
  auto waiter = std::async(std::launch::async, [&] {
    return s.read_range("c2c2c2c2c2c2c2c2", "current.json", 0, UINT64_MAX,
                        [](std::uint64_t, std::span<const std::uint8_t>) { return true; }, nullptr, false);
  });
  CHECK(waiter.wait_for(std::chrono::milliseconds(300)) == std::future_status::timeout);  // no stream is free
  s.shut_down();
  // shut_down alone must wake it: the 31 still hold their streams.
  const bool woke = waiter.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  {
    std::lock_guard lock(hold_m);
    release = true;  // released either way, so a regression fails here instead of hanging
  }
  hold_cv.notify_all();
  for (auto& t : holders) t.join();
  if (!woke) FAIL("shut_down did not wake a reader waiting for a stream");
  CHECK_FALSE(waiter.get());
}

TEST_CASE("link: Forget cuts reads on a client dropped after an error", "[mobile-link]") {
  rig r(6 * 1024 * 1024);
  r.pair_now();
  mob::link_session s(r.config());
  mob::route_report rep;
  REQUIRE(s.connect(rep));
  std::atomic<bool> started{false};
  std::atomic<std::uint64_t> got{0};
  mv::expected read = {};
  std::thread reader([&] {
    read = s.read_range("c2c2c2c2c2c2c2c2", "gen/000001/extra.bin", 0, UINT64_MAX, [&](std::uint64_t, std::span<const std::uint8_t> b) {
      started = true;
      got += b.size();
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      return true;
    }, nullptr, false);
  });
  while (!started) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  // An io error on another request drops the client from live_ while the read above still runs on it.
  s.drop_for_test();
  s.shut_down();
  reader.join();
  // Cut, not finished: what Forget did is visible in the result, not in how long it took.
  CHECK_FALSE(read);
  CHECK(got.load() < r.payload.size());
}

TEST_CASE("sync: bytes stop when they may not move, resume when they may, and a half-done install finishes", "[mobile-link]") {
  rig r(6 * 1024 * 1024);
  r.pair_now();
  mob::link_session s(r.config());
  mob::route_report rep;
  REQUIRE(s.connect(rep));
  auto cfg = r.sync_config();
  const fs::path library = cfg.library_dir;

  // Mobile data from the start: nothing moves.
  auto plan = mob::check_catalogue(s, cfg);
  REQUIRE(plan.outcome == mob::sync_outcome::copy_available);
  std::uint64_t moved = 0;
  auto out = mob::copy_catalogue(s, cfg, plan, [&](std::uint64_t d, std::uint64_t) { moved = d; }, nullptr, [] { return false; });
  CHECK(out == mob::sync_outcome::waiting);
  CHECK(moved == 0);

  // Wi-Fi goes away in the middle: it stops there, and nothing is installed.
  std::atomic<bool> on_wifi{true};
  plan = mob::check_catalogue(s, cfg);
  out = mob::copy_catalogue(s, cfg, plan, [&](std::uint64_t d, std::uint64_t) {
    moved = d;
    if (d > 1024 * 1024) on_wifi = false;
  }, nullptr, [&] { return on_wifi.load(); });
  CHECK(out == mob::sync_outcome::waiting);
  CHECK(moved < 3 * 1024 * 1024);
  CHECK_FALSE(fs::exists(library / "installed.json"));

  // Back on Wi-Fi: it picks up and installs.
  plan = mob::check_catalogue(s, cfg);
  CHECK(plan.done_bytes > 1024 * 1024);
  REQUIRE(mob::copy_catalogue(s, cfg, plan, nullptr, nullptr, [] { return true; }) == mob::sync_outcome::installed);

  // A crash after the folder's rename and before the record's: no second download.
  fs::remove(library / "installed.json");
  plan = mob::check_catalogue(s, cfg);
  REQUIRE(plan.outcome == mob::sync_outcome::copy_available);
  moved = 0;
  CHECK(mob::copy_catalogue(s, cfg, plan, [&](std::uint64_t d, std::uint64_t) { moved = std::max(moved, d - plan.done_bytes); },
                            nullptr) == mob::sync_outcome::installed);
  CHECK(moved == 0);
  CHECK(mob::read_installed(cfg.library_dir)->generation == 1);
}

namespace {
struct fake_packs final : mob::remote_file {
  std::atomic<int> reads{0};
  std::string pack = std::string(1 << 20, 'a');
  std::mutex m;
  std::condition_variable cv;
  bool gate_open = true;
  mv::result<std::vector<serve::root_info>> roots() override { return std::vector<serve::root_info>{}; }
  mv::result<std::vector<serve::list_entry>> list(const std::string&, const std::string&) override { return std::vector<serve::list_entry>{}; }
  mv::result<serve::stat_info> stat(const std::string&, const std::string&) override { return serve::stat_info{}; }
  mv::expected read_range(const std::string&, const std::string&, std::uint64_t at, std::uint64_t length,
                          const mob::byte_sink& sink, const mob::cancel_flag*, bool) override {
    {
      std::unique_lock lock(m);
      cv.wait(lock, [this] { return gate_open; });
    }
    reads.fetch_add(1);
    (void)sink(at, std::span(reinterpret_cast<const std::uint8_t*>(pack.data()) + at, length));
    return {};
  }
};
}  // namespace

TEST_CASE("thumb cache: neighbours merge into one read; a disk hit needs no network; LRU stays under its budget", "[mobile-link]") {
  scratch d;
  fake_packs remote;
  remote.gate_open = false;
  mob::thumb_cache cache((d.root / "thumbs").string(), 1 << 20, remote);

  const auto want = [&](std::uint64_t at, std::uint32_t len) {
    catalog::thumb_location w;
    w.pack = "t-000.pack";
    w.offset = at;
    w.length = len;
    return w;
  };
  std::mutex done_m;
  std::condition_variable done_cv;
  int done_count = 0;
  const auto record = [&](mv::result<std::vector<std::uint8_t>> r) {
    REQUIRE(r);
    std::lock_guard lock(done_m);
    ++done_count;
    done_cv.notify_all();
  };
  // Queued together before any worker may read (no catalogue root yet), so they merge into one.
  auto a = cache.request(want(0, 1000), 0, record);
  auto b = cache.request(want(1000, 1000), 0, record);
  auto c = cache.request(want(5000, 1000), 0, record);
  cache.set_catalogue_root("root");
  {
    std::lock_guard lock(remote.m);
    remote.gate_open = true;
  }
  remote.cv.notify_all();
  {
    std::unique_lock lock(done_m);
    REQUIRE(done_cv.wait_for(lock, std::chrono::seconds(5), [&] { return done_count == 3; }));
  }
  CHECK(remote.reads.load() == 1);
  const int reads_before = remote.reads.load();

  // On disk now: no network.
  auto again = cache.request(want(0, 1000), 0, record);
  CHECK(remote.reads.load() == reads_before);

  // A request cancelled while it waits hears so, once.
  {
    std::lock_guard lock(remote.m);
    remote.gate_open = false;
  }
  std::atomic<int> told{0};
  std::atomic<bool> cancelled{false};
  // Two in flight hold the workers at the gate; the third waits in the queue and is cancelled.
  (void)cache.request(want(900000, 1000), 0, [](auto) {});
  (void)cache.request(want(950000, 1000), 0, [](auto) {});
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  auto waiting = cache.request(want(700000, 1000), 1, [&](mv::result<std::vector<std::uint8_t>> r) {
    ++told;
    cancelled = !r && r.error() == mv::status::cancelled;
  });
  waiting->stop.store(true);
  {
    std::lock_guard lock(remote.m);
    remote.gate_open = true;
  }
  remote.cv.notify_all();
  for (int i = 0; i < 200 && told.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(told.load() == 1);
  CHECK(cancelled.load());
  // A want that could never be a tile fails alone.
  catalog::thumb_location bad = want(0, 1000);
  bad.pack = "../etc";
  bool refused = false;
  (void)cache.request(bad, 0, [&](mv::result<std::vector<std::uint8_t>> r) { refused = !r; });
  CHECK(refused);

  // Under its budget however much arrives.
  for (int i = 0; i < 400; ++i) {
    (void)cache.request(want(20000 + static_cast<std::uint64_t>(i) * 2000, 2000), 1, [](auto) {});
  }
  for (int i = 0; i < 200 && cache.bytes_used() < 256 * 1024; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(cache.bytes_used() <= (1 << 20));
}

// ---- the sample library (docs/plans/mobile.md §11.1): a static folder, the same sync ----------

TEST_CASE("sample: one fixed code and its link, nothing else", "[mobile-link]") {
  CHECK(mob::sample::is_code("SAMPLE"));
  CHECK(mob::sample::is_code("  sample\n"));
  CHECK(mob::sample::is_code("Sample"));
  CHECK(mob::sample::is_code("mediaviewer://sample"));
  CHECK(mob::sample::is_code("MediaViewer://Sample/"));
  CHECK_FALSE(mob::sample::is_code(""));
  CHECK_FALSE(mob::sample::is_code("SAMPLES"));
  CHECK_FALSE(mob::sample::is_code("sam ple"));
  CHECK_FALSE(mob::sample::is_code("mediaviewer://sample/x"));
  CHECK_FALSE(mob::sample::is_code("mediaviewer://pair#sample"));

  const auto p = mob::sample::payload();
  CHECK(catalog::to_hex(p.publisher_key) == mob::sample::kPublisherKey);  // the constant decodes
  CHECK(p.library_id == mob::sample::kLibraryID);
  CHECK(p.fingerprint.size() >= 8);
  CHECK(mob::sample::kBase.substr(0, 8) == "https://");
  CHECK(mob::sample::kBase.back() == '/');

  mv_pair_info info{};
  REQUIRE(mv_pair_parse("sample", 1, &info) == MV_LINK_OK);
  CHECK(info.sample == 1);
  CHECK(std::string(info.library_name) == "Sample library");
  CHECK(std::string(info.fingerprint) == p.fingerprint);
  CHECK(std::string(info.local).empty());
  CHECK(std::string(info.public_name).empty());
  // It never expires, and nothing is redeemed.
  REQUIRE(mv_pair_parse("mediaviewer://sample", INT64_MAX, &info) == MV_LINK_OK);
  const key32 device = random_key();
  CHECK(mv_pair_redeem(&info, device.data(), "iPhone", nullptr, nullptr) == MV_LINK_OK);
  // A wrong code is what it always was.
  CHECK(mv_pair_parse("SAMPLE-1", 1, &info) == MV_LINK_INVALID);
  CHECK(mv_pair_parse("hello", 1, &info) == MV_LINK_INVALID);
  CHECK(std::string(mv_sample_base()) == mob::sample::kBase);
}

TEST_CASE("sample: a path maps to one flat file name, or to none", "[mobile-link]") {
  using mob::sample::flat_name;
  CHECK(flat_name("c0c0c0c0c0c0c0c0", "current.json") == "c0c0c0c0c0c0c0c0__current.json");
  CHECK(flat_name("c0c0c0c0c0c0c0c0", "gen/000001/catalog.db") == "c0c0c0c0c0c0c0c0__gen__000001__catalog.db");
  CHECK(flat_name("a0a0a0a0a0a0a0a0", "Sample library/Dogs/dog-01.jpg") ==
        "a0a0a0a0a0a0a0a0__Sample_library__Dogs__dog-01.jpg");
  CHECK_FALSE(flat_name("c0c0c0c0c0c0c0c0", ""));
  CHECK_FALSE(flat_name("", "current.json"));
  CHECK_FALSE(flat_name("c0/c0", "current.json"));
  CHECK_FALSE(flat_name("c0c0c0c0c0c0c0c0", "../current.json"));
  CHECK_FALSE(flat_name("c0c0c0c0c0c0c0c0", "gen/./x"));
  CHECK_FALSE(flat_name("c0c0c0c0c0c0c0c0", "/current.json"));
  CHECK_FALSE(flat_name("c0c0c0c0c0c0c0c0", "gen//x"));
  CHECK_FALSE(flat_name("c0c0c0c0c0c0c0c0", "a\\b"));
  CHECK_FALSE(flat_name("c0c0c0c0c0c0c0c0", "caf\xc3\xa9.jpg"));
  CHECK_FALSE(flat_name("c0c0c0c0c0c0c0c0", "a?b"));
}

namespace {

constexpr const char* kSampleCatalogueRoot = "c0c0c0c0c0c0c0c0";
constexpr const char* kSampleMediaRoot = "a0a0a0a0a0a0a0a0";
constexpr const char* kTestBase = "https://static.example.test/sample/";
constexpr const char* kTestPack = "t-0123456789abcdef0123456789abcdef.pack";

// What tools/ios/sample-library.sh uploads, made from a publisher's destination and its photos:
// the flat files, and files.json signed by the publisher's key for everything sync does not check.
struct static_folder {
  fs::path dir;
  // Fetched from several threads at once (different files hold different locks).
  std::atomic<int> fetches{0};
  std::atomic<bool> down{false};
  std::mutex asked_m;
  std::vector<std::string> asked;      // under asked_m
  std::atomic<std::uint64_t> served{0};  // bytes handed to sinks, all fetches
  std::atomic<mob::cancel_flag*> trip{nullptr};  // fired once `served` reaches `trip_at`
  std::atomic<std::uint64_t> trip_at{0};

  std::vector<std::string> asked_now() {
    std::lock_guard lock(asked_m);
    return asked;
  }

  void fill(const fs::path& catalogue, const fs::path& photos, const std::string& share, const std::string& library_id,
            const std::vector<std::uint8_t>& secret) {
    fs::create_directories(dir);
    std::string files;
    const auto add = [&](const char* root, const fs::path& from) {
      for (const auto& e : fs::recursive_directory_iterator(from)) {
        if (!e.is_regular_file()) continue;
        const std::string rel = fs::relative(e.path(), from).generic_string();
        const auto name = mob::sample::flat_name(root, rel);
        REQUIRE(name);
        fs::copy_file(e.path(), dir / *name, fs::copy_options::overwrite_existing);
        const bool by_sync = std::string(root) == kSampleCatalogueRoot &&
                             (rel == "current.json" || rel.rfind("gen/", 0) == 0 || rel.rfind("models/", 0) == 0);
        if (by_sync) continue;
        const std::string bytes = read_all(e.path());
        if (!files.empty()) files += ",";
        files += "{\"name\":\"" + *name + "\",\"size\":" + std::to_string(bytes.size()) + ",\"sha256\":\"" +
                 sha256_hex(bytes) + "\"}";
      }
    };
    add(kSampleCatalogueRoot, catalogue);
    add(kSampleMediaRoot, photos);
    const std::string list = std::string("{\"format\":1,\"library_id\":\"") + library_id + "\",\"roots\":[{\"id\":\"" +
                             kSampleCatalogueRoot + "\",\"label\":\"Catalogue\"},{\"id\":\"" + kSampleMediaRoot +
                             "\",\"label\":\"" + share + "\"}],\"files\":[" + files + "]}";
    std::string msg(mob::sample::kFilesContext);
    msg += list;
    unsigned char sig[crypto_sign_BYTES];
    crypto_sign_detached(sig, nullptr, reinterpret_cast<const unsigned char*>(msg.data()), msg.size(), secret.data());
    std::string escaped;  // the list as a JSON string: it holds quotes, nothing else to escape
    for (char c : list) {
      if (c == '"' || c == '\\') escaped += '\\';
      escaped += c;
    }
    write_file(dir / "files.json", "{\"signature\":\"" + catalog::to_hex(std::span<const std::uint8_t>(sig, sizeof sig)) +
                                       "\",\"list\":\"" + escaped + "\"}");
  }

  // The host's GET: ranges (offsets from `at`), a 404 for what is not there, and the network gone when `down`.
  static int32_t fetch(void* ctx, const char* url, uint64_t at, uint64_t length, mv_sink_fn sink, void* sink_ctx,
                       mv_stopped_fn stopped, void* stopped_ctx) {
    auto* self = static_cast<static_folder*>(ctx);
    ++self->fetches;
    const std::string u(url);
    {
      std::lock_guard lock(self->asked_m);
      self->asked.push_back(u);
    }
    if (self->down) return MV_LINK_UNREACHABLE;
    if (u.rfind(kTestBase, 0) != 0) return MV_LINK_NOT_FOUND;
    std::ifstream f(self->dir / u.substr(std::strlen(kTestBase)), std::ios::binary);
    if (!f) return MV_LINK_NOT_FOUND;
    f.seekg(0, std::ios::end);
    const auto size = static_cast<std::uint64_t>(f.tellg());
    const std::uint64_t end = length == UINT64_MAX || at + length > size ? size : at + length;
    std::vector<char> buf(64 * 1024);
    for (std::uint64_t pos = at; pos < end;) {
      if (stopped(stopped_ctx)) return MV_LINK_CANCELLED;
      const std::uint64_t n = std::min<std::uint64_t>(buf.size(), end - pos);
      f.seekg(static_cast<std::streamoff>(pos));
      f.read(buf.data(), static_cast<std::streamsize>(n));
      if (sink(sink_ctx, pos - at, reinterpret_cast<const uint8_t*>(buf.data()), static_cast<size_t>(n)) == 0) {
        return MV_LINK_CANCELLED;
      }
      const std::uint64_t now = self->served += n;
      if (auto* t = self->trip.load(); t && now >= self->trip_at) t->stop = true;
      pos += n;
    }
    return MV_LINK_OK;
  }

  mob::sample::fetch_fn as_fetch() {
    return [this](const std::string& url, std::uint64_t at, std::uint64_t length, const mob::byte_sink& sink,
                  const std::function<bool()>&) -> mv::expected {
      struct k { const mob::byte_sink* s; } c{&sink};
      const int32_t r = fetch(this, url.c_str(), at, length,
                              [](void* p, uint64_t o, const uint8_t* d, size_t n) -> int32_t {
                                return (*static_cast<k*>(p)->s)(o, std::span<const std::uint8_t>(d, n)) ? 1 : 0;
                              },
                              &c, [](void*) -> int32_t { return 0; }, nullptr);
      if (r == MV_LINK_OK) return {};
      if (r == MV_LINK_CANCELLED) return mv::err(mv::status::cancelled);
      return mv::err(r == MV_LINK_NOT_FOUND ? mv::status::not_found : mv::status::io);
    };
  }
};

mv_mobile_link* open_static(const mob::sync_config& cfg, const std::string& cache, static_folder& folder,
                            const char* base = kTestBase) {
  mv_link_config c{};
  c.library_dir = cfg.library_dir.c_str();
  c.cache_dir = cache.c_str();
  c.library_id = cfg.library_id.c_str();
  c.publisher_key = cfg.publisher_key.data();
  c.static_base = base;
  c.fetch = &static_folder::fetch;
  c.fetch_ctx = &folder;
  return mv_link_open(&c);
}

// One sample folder from a real deliver(): a photo, a JPEG-512 pack, a 2 MB extra file to copy.
struct sample_rig {
  scratch dir;
  fs::path photos = dir.root / "photos";
  publisher pub{dir.root / "nas", dir.root / "work"};
  static_folder folder;
  mob::sync_config cfg;
  std::string cache = (dir.root / "thumbs").string();
  std::string pack_bytes;

  sample_rig() {
    REQUIRE(sodium_init() >= 0);
    write_file(photos / "2024" / "pic.bin", std::string(70000, 'p'));
    for (int i = 0; i < 4000; ++i) pack_bytes += static_cast<char>('a' + i % 26);
    // thumbs.idx: asset 1's JPEG-512 is 200 bytes at 1000 in the pack (catalog.cpp's layout).
    std::string idx = "MVTIDX01";
    const auto put = [&idx](std::uint64_t v, int bytes) {
      for (int b = 0; b < bytes; ++b) idx += static_cast<char>(v >> (8 * b) & 0xff);
    };
    put(1, 4);  // packs
    put(1, 4);  // rows
    std::string name(kTestPack);
    name.resize(40, '\0');
    idx += name;
    put(1, 8);     // asset
    put(0, 4);     // pack
    put(200, 4);   // length
    put(1000, 8);  // offset
    pub.gen_files.emplace_back("thumbs.idx", idx);
    std::string big(8 * 1024 * 1024, '\0');
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 7 % 251);
    write_file(photos / "2024" / "clip.bin", big);
    pub.publish(1, std::string(2 * 1024 * 1024, 'x'));
    write_file(dir.root / "nas" / ".mediaviewer" / "thumbs" / kTestPack, pack_bytes);
    folder.dir = dir.root / "static";
    folder.fill(dir.root / "nas" / ".mediaviewer", photos, "Photos", pub.library_id, pub.secret);
    cfg.library_dir = (dir.root / "phone").string();
    cfg.library_id = pub.library_id;
    std::copy(pub.pub.begin(), pub.pub.end(), cfg.publisher_key.begin());
  }

  // A file in the static folder, by its path under the publisher's destination or the photos.
  fs::path flat(const char* root, const std::string& rel) { return folder.dir / *mob::sample::flat_name(root, rel); }

  mob::sample::static_remote remote(std::uint64_t budget = mob::sample::static_remote::kVerifiedBudget) {
    return mob::sample::static_remote(kTestBase, folder.as_fetch(), cfg.publisher_key, cfg.library_id,
                                      (dir.root / "verified").string(), budget);
  }
};

const mv_sink_fn collect = [](void* ctx, uint64_t, const uint8_t* data, size_t size) -> int32_t {
  static_cast<std::string*>(ctx)->append(reinterpret_cast<const char*>(data), size);
  return 1;
};

}  // namespace

TEST_CASE("sample: a static folder installs through the same sync, and serves originals", "[mobile-link]") {
  sample_rig r;
  std::string got;

  SECTION("connect, check, copy, install, then an original by range") {
    mv_mobile_link* l = open_static(r.cfg, r.cache, r.folder);
    REQUIRE(l);
    mv_route_report report{};
    REQUIRE(mv_link_connect(l, nullptr, &report) == MV_LINK_OK);
    CHECK(std::string(report.address) == "static.example.test");
    CHECK(report.over_internet == 1);
    CHECK(mv_link_is_connected(l) == 1);
    mv_sync_report check{};
    CHECK(mv_sync_check(l, &check) == MV_LINK_COPY_AVAILABLE);
    CHECK(check.generation == 1);
    mv_sync_report sync{};
    REQUIRE(mv_sync_copy(l, nullptr, nullptr, nullptr, nullptr, &sync) == MV_LINK_OK);
    const auto installed = mob::read_installed(r.cfg.library_dir);
    REQUIRE(installed);
    CHECK(installed->generation == 1);
    CHECK(installed->catalogue_root == kSampleCatalogueRoot);
    CHECK(mv_sync_check(l, &check) == MV_LINK_UP_TO_DATE);
    mv_catalogue_info info{};
    REQUIRE(mv_catalogue_read(l, &info) == MV_LINK_OK);
    CHECK(info.present == 1);
    CHECK(mv_asset_folder_check(l, 1) == MV_LINK_OK);
    CHECK(mv_file_read(l, 1, 0, UINT64_MAX, 0, nullptr, collect, &got) == MV_LINK_OK);
    CHECK(got == std::string(70000, 'p'));
    got.clear();
    CHECK(mv_file_read(l, 1, 100, 10, 0, nullptr, collect, &got) == MV_LINK_OK);
    CHECK(got == std::string(10, 'p'));
    // Every request stayed inside the folder it was given.
    for (const auto& u : r.folder.asked_now()) CHECK(u.rfind(kTestBase, 0) == 0);

    // Forget: reads stop and nothing connects again.
    mv_link_shut_down(l);
    got.clear();
    CHECK(mv_file_read(l, 1, 0, UINT64_MAX, 0, nullptr, collect, &got) != MV_LINK_OK);
    CHECK(mv_link_connect(l, nullptr, &report) == MV_LINK_UNREACHABLE);
    mv_link_close(l);
    mv_library_remove(r.cfg.library_dir.c_str());
    CHECK_FALSE(mob::read_installed(r.cfg.library_dir));
  }
  SECTION("a copy stopped part way resumes from where it got to, and installs") {
    mv_mobile_link* l = open_static(r.cfg, r.cache, r.folder);
    REQUIRE(l);
    int asks = 0;
    const mv_sync_allowed_fn allowed = [](void* ctx) -> int32_t { return ++*static_cast<int*>(ctx) < 6 ? 1 : 0; };
    mv_sync_report first{};
    CHECK(mv_sync_copy(l, nullptr, allowed, nullptr, &asks, &first) == MV_LINK_WAITING);
    CHECK_FALSE(mob::read_installed(r.cfg.library_dir));
    mv_sync_report check{};
    REQUIRE(mv_sync_check(l, &check) == MV_LINK_COPY_AVAILABLE);
    CHECK(check.done_bytes > 0);  // what the stopped copy already holds
    CHECK(check.done_bytes < check.total_bytes);
    mv_sync_report second{};
    REQUIRE(mv_sync_copy(l, nullptr, nullptr, nullptr, nullptr, &second) == MV_LINK_OK);
    const auto installed = mob::read_installed(r.cfg.library_dir);
    REQUIRE(installed);
    CHECK(installed->generation == 1);
    mv_link_close(l);
  }
  SECTION("a catalogue the pinned key did not sign is refused, and nothing is installed") {
    const auto other = catalog::make_keypair();
    r.pub.publish(2, std::string(300000, 'y'), &other.second);
    r.folder.fill(r.dir.root / "nas" / ".mediaviewer", r.photos, "Photos", r.pub.library_id, r.pub.secret);
    mv_mobile_link* l = open_static(r.cfg, r.cache, r.folder);
    REQUIRE(l);
    mv_route_report report{};
    REQUIRE(mv_link_connect(l, nullptr, &report) == MV_LINK_OK);
    mv_sync_report sync{};
    CHECK(mv_sync_copy(l, nullptr, nullptr, nullptr, nullptr, &sync) == MV_LINK_REJECTED);
    CHECK_FALSE(mob::read_installed(r.cfg.library_dir));
    mv_link_close(l);
  }
  SECTION("a changed catalogue file is refused by its hash") {
    write_file(r.flat(kSampleCatalogueRoot, "gen/000001/extra.bin"), std::string(2 * 1024 * 1024 + 1, 'z'));
    mv_mobile_link* l = open_static(r.cfg, r.cache, r.folder);
    REQUIRE(l);
    mv_sync_report sync{};
    CHECK(mv_sync_copy(l, nullptr, nullptr, nullptr, nullptr, &sync) == MV_LINK_REJECTED);
    CHECK_FALSE(mob::read_installed(r.cfg.library_dir));
    mv_link_close(l);
  }
  SECTION("a list not signed by the pinned key: rejected, as a bad catalogue is, and nothing is read") {
    const auto other = catalog::make_keypair();
    r.folder.fill(r.dir.root / "nas" / ".mediaviewer", r.photos, "Photos", r.pub.library_id, other.second);
    mv_mobile_link* l = open_static(r.cfg, r.cache, r.folder);
    REQUIRE(l);
    mv_route_report report{};
    CHECK(mv_link_connect(l, nullptr, &report) == MV_LINK_REJECTED);
    mv_sync_report check{};
    CHECK(mv_sync_check(l, &check) == MV_LINK_REJECTED);
    mv_sync_report sync{};
    CHECK(mv_sync_copy(l, nullptr, nullptr, nullptr, nullptr, &sync) == MV_LINK_REJECTED);
    CHECK(sync.outcome == MV_LINK_REJECTED);
    CHECK_FALSE(mob::read_installed(r.cfg.library_dir));
    mv_link_close(l);
  }
  SECTION("no network: unreachable, named by its host") {
    r.folder.down = true;
    mv_mobile_link* l = open_static(r.cfg, r.cache, r.folder);
    REQUIRE(l);
    mv_route_report report{};
    CHECK(mv_link_connect(l, nullptr, &report) == MV_LINK_UNREACHABLE);
    REQUIRE(report.failure_count == 1);
    CHECK(std::string(report.failures[0].address) == "static.example.test");
    mv_sync_report sync{};
    CHECK(mv_sync_copy(l, nullptr, nullptr, nullptr, nullptr, &sync) == MV_LINK_UNREACHABLE);
    mv_link_close(l);
  }
  SECTION("a folder with no catalogue in it: unreachable, not installed") {
    mv_mobile_link* l = open_static(r.cfg, r.cache, r.folder, "https://static.example.test/elsewhere/");
    REQUIRE(l);
    mv_route_report report{};
    CHECK(mv_link_connect(l, nullptr, &report) == MV_LINK_UNREACHABLE);
    mv_sync_report sync{};
    CHECK(mv_sync_copy(l, nullptr, nullptr, nullptr, nullptr, &sync) != MV_LINK_OK);
    CHECK_FALSE(mob::read_installed(r.cfg.library_dir));
    mv_link_close(l);
  }
}

TEST_CASE("sample: a read from the middle of a file hands over file offsets", "[mobile-link]") {
  sample_rig r;
  auto remote = r.remote();
  std::vector<std::uint64_t> offsets;
  std::string got;
  const mob::byte_sink keep = [&](std::uint64_t offset, std::span<const std::uint8_t> b) {
    offsets.push_back(offset);
    got.append(reinterpret_cast<const char*>(b.data()), b.size());
    return true;
  };
  // A file sync checks (passed through by range) and a listed one (from its verified copy).
  REQUIRE(remote.read_range(kSampleCatalogueRoot, "gen/000001/extra.bin", 4096, 100, keep, nullptr, false));
  REQUIRE_FALSE(offsets.empty());
  CHECK(offsets.front() == 4096);
  CHECK(got == std::string(100, 'x'));
  offsets.clear();
  got.clear();
  REQUIRE(remote.read_range(kSampleMediaRoot, "2024/pic.bin", 4096, 100, keep, nullptr, false));
  REQUIRE_FALSE(offsets.empty());
  CHECK(offsets.front() == 4096);
  CHECK(got == std::string(100, 'p'));
}

TEST_CASE("sample: a JPEG-512 batch from the middle of a pack arrives", "[mobile-link]") {
  sample_rig r;
  auto remote = r.remote();
  mob::thumb_cache thumbs((r.dir.root / "tiles").string(), 1 << 20, remote);
  thumbs.set_catalogue_root(kSampleCatalogueRoot);
  std::promise<mv::result<std::vector<std::uint8_t>>> done;
  catalog::thumb_location where{kTestPack, 1000, 200};
  auto flag = thumbs.request(where, 0, [&](mv::result<std::vector<std::uint8_t>> got) { done.set_value(std::move(got)); });
  auto got = done.get_future().get();
  REQUIRE(got);
  CHECK(std::string(got->begin(), got->end()) == r.pack_bytes.substr(1000, 200));
}

TEST_CASE("sample: a swapped original or tile pack is refused, and nothing of it is handed over", "[mobile-link]") {
  sample_rig r;
  std::string got;
  SECTION("an original") {
    write_file(r.flat(kSampleMediaRoot, "2024/pic.bin"), std::string(70000, 'q'));  // same size, other bytes
    mv_mobile_link* l = open_static(r.cfg, r.cache, r.folder);
    REQUIRE(l);
    mv_sync_report sync{};
    REQUIRE(mv_sync_copy(l, nullptr, nullptr, nullptr, nullptr, &sync) == MV_LINK_OK);
    CHECK(mv_file_read(l, 1, 0, UINT64_MAX, 0, nullptr, collect, &got) != MV_LINK_OK);
    CHECK(got.empty());
    CHECK(mv_file_read(l, 1, 100, 10, 0, nullptr, collect, &got) != MV_LINK_OK);
    CHECK(got.empty());
    mv_link_close(l);
  }
  SECTION("a tile pack") {
    std::string swapped = r.pack_bytes;
    swapped[1050] = '#';
    write_file(r.flat(kSampleCatalogueRoot, std::string("thumbs/") + kTestPack), swapped);
    auto remote = r.remote();
    mob::thumb_cache thumbs((r.dir.root / "tiles").string(), 1 << 20, remote);
    thumbs.set_catalogue_root(kSampleCatalogueRoot);
    std::promise<mv::result<std::vector<std::uint8_t>>> done;
    auto flag = thumbs.request(catalog::thumb_location{kTestPack, 1000, 200}, 0,
                               [&](mv::result<std::vector<std::uint8_t>> g) { done.set_value(std::move(g)); });
    CHECK_FALSE(done.get_future().get());
  }
  SECTION("a file the list does not name is never fetched") {
    write_file(r.folder.dir / *mob::sample::flat_name(kSampleMediaRoot, "2024/other.bin"), "anything");
    auto remote = r.remote();
    const int before = r.folder.fetches;
    auto read = remote.read_range(kSampleMediaRoot, "2024/other.bin", 0, UINT64_MAX,
                                  [&](std::uint64_t, std::span<const std::uint8_t>) { return true; }, nullptr, false);
    REQUIRE_FALSE(read);
    CHECK(read.error() == mv::status::not_found);
    const auto asked = r.folder.asked_now();
    for (std::size_t i = static_cast<std::size_t>(before); i < asked.size(); ++i) CHECK(asked[i].find("other.bin") == std::string::npos);
  }
}

TEST_CASE("sample: a whole-file download a reader cancels is resumed by the next, not begun again", "[mobile-link]") {
  sample_rig r;
  auto remote = r.remote();
  const std::string want = read_all(r.photos / "2024" / "clip.bin");
  mob::cancel_flag stop;
  r.folder.trip = &stop;
  r.folder.trip_at = r.folder.served + 6 * 1024 * 1024;  // a seek cancels the read at 6 of 8 MB
  std::string got;
  const mob::byte_sink keep = [&](std::uint64_t, std::span<const std::uint8_t> b) {
    got.append(reinterpret_cast<const char*>(b.data()), b.size());
    return true;
  };
  auto first = remote.read_range(kSampleMediaRoot, "2024/clip.bin", 0, 4096, keep, &stop, false);
  REQUIRE_FALSE(first);
  CHECK(first.error() == mv::status::cancelled);
  CHECK(got.empty());  // nothing unverified is handed over
  r.folder.trip = nullptr;
  const std::uint64_t before = r.folder.served.load();
  REQUIRE(remote.read_range(kSampleMediaRoot, "2024/clip.bin", 5000000, 1000, keep, nullptr, false));
  CHECK(r.folder.served - before == 2 * 1024 * 1024);  // only the rest came over the network
  CHECK(got == want.substr(5000000, 1000));
  // And from then on it is read from the phone: no more bytes from the network.
  const std::uint64_t after = r.folder.served.load();
  got.clear();
  REQUIRE(remote.read_range(kSampleMediaRoot, "2024/clip.bin", 0, UINT64_MAX, keep, nullptr, false));
  CHECK(got == want);
  CHECK(r.folder.served == after);
}

TEST_CASE("sample: a reader waiting for another's download stops on its own cancel", "[mobile-link]") {
  sample_rig r;
  std::atomic<bool> release{false}, started{false};
  auto inner = r.folder.as_fetch();
  // The first reader's download hangs in the network until released.
  mob::sample::fetch_fn slow = [&](const std::string& url, std::uint64_t at, std::uint64_t length,
                                   const mob::byte_sink& sink, const std::function<bool()>& cancel) -> mv::expected {
    if (url.find("clip.bin") != std::string::npos && !release) {
      started = true;
      while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return inner(url, at, length, sink, cancel);
  };
  mob::sample::static_remote remote(kTestBase, slow, r.cfg.publisher_key, r.cfg.library_id,
                                    (r.dir.root / "verified").string());
  const mob::byte_sink drop = [](std::uint64_t, std::span<const std::uint8_t>) { return true; };
  std::thread first([&] { (void)remote.read_range(kSampleMediaRoot, "2024/clip.bin", 0, 10, drop, nullptr, false); });
  for (int i = 0; i < 2000 && !started; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  if (!started) {
    release = true;
    first.join();
    FAIL("the first reader never reached the network");
  }
  mob::cancel_flag mine;
  mine.stop = true;
  const auto t0 = std::chrono::steady_clock::now();
  auto second = remote.read_range(kSampleMediaRoot, "2024/clip.bin", 0, 10, drop, &mine, false);
  const auto waited = std::chrono::steady_clock::now() - t0;
  release = true;
  first.join();
  REQUIRE_FALSE(second);
  CHECK(second.error() == mv::status::cancelled);
  CHECK(waited < std::chrono::seconds(2));  // gave up on its own cancel, not after the other's download
}

TEST_CASE("sample: verified copies stay under their budget and count in the cache size", "[mobile-link]") {
  sample_rig r;
  {
    auto remote = r.remote(9 * 1024 * 1024);  // room for the 8 MB clip or the photo, not both and more
    const mob::byte_sink drop = [](std::uint64_t, std::span<const std::uint8_t>) { return true; };
    REQUIRE(remote.read_range(kSampleMediaRoot, "2024/pic.bin", 0, 10, drop, nullptr, false));
    REQUIRE(remote.read_range(kSampleMediaRoot, "2024/clip.bin", 0, 10, drop, nullptr, false));
    CHECK(remote.verified_bytes() <= 9 * 1024 * 1024);
    CHECK(remote.verified_bytes() >= 8 * 1024 * 1024);  // the one just read stays
  }
  // Through the C ABI: the cache size the app shows includes them.
  mv_mobile_link* l = open_static(r.cfg, r.cache, r.folder);
  REQUIRE(l);
  mv_sync_report sync{};
  REQUIRE(mv_sync_copy(l, nullptr, nullptr, nullptr, nullptr, &sync) == MV_LINK_OK);
  const std::uint64_t before = mv_thumb_bytes_used(l);
  std::string got;
  REQUIRE(mv_file_read(l, 1, 0, 10, 0, nullptr, collect, &got) == MV_LINK_OK);
  CHECK(mv_thumb_bytes_used(l) >= before + 70000);
  mv_link_close(l);
}

TEST_CASE("sample: a file republished with the same name and size is fetched again, not served from the old copy", "[mobile-link]") {
  sample_rig r;
  const mob::byte_sink drop = [](std::uint64_t, std::span<const std::uint8_t>) { return true; };
  std::string got;
  const mob::byte_sink keep = [&](std::uint64_t, std::span<const std::uint8_t> b) {
    got.append(reinterpret_cast<const char*>(b.data()), b.size());
    return true;
  };
  {
    auto remote = r.remote();
    REQUIRE(remote.read_range(kSampleMediaRoot, "2024/pic.bin", 0, 10, drop, nullptr, false));
  }
  // The publisher replaces it (--clobber): same name, same size, other bytes, a new signed list.
  write_file(r.photos / "2024" / "pic.bin", std::string(70000, 'n'));
  r.folder.fill(r.dir.root / "nas" / ".mediaviewer", r.photos, "Photos", r.pub.library_id, r.pub.secret);
  auto remote = r.remote();  // the same verified folder, the new list
  REQUIRE(remote.read_range(kSampleMediaRoot, "2024/pic.bin", 0, 10, keep, nullptr, false));
  CHECK(got == std::string(10, 'n'));
}

TEST_CASE("sample: a transport that sends more than the list says is refused and nothing of it is kept", "[mobile-link]") {
  sample_rig r;
  write_file(r.flat(kSampleMediaRoot, "2024/pic.bin"), std::string(70001, 'p'));  // after signing
  auto inner = r.folder.as_fetch();
  // A host that ignores the length asked for and sends the whole file.
  mob::sample::fetch_fn sloppy = [&](const std::string& url, std::uint64_t at, std::uint64_t, const mob::byte_sink& sink,
                                     const std::function<bool()>& cancel) { return inner(url, at, UINT64_MAX, sink, cancel); };
  mob::sample::static_remote remote(kTestBase, sloppy, r.cfg.publisher_key, r.cfg.library_id,
                                    (r.dir.root / "verified").string());
  auto read = remote.read_range(kSampleMediaRoot, "2024/pic.bin", 0, 10,
                                [](std::uint64_t, std::span<const std::uint8_t>) { return true; }, nullptr, false);
  REQUIRE_FALSE(read);
  CHECK(read.error() == mv::status::corrupt);
  std::size_t kept = 0;
  std::error_code ec;
  for (fs::directory_iterator it(r.dir.root / "verified", ec), end; !ec && it != end; it.increment(ec)) ++kept;
  CHECK(kept == 0);
}

TEST_CASE("sample: after Forget a reader makes nothing on disk", "[mobile-link]") {
  sample_rig r;
  auto remote = r.remote();
  REQUIRE(remote.connect(nullptr));
  remote.shut_down();
  auto read = remote.read_range(kSampleMediaRoot, "2024/pic.bin", 0, 10,
                                [](std::uint64_t, std::span<const std::uint8_t>) { return true; }, nullptr, false);
  CHECK_FALSE(read);
  CHECK_FALSE(fs::exists(r.dir.root / "verified"));
}

TEST_CASE("sample: Forget waits for the readers in flight, and nothing of theirs is left after the removal", "[mobile-link]") {
  sample_rig r;
  auto inner = r.folder.as_fetch();
  for (int round = 0; round < 20; ++round) {
    const fs::path dir = r.dir.root / ("verified-" + std::to_string(round));
    // A slow line: each chunk takes a moment, so shut_down lands in the middle of downloads.
    mob::sample::fetch_fn slow = [&](const std::string& url, std::uint64_t at, std::uint64_t length,
                                     const mob::byte_sink& sink, const std::function<bool()>& stop) -> mv::expected {
      return inner(url, at, length,
                   [&](std::uint64_t o, std::span<const std::uint8_t> b) {
                     std::this_thread::sleep_for(std::chrono::microseconds(200));
                     return !(stop && stop()) && sink(o, b);
                   },
                   stop);
    };
    mob::sample::static_remote remote(kTestBase, slow, r.cfg.publisher_key, r.cfg.library_id, dir.string());
    REQUIRE(remote.connect(nullptr));
    std::vector<std::thread> readers;
    for (const char* rel : {"2024/clip.bin", "2024/pic.bin", "2024/clip.bin", "2024/pic.bin"}) {
      readers.emplace_back([&remote, rel] {
        (void)remote.read_range(kSampleMediaRoot, rel, 0, 10,
                                [](std::uint64_t, std::span<const std::uint8_t>) { return true; }, nullptr, false);
      });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(round % 5 * 3));
    remote.shut_down();  // Forget: stop and wait
    std::error_code ec;
    fs::remove_all(dir, ec);  // what LinkConnection.removeLibrary does next
    for (auto& t : readers) t.join();
    CHECK_FALSE(fs::exists(dir));
    // And a reader that comes after does nothing to disk.
    CHECK_FALSE(remote.read_range(kSampleMediaRoot, "2024/pic.bin", 0, 10,
                                  [](std::uint64_t, std::span<const std::uint8_t>) { return true; }, nullptr, false));
    CHECK_FALSE(fs::exists(dir));
  }
}

TEST_CASE("sample: Forget does not return while a reader is still inside its download", "[mobile-link]") {
  sample_rig r;
  auto inner = r.folder.as_fetch();
  std::atomic<int> parked{0}, finishing{0};
  // Each download stops in its first chunk and stays there until it sees Forget.
  mob::sample::fetch_fn parking = [&](const std::string& url, std::uint64_t at, std::uint64_t length,
                                      const mob::byte_sink& sink, const std::function<bool()>& stop) -> mv::expected {
    if (url.find("files.json") != std::string::npos) return inner(url, at, length, sink, stop);
    bool first = true;
    return inner(url, at, length,
                 [&](std::uint64_t o, std::span<const std::uint8_t> b) {
                   if (first) {
                     first = false;
                     ++parked;
                     while (!stop()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                     std::this_thread::sleep_for(std::chrono::milliseconds(50));  // slow to notice: still in flight
                     ++finishing;  // counted inside the read, before it leaves: shut_down must wait past this
                     return false;
                   }
                   return sink(o, b);
                 },
                 stop);
  };
  mob::sample::static_remote remote(kTestBase, parking, r.cfg.publisher_key, r.cfg.library_id,
                                    (r.dir.root / "verified").string());
  REQUIRE(remote.connect(nullptr));
  std::vector<std::thread> readers;
  // However the test ends, the readers are released and joined: a thread left joinable would
  // terminate the whole binary.
  struct cleanup {
    mob::sample::static_remote& remote;
    std::vector<std::thread>& threads;
    ~cleanup() {
      remote.shut_down();
      for (auto& t : threads) {
        if (t.joinable()) t.join();
      }
    }
  } guard{remote, readers};
  for (const char* rel : {"2024/clip.bin", "2024/pic.bin"}) {
    readers.emplace_back([&remote, rel] {
      (void)remote.read_range(kSampleMediaRoot, rel, 0, 10, [](std::uint64_t, std::span<const std::uint8_t>) { return true; },
                              nullptr, false);
    });
  }
  for (int i = 0; i < 2000 && parked < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  REQUIRE(parked == 2);
  remote.shut_down();
  CHECK(finishing == 2);  // it waited for both: neither was still inside its download when it returned
}

TEST_CASE("thumb cache: after shut_down no tile is written, even from a read already in flight", "[mobile-link]") {
  scratch dir;
  struct held final : mob::remote_file {
    std::atomic<bool> release{false}, reading{false};
    mv::result<std::vector<mv::driftwire::serve::root_info>> roots() override { return mv::err(mv::status::io); }
    mv::result<std::vector<mv::driftwire::serve::list_entry>> list(const std::string&, const std::string&) override {
      return mv::err(mv::status::io);
    }
    mv::result<mv::driftwire::serve::stat_info> stat(const std::string&, const std::string&) override { return mv::err(mv::status::io); }
    mv::expected read_range(const std::string&, const std::string&, std::uint64_t at, std::uint64_t length,
                            const mob::byte_sink& sink, const mob::cancel_flag*, bool) override {
      reading = true;
      while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length), 7);
      return sink(at, bytes) ? mv::expected{} : mv::err(mv::status::cancelled);
    }
  } remote;
  const fs::path tiles = dir.root / "tiles";
  {
    mob::thumb_cache thumbs(tiles.string(), 1 << 20, remote);
    thumbs.set_catalogue_root("c0c0c0c0c0c0c0c0");
    std::promise<void> done;
    auto flag = thumbs.request(catalog::thumb_location{kTestPack, 100, 50}, 0, [&](auto) { done.set_value(); });
    while (!remote.reading) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    thumbs.shut_down();  // Forget, with the read still out
    // The folder is left in place here, so any write at all would show: removeItem lists, then
    // deletes, and a write between the two is the one that outlives Forget.
    remote.release = true;  // the read comes back after shut_down returned
    done.get_future().wait();
  }
  std::size_t files = 0;
  std::error_code ec;
  for (fs::directory_iterator it(tiles, ec), end; !ec && it != end; it.increment(ec)) ++files;
  CHECK(files == 0);
}

TEST_CASE("sample: Forget through the C ABI stops a tile write whose read has already come back", "[mobile-link]") {
  sample_rig r;
  mv_mobile_link* l = open_static(r.cfg, r.cache, r.folder);
  REQUIRE(l);
  mv_sync_report sync{};
  REQUIRE(mv_sync_copy(l, nullptr, nullptr, nullptr, nullptr, &sync) == MV_LINK_OK);
  // Declared before the guard, so they outlive the worker the guard joins.
  std::atomic<bool> parked{false}, release{false};
  std::promise<int32_t> done;
  mv_cancel* cancel = nullptr;
  // However the test ends: the worker is let go, the link is shut and closed (which joins the
  // worker, so nothing it captured is used after this scope), and the hook is cleared.
  struct teardown {
    std::atomic<bool>& release;
    mv_mobile_link*& link;
    mv_cancel*& cancel;
    ~teardown() {
      release = true;
      if (link) {
        mv_link_shut_down(link);
        mv_link_close(link);
      }
      mv_cancel_free(cancel);
      mob::testing::set_tile_write_hook({});
    }
  } guard{release, l, cancel};
  mob::testing::set_tile_write_hook([&] {
    parked = true;
    while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  });
  REQUIRE(mv_thumb_request(l, 1, 0, [](void* ctx, int32_t status, const uint8_t*, size_t) {
            static_cast<std::promise<int32_t>*>(ctx)->set_value(status);
          }, &done, &cancel) == MV_LINK_OK);
  for (int i = 0; i < 5000 && !parked; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  REQUIRE(parked);  // the pack read is back; its tile is not written yet
  mv_link_shut_down(l);  // Forget
  release = true;        // the worker goes on to write, after Forget
  CHECK(done.get_future().get() == MV_LINK_OK);  // the caller still gets its bytes
  std::size_t tiles = 0;
  std::error_code ec;
  for (fs::directory_iterator it(r.cache, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file()) ++tiles;
  }
  CHECK(tiles == 0);  // nothing written into the folder Forget removes next
}

// ---- the sound text tower's download (docs/plans/mobile.md §9, "Sound search") ------------------

namespace {

std::string read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

std::string sha_hex(std::string_view bytes) {
  unsigned char h[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(h, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
  return catalog::to_hex(std::span<const std::uint8_t>(h, sizeof h));
}

// A release in memory: the files by URL, every request seen, and a way to make one fail.
struct fake_release {
  std::map<std::string, std::string> files;
  std::vector<std::pair<std::string, uint64_t>> asked;  // url, from
  int32_t answer = MV_LINK_OK;
  std::size_t cut_after = SIZE_MAX;  // stop delivering after this many bytes in all (a dropped line)
  std::size_t sent = 0;
};

int32_t fake_fetch(void* ctx, const char* url, uint64_t at, uint64_t length, mv_sink_fn sink, void* sink_ctx,
                   mv_stopped_fn stopped, void* stopped_ctx) {
  auto* r = static_cast<fake_release*>(ctx);
  r->asked.emplace_back(url, at);
  if (r->answer != MV_LINK_OK) return r->answer;
  const auto it = r->files.find(url);
  if (it == r->files.end()) return MV_LINK_NOT_FOUND;
  const std::string& body = it->second;
  const uint64_t end = length == UINT64_MAX ? body.size() : std::min<uint64_t>(body.size(), at + length);
  for (uint64_t i = at; i < end; i += 3) {  // small chunks, so `allowed` is asked often
    if (stopped && stopped(stopped_ctx)) return MV_LINK_CANCELLED;
    if (r->sent >= r->cut_after) return MV_LINK_UNREACHABLE;
    const std::size_t n = static_cast<std::size_t>(std::min<uint64_t>(3, end - i));
    if (!sink(sink_ctx, i - at, reinterpret_cast<const uint8_t*>(body.data() + i), n)) return MV_LINK_CANCELLED;
    r->sent += n;
  }
  return MV_LINK_OK;
}

struct sound_fixture {
  fs::path root = fs::temp_directory_path() / ("mv-sound-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
  std::string a = "model file A, the first", b = "tokens: the second file, a little longer";
  std::string sa = sha_hex(a), sb = sha_hex(b);  // the pin's views point here
  std::array<mob::sound::pinned_file, 2> pin{};
  fake_release release;
  sound_fixture() {
    fs::remove_all(root);
    fs::create_directories(root);
    pin = {{{"a.bin", sa, a.size()}, {"b.txt", sb, b.size()}}};
    release.files["https://example.invalid/r/a.bin"] = a;
    release.files["https://example.invalid/r/b.txt"] = b;
  }
  ~sound_fixture() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  std::string library() const { return root.string(); }
  fs::path dir() const { return fs::path(mob::sound::folder(library())); }
  int32_t fetch(mv_sync_allowed_fn allowed = nullptr, void* ctx = nullptr, const mob::cancel_flag* cancel = nullptr) {
    return mob::sound::fetch_all(library(), "https://example.invalid/r/", &fake_fetch, &release, cancel, allowed, nullptr, ctx,
                                 pin);
  }
};

int32_t never(void*) { return 0; }

}  // namespace

TEST_CASE("sound tower: downloaded, hashed and recorded; a second run asks for nothing", "[mobile-link]") {
  sound_fixture f;
  REQUIRE(f.fetch() == MV_LINK_OK);
  CHECK(mob::sound::ready(f.dir().string(), f.pin));
  CHECK(read_text(f.dir() / "a.bin") == f.a);
  CHECK(read_text(f.dir() / "b.txt") == f.b);
  CHECK_FALSE(fs::exists(f.dir() / "a.bin.part"));
  CHECK(mob::sound::bytes_on_disk(f.library()) >= f.a.size() + f.b.size());
  f.release.asked.clear();
  REQUIRE(f.fetch() == MV_LINK_OK);
  CHECK(f.release.asked.empty());
}

TEST_CASE("sound tower: a dropped download resumes where it stopped", "[mobile-link]") {
  sound_fixture f;
  f.release.cut_after = f.a.size() + 7;  // all of a, then part of b
  CHECK(f.fetch() == MV_LINK_UNREACHABLE);
  CHECK(fs::exists(f.dir() / "a.bin"));
  REQUIRE(fs::exists(f.dir() / "b.txt.part"));
  const auto have = fs::file_size(f.dir() / "b.txt.part");
  CHECK(have > 0);
  f.release.cut_after = SIZE_MAX;
  f.release.asked.clear();
  REQUIRE(f.fetch() == MV_LINK_OK);
  REQUIRE(f.release.asked.size() == 1);
  CHECK(f.release.asked[0].first == "https://example.invalid/r/b.txt");
  CHECK(f.release.asked[0].second == have);
  CHECK(read_text(f.dir() / "b.txt") == f.b);
}

TEST_CASE("sound tower: a file that is not the pinned one is deleted, never kept or resumed", "[mobile-link]") {
  sound_fixture f;
  f.release.files["https://example.invalid/r/b.txt"] = std::string(f.b.size(), 'x');  // the right size, not the file
  CHECK(f.fetch() == MV_LINK_REJECTED);
  CHECK_FALSE(fs::exists(f.dir() / "b.txt"));
  CHECK_FALSE(fs::exists(f.dir() / "b.txt.part"));
  CHECK_FALSE(mob::sound::ready(f.dir().string(), f.pin));
  // A verified record left by hand does not make a wrong file right either.
  std::ofstream(f.dir() / "b.txt", std::ios::binary) << std::string(f.b.size(), 'x');
  CHECK(f.fetch() == MV_LINK_REJECTED);
  CHECK_FALSE(fs::exists(f.dir() / "b.txt"));
}

TEST_CASE("sound tower: no bytes move while the Wi-Fi rule says no, and a gone pin says so", "[mobile-link]") {
  sound_fixture f;
  CHECK(f.fetch(&never) == MV_LINK_WAITING);
  CHECK(f.release.asked.empty());
  CHECK_FALSE(fs::exists(f.dir() / "a.bin.part"));
  f.release.files.erase("https://example.invalid/r/b.txt");
  CHECK(f.fetch() == MV_LINK_NOT_FOUND);
  CHECK_FALSE(mob::sound::ready(f.dir().string(), f.pin));
  mob::cancel_flag stop;
  stop.stop = true;
  CHECK(f.fetch(nullptr, nullptr, &stop) == MV_LINK_CANCELLED);
}

TEST_CASE("sound tower: an older pin's files are cleared, the record is per pin", "[mobile-link]") {
  sound_fixture f;
  fs::create_directories(f.dir());
  std::ofstream(f.dir() / "old-text.onnx") << "an older tower";
  std::ofstream(f.dir() / std::string(mob::sound::kVerified)) << "old-text.onnx 00\n";
  REQUIRE(f.fetch() == MV_LINK_OK);
  CHECK_FALSE(fs::exists(f.dir() / "old-text.onnx"));
  CHECK(read_text(f.dir() / std::string(mob::sound::kVerified)) == mob::sound::verified_text(f.pin));
  CHECK_FALSE(mob::sound::ready(f.dir().string()));  // the app's own pin is another
}

TEST_CASE("sound tower: wanted only for a catalogue whose clips the pinned tower answers", "[mobile-link]") {
  const fs::path root = fs::temp_directory_path() / "mv-sound-wanted";
  fs::remove_all(root);
  fs::create_directories(root);
  const auto make = [&](const std::string& spec) {
    fs::remove(root / "catalog.db");
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open((root / "catalog.db").string().c_str(), &db) == SQLITE_OK);
    const std::string sql = "CREATE TABLE frames(id INTEGER PRIMARY KEY, asset_id INTEGER, spec TEXT);"
                            "CREATE INDEX frames_spec ON frames(spec);"
                            "INSERT INTO frames(asset_id, spec) VALUES(1, 'clip-vit-l14/fp16/pre1'), (2, '" + spec + "');";
    REQUIRE(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
  };
  make(std::string(mob::sound::kSpec));
  CHECK(mv_sound_wanted(root.string().c_str()) == 1);
  make("clap-other/fp16/pre1");
  CHECK(mv_sound_wanted(root.string().c_str()) == 0);
  CHECK(mv_sound_wanted((root / "missing").string().c_str()) == 0);
  CHECK(mv_sound_wanted(nullptr) == 0);
  CHECK(mv_sound_total_bytes() == mob::sound::total_bytes());
  CHECK(std::string(mv_sound_base()).back() == '/');
  fs::remove_all(root);
}
