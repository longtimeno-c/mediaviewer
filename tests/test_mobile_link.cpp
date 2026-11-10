// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The phone's link and sync against a real `driftwire serve` on loopback
// (docs/plans/mobile.md M3, M4): pairing, the route race, signature and
// generation checks, resume, and the JPEG-512 cache. Filter with
// "[mobile-link]"; nothing here leaves 127.0.0.1 / ::1.
#include <sodium.h>

#include "catch_compat.h"

#include <filesystem>
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
#include "mobile/sync.h"

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
    for (const char* name : {"catalog.db", "extra.bin"}) {
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
    auto next = mob::check_catalogue(s, cfg);
    REQUIRE(next.outcome == mob::sync_outcome::copy_available);
    REQUIRE(mob::copy_catalogue(s, cfg, next, nullptr, nullptr) == mob::sync_outcome::installed);
    installed = mob::read_installed(cfg.library_dir);
    REQUIRE(installed->generation == 2);
    CHECK(installed->high_generation == 2);
    CHECK_FALSE(fs::exists(library / "gen" / "1-1"));
    CHECK_FALSE(fs::exists(library / "engine" / "1-1"));

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
