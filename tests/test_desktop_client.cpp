// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Another desktop opens the host's drives (docs/plans/desktop-client.md, #686):
// src/remote against a real `driftwire serve` on loopback, the way a phone
// meets it. Pairing from the code alone, the folders as `remote:` keys, tiles
// (whole, head, a clip's poster by range reads), originals into the disk cache
// by content identity, and the server shown plainly when it is unreachable, its
// drive offline or this desktop's access ended. Filter with "[desktop-client]";
// nothing leaves 127.0.0.1. The timings are "[.desktop-client-perf]", also in
// "[.perf-bench]" (hidden, so "[desktop-client]" does not run them).
#include <sodium.h>

#include "catch_compat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "clip_fixture.h"
#include "fixtures.h"
#include "import_fixture.h"
#include "image/pipeline.h"
#include "image/thumb.h"
#include "io/file.h"
#include "player/poster.h"
#include "remote/key.h"
#include "remote/library.h"
#include "wire/known.h"
#include "wire/net.h"
#include "wire/secrets.h"
#include "wire/serve.h"

namespace fs = std::filesystem;
namespace rm = mv::remote;
namespace serve = mv::driftwire::serve;
namespace dw = mv::driftwire;
using mv::test::scratch_dir;
using mv::test::utf8;

namespace {

using key32 = std::array<std::uint8_t, 32>;

std::string hex(std::span<const std::uint8_t> b) {
  std::string out(b.size() * 2 + 1, '\0');
  sodium_bin2hex(out.data(), out.size(), b.data(), b.size());
  out.pop_back();
  return out;
}

void put_file(const fs::path& p, const std::vector<std::uint8_t>& bytes) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// A camera-ish JPEG: a gradient with grain, so it compresses as a photo does
// (a 12 MP frame lands at a few MiB, not the 20 of pure noise).
std::vector<std::uint8_t> photo(std::uint32_t w, std::uint32_t h, std::uint32_t seed) {
  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w) * h * 3);
  std::mt19937 rng(seed);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 3;
      const int grain = static_cast<int>(rng() % 24) - 12;
      rgb[i] = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(x * 255 / w) + grain, 0, 255));
      rgb[i + 1] = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(y * 255 / h) + grain, 0, 255));
      rgb[i + 2] = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>((x + y + seed * 37) % 255) + grain, 0, 255));
    }
  }
  return fixtures::jpeg_rgb(w, h, rgb.data());
}

// A host (`driftwire host` and a Synology run this same service), its folder
// "Photos" and the Catalogue a phone pairs with.
struct host {
  scratch_dir dir{"desktop_client"};
  fs::path data = dir / "serve";
  fs::path photos = dir / "photos";
  fs::path catalogue = dir / "lib" / ".mediaviewer";
  std::unique_ptr<dw::secrets::store> store;
  key32 identity{}, access{};
  std::unique_ptr<serve::server> srv;
  std::uint16_t port = 0;
  std::string library_id = "11111111-2222-3333-4444-555555555555";
  key32 publisher{};

  host() {
    REQUIRE(sodium_init() >= 0);
    fs::create_directories(data);
    fs::create_directories(photos);
    fs::create_directories(catalogue);
    REQUIRE(dw::secrets::owner_only_folder((data / "secrets").string()));
    store = dw::secrets::file_store((data / "secrets").string());
    REQUIRE(store);
    bool made = false;
    REQUIRE(serve::set_up(*store, identity, access, made));
    randombytes_buf(publisher.data(), publisher.size());
    // The host library a phone pairs with (self-hosted.md "The host library").
    const std::string lib = "{\"format\":1,\"library_id\":\"" + library_id + "\",\"public_key\":\"" + hex(publisher) + "\"}";
    put_file(catalogue / "library.json", std::vector<std::uint8_t>(lib.begin(), lib.end()));
    start();
  }

  void start(std::uint16_t want_port = 0) {
    serve::server::options o;
    o.store = store.get();
    o.loopback_only = true;
    o.per_minute = 100000;
    o.per_source_pending = 16;
    o.max_pending = 64;
    o.cfg.port = want_port;
    o.cfg.name = "Home";
    o.cfg.roots.push_back({"a1a1a1a1a1a1a1a1", "Photos", photos.string()});
    o.cfg.roots.push_back({"c2c2c2c2c2c2c2c2", serve::kCatalogueLabel, catalogue.string()});
    o.make_listener = [want_port] {
      auto l = dw::net::listen_internet(want_port, true, 64);
      REQUIRE(l);
      return std::move(*l);
    };
    auto s = serve::server::start(o);
    REQUIRE(s);
    srv = std::move(*s);
    port = srv->port();
  }
  void stop() { srv.reset(); }

  // What the host's card and `driftwire host pair` show: the code as a link.
  std::string code(std::int64_t valid_s = 600) {
    const std::string c = serve::issue_pair_code(*store, valid_s, std::time(nullptr));
    const std::string json = "{\"v\":1,\"library_id\":\"" + library_id + "\",\"library_name\":\"Home\",\"publisher_key\":\"" +
                             hex(publisher) + "\",\"service\":{\"local\":\"127.0.0.1:" + std::to_string(port) +
                             "\",\"public\":\"localhost:" + std::to_string(port) + "\",\"identity\":\"" + hex(identity) +
                             "\"},\"access_key\":\"" + hex(access) + "\",\"code\":\"" + c +
                             "\",\"expires\":" + std::to_string(std::time(nullptr) + valid_s) + "}";
    std::string b64(sodium_base64_ENCODED_LEN(json.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING), '\0');
    sodium_bin2base64(b64.data(), b64.size(), reinterpret_cast<const unsigned char*>(json.data()), json.size(),
                      sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    b64.resize(std::strlen(b64.c_str()));
    return "mediaviewer://pair#" + b64;
  }
};

// The tile makers as the hosts wire them.
rm::config desktop_config(const fs::path& root, dw::secrets::store* keys) {
  rm::config c;
  c.data_dir = utf8(root / "data");
  c.cache_dir = utf8(root / "cache");
  c.device_name = "Test Desktop";
  c.store = keys;
  c.loopback_only = true;
  c.connect_timeout_ms = 5000;
  c.retry_after_ms = 10000;
  c.still_tile = [](std::span<const std::uint8_t> bytes, const mv::job_context* ctx) {
    return mv::image::make_thumb_jpeg(bytes, ctx);
  };
  c.clip_tile = [](rm::range_reader& r, const mv::job_context* ctx) -> mv::result<std::vector<std::uint8_t>> {
    struct adapter final : mv::player::byte_source {
      rm::range_reader& r;
      explicit adapter(rm::range_reader& rr) : r(rr) {}
      mv::result<std::size_t> read_at(std::uint64_t off, std::span<std::uint8_t> out) noexcept override {
        return r.read_at(off, out);
      }
      std::uint64_t size() const noexcept override { return r.size(); }
    } src(r);
    auto poster = mv::player::poster_frame(src, mv::image::kThumbLongEdge, ctx);
    if (!poster) return mv::err(poster.error());
    return mv::image::encode_thumb_rgba(poster->rgba, poster->width, poster->height);
  };
  return c;
}

struct desktop {
  scratch_dir dir{"desktop_side"};
  std::unique_ptr<dw::secrets::store> keys;
  std::unique_ptr<rm::library> lib;
  desktop() {
    REQUIRE(dw::secrets::owner_only_folder(utf8(dir / "keys")));
    keys = dw::secrets::file_store(utf8(dir / "keys"));
    REQUIRE(keys);
    relaunch();
  }
  // A new run of the app: everything from disk again.
  void relaunch() {
    lib.reset();
    lib = std::make_unique<rm::library>(desktop_config(dir.root(), keys.get()));
    REQUIRE(lib->load());
  }
};

const rm::file* find_file(const rm::listing& l, std::string_view name) {
  for (const auto& f : l.files) {
    if (f.name == name) return &f;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("remote keys: a folder ends in a slash, nothing climbs out", "[desktop-client]") {
  const std::string s = "0123456789abcdef", r = "a1a1a1a1a1a1a1a1";
  auto top = rm::parse_key("remote:" + s + "/");
  REQUIRE(top);
  CHECK(top->root.empty());
  CHECK(top->folder);
  auto file = rm::parse_key("remote:" + s + "/" + r + "/2024/IMG 1.JPG");
  REQUIRE(file);
  CHECK_FALSE(file->folder);
  CHECK(file->rel == "2024/IMG 1.JPG");
  CHECK(rm::make_key(*file) == "remote:" + s + "/" + r + "/2024/IMG 1.JPG");
  auto sub = rm::parse_key("remote:" + s + "/" + r + "/2024/");
  REQUIRE(sub);
  CHECK(sub->folder);
  CHECK(rm::make_key(*sub) == "remote:" + s + "/" + r + "/2024/");
  CHECK(rm::parent_key("remote:" + s + "/" + r + "/2024/IMG 1.JPG") == "remote:" + s + "/" + r + "/2024/");
  CHECK(rm::parent_key("remote:" + s + "/" + r + "/") == "remote:" + s + "/");
  CHECK(rm::leaf_name("remote:" + s + "/" + r + "/2024/IMG 1.JPG") == "IMG 1.JPG");
  for (const std::string& bad : std::vector<std::string>{"remote:", "remote:xyz/", "remote:" + s, "remote:" + s + "/" + r,
                                "remote:" + s + "/" + r + "/../x", "remote:" + s + "/" + r + "/a//b",
                                "remote:" + s + "/" + r + "/a\\b", "photos:abc", "/Users/x.jpg"}) {
    CHECK_FALSE(rm::parse_key(bad));
  }
  CHECK(rm::is_key("remote:x"));
  CHECK_FALSE(rm::is_key("/remote:x"));
}

TEST_CASE("a desktop pairs from the code alone and is listed with the phones", "[desktop-client]") {
  host h;
  desktop d;
  const std::string code = h.code();
  auto preview = d.lib->preview(code, std::time(nullptr));
  REQUIRE(preview);
  CHECK(preview->name == "Home");
  CHECK(preview->fingerprint.size() == 19);
  CHECK_FALSE(preview->already_paired);
  // No passphrase, no owner: the code is all it takes (owner call 2).
  auto id = d.lib->pair("  " + code + "\n", std::time(nullptr));
  if (!id) FAIL("pair: " << mv::status_name(id.error()));
  const auto servers = d.lib->servers();
  REQUIRE(servers.size() == 1);
  CHECK(servers[0].name == "Home");
  CHECK(servers[0].state == rm::reach::online);
  // The service keeps it as one more paired device, under the name it gave.
  const auto devices = dw::known::load(*h.store);
  CHECK(std::any_of(devices.begin(), devices.end(), [](const dw::known::device& dv) { return dv.name == "Test Desktop"; }));
  // The code is single use; and no key reached servers.json.
  CHECK_FALSE(d.lib->pair(code, std::time(nullptr)));
  auto json = mv::io::read_all(utf8(d.dir / "data" / "servers.json"));
  REQUIRE(json);
  const std::string text(json->begin(), json->end());
  CHECK(text.find(hex(h.access)) == std::string::npos);
  CHECK(text.find("Home") != std::string::npos);
  // Expired and not-a-code are said apart.
  CHECK(d.lib->pair(h.code(1), std::time(nullptr) + 5).error() == mv::status::timeout);
  CHECK(d.lib->pair("hello", std::time(nullptr)).error() == mv::status::invalid_arg);
  // A relaunch finds it again, with its keys from the OS store.
  d.relaunch();
  REQUIRE(d.lib->servers().size() == 1);
  CHECK(d.lib->list(rm::server_key(*id)));
}

TEST_CASE("the host's folders list as remote keys, the Catalogue hidden", "[desktop-client]") {
  host h;
  put_file(h.photos / "2024" / "a.jpg", photo(64, 48, 1));
  put_file(h.photos / "2024" / "b.JPG", photo(64, 48, 2));
  put_file(h.photos / "2024" / "notes.txt", {'h', 'i'});
  put_file(h.photos / "2024" / ".hidden.jpg", photo(8, 8, 3));
  fs::create_directories(h.photos / "2024" / "Trip");
  put_file(h.photos / "top.png", photo(8, 8, 4));
  desktop d;
  auto id = d.lib->pair(h.code(), std::time(nullptr));
  REQUIRE(id);
  auto top = d.lib->list(rm::server_key(*id));
  REQUIRE(top);
  CHECK(top->title == "Home");
  REQUIRE(top->folders.size() == 1);  // Photos; never the Catalogue
  CHECK(top->folders[0].name == "Photos");
  CHECK(d.lib->display_name(top->folders[0].key) == "Photos");
  auto photos = d.lib->list(top->folders[0].key);
  REQUIRE(photos);
  CHECK(photos->title == "Photos");
  REQUIRE(photos->folders.size() == 1);
  CHECK(photos->folders[0].name == "2024");
  REQUIRE(photos->files.size() == 1);  // top.png is media; the PNG name kept
  auto year = d.lib->list(photos->folders[0].key);
  REQUIRE(year);
  CHECK(year->title == "2024");
  REQUIRE(year->folders.size() == 1);
  CHECK(year->folders[0].name == "Trip");
  REQUIRE(year->files.size() == 2);  // no .txt, no hidden file
  CHECK(year->files[0].name == "a.jpg");
  CHECK(year->files[1].name == "b.JPG");
  CHECK(year->files[0].size == fs::file_size(h.photos / "2024" / "a.jpg"));
  CHECK(rm::parse_key(year->files[0].key)->rel == "2024/a.jpg");
  CHECK_FALSE(d.lib->can_write(year->files[0].key));  // read-only first (owner call 1)
}

TEST_CASE("first pixel: a tile before the original, and each read once", "[desktop-client]") {
  host h;
  put_file(h.photos / "small.jpg", photo(1600, 1200, 5));
  // A large RAW whose picture is all in its head (a camera's embedded preview
  // stands in): a JPEG, then 17 MiB of sensor data. Read by its head only.
  std::vector<std::uint8_t> big = photo(1200, 900, 6);
  const std::size_t picture = big.size();
  big.resize(picture + (17u << 20), 0x5a);
  put_file(h.photos / "big.dng", big);
  // A large JPEG is never read by its head: cut short it would be a wrong tile.
  std::vector<std::uint8_t> huge = photo(1600, 1200, 9);
  huge.resize(huge.size() + (17u << 20), 0x00);
  put_file(h.photos / "huge.jpg", huge);
  namespace fx = mv::test::clipfx;
  fx::spec clip;
  clip.audio = false;
  clip.frames = 30;
  REQUIRE(fx::make(utf8(h.photos / "clip.mp4"), clip));
  desktop d;
  auto id = d.lib->pair(h.code(), std::time(nullptr));
  REQUIRE(id);
  auto top = d.lib->list(rm::server_key(*id));
  REQUIRE(top);
  auto listing = d.lib->list(top->folders.at(0).key);
  REQUIRE(listing);
  const rm::file* small = find_file(*listing, "small.jpg");
  const rm::file* large = find_file(*listing, "big.dng");
  const rm::file* huge_jpeg = find_file(*listing, "huge.jpg");
  const rm::file* movie = find_file(*listing, "clip.mp4");
  REQUIRE(small);
  REQUIRE(large);
  REQUIRE(huge_jpeg);
  REQUIRE(movie);

  // A small still: fetched whole once, the tile made from it, the original kept.
  auto t1 = d.lib->tile(small->key);
  REQUIRE(t1);
  CHECK(t1->size() > 1000);
  CHECK(d.lib->stats().whole_fetches == 1);
  REQUIRE(d.lib->cached(small->key));
  CHECK(d.lib->cached_tile(small->key));
  const auto read_after_small = d.lib->stats().bytes_read.load();
  // Opening it now reads nothing more.
  auto local = d.lib->fetch(small->key);
  REQUIRE(local);
  CHECK(d.lib->stats().bytes_read == read_after_small);
  CHECK(*mv::io::read_all(*local) == *mv::io::read_all(utf8(h.photos / "small.jpg")));

  // A large still: its head only.
  auto t2 = d.lib->tile(large->key);
  REQUIRE(t2);
  CHECK(d.lib->stats().head_reads == 1);
  CHECK(d.lib->stats().whole_fetches == 1);
  CHECK(d.lib->stats().bytes_read - read_after_small <= rm::library::kHeadTileBytes);

  // A large JPEG: whole (its head is not a picture of it).
  REQUIRE(d.lib->tile(huge_jpeg->key));
  CHECK(d.lib->stats().head_reads == 1);
  CHECK(d.lib->stats().whole_fetches == 2);
  CHECK(d.lib->cached(huge_jpeg->key));

  // A clip: its poster through range reads, never the whole clip.
  const auto before_clip = d.lib->stats().bytes_read.load();
  auto t3 = d.lib->tile(movie->key);
  REQUIRE(t3);
  CHECK(d.lib->stats().whole_fetches == 2);
  CHECK(d.lib->stats().bytes_read - before_clip <= movie->size + (256u << 10));

  // A relaunch: tiles and the original from the disk, nothing from the network.
  d.relaunch();
  REQUIRE(d.lib->list(top->folders.at(0).key));
  const auto listed = d.lib->stats().bytes_read.load();
  REQUIRE(d.lib->tile(small->key));
  REQUIRE(d.lib->tile(large->key));
  REQUIRE(d.lib->tile(movie->key));
  REQUIRE(d.lib->fetch(small->key));
  CHECK(d.lib->stats().bytes_read == listed);
  CHECK(d.lib->stats().whole_fetches == 0);

  // A changed file is a new entry (size and mtime are its identity).
  put_file(h.photos / "small.jpg", photo(800, 600, 7));
  auto again = d.lib->list(top->folders.at(0).key);
  REQUIRE(again);
  CHECK_FALSE(d.lib->cached(find_file(*again, "small.jpg")->key));
}

TEST_CASE("originals: a range, one fetch for two callers, cancel leaves nothing", "[desktop-client]") {
  host h;
  std::vector<std::uint8_t> raw(6u << 20);
  for (std::size_t i = 0; i < raw.size(); ++i) raw[i] = static_cast<std::uint8_t>(i * 2654435761u >> 24);
  put_file(h.photos / "IMG_0001.CR3", raw);
  desktop d;
  auto id = d.lib->pair(h.code(), std::time(nullptr));
  REQUIRE(id);
  auto top = d.lib->list(rm::server_key(*id));
  REQUIRE(top);
  auto l = d.lib->list(top->folders.at(0).key);
  REQUIRE(l);
  const rm::file* f = find_file(*l, "IMG_0001.CR3");
  REQUIRE(f);

  std::vector<std::uint8_t> mid;
  REQUIRE(d.lib->read_range(f->key, 1000000, 4096, [&](std::uint64_t, std::span<const std::uint8_t> b) {
    mid.insert(mid.end(), b.begin(), b.end());
    return true;
  }));
  REQUIRE(mid.size() == 4096);
  CHECK(std::equal(mid.begin(), mid.end(), raw.begin() + 1000000));

  // Cancelled at once: no original, no part file.
  mv::mobile::cancel_flag stop;
  stop.stop = true;
  CHECK_FALSE(d.lib->fetch(f->key, &stop));
  CHECK_FALSE(d.lib->cached(f->key));

  // Two callers at once (the viewer and its prefetch): one fetch.
  auto a = std::async(std::launch::async, [&] { return d.lib->fetch(f->key); });
  auto b = std::async(std::launch::async, [&] { return d.lib->fetch(f->key); });
  auto ra = a.get();
  auto rb = b.get();
  REQUIRE(ra);
  REQUIRE(rb);
  CHECK(*ra == *rb);
  CHECK(d.lib->stats().whole_fetches == 1);
  CHECK(*mv::io::read_all(*ra) == raw);
  CHECK(fs::path(*ra).filename() == "IMG_0001.CR3");  // the name kept: kind by name holds
  std::size_t parts = 0;
  for (auto& e : fs::recursive_directory_iterator(d.dir / "cache")) parts += e.path().extension() == ".part";
  CHECK(parts == 0);
}

TEST_CASE("the cache keeps to its cap, newest kept", "[desktop-client]") {
  scratch_dir dir("remote_cache");
  rm::disk_cache cache(utf8(dir.root()), 3000, /*keep_recent=*/1);
  auto put = [&](const char* rel) {
    rm::identity id{"0123456789abcdef", "a1a1a1a1a1a1a1a1", rel, 1000, 7};
    const std::string part = cache.original_path(id) + ".part";
    put_file(fs::path(part), std::vector<std::uint8_t>(1000, 1));
    REQUIRE(cache.admit(id, part));
    return id;
  };
  const auto a = put("a.jpg");
  const auto b = put("b.jpg");
  const auto c = put("c.jpg");
  REQUIRE(cache.find(a));  // a used again: b is now the oldest
  const auto e = put("e.jpg");
  CHECK(cache.find(a));
  CHECK_FALSE(cache.find(b));
  CHECK(cache.find(c));
  CHECK(cache.find(e));
  CHECK(cache.bytes_used() <= 3000);
  // A part file whose size is not the listing's is refused.
  rm::identity bad{"0123456789abcdef", "a1a1a1a1a1a1a1a1", "bad.jpg", 999, 7};
  const std::string part = cache.original_path(bad) + ".part";
  put_file(fs::path(part), std::vector<std::uint8_t>(10, 1));
  CHECK_FALSE(cache.admit(bad, part));
  CHECK_FALSE(fs::exists(fs::path(part)));
}

TEST_CASE("unreachable, offline and ended are said plainly", "[desktop-client]") {
  host h;
  put_file(h.photos / "a.jpg", photo(64, 48, 8));
  desktop d;
  auto id = d.lib->pair(h.code(), std::time(nullptr));
  REQUIRE(id);
  auto top = d.lib->list(rm::server_key(*id));
  REQUIRE(top);
  auto l = d.lib->list(top->folders.at(0).key);
  REQUIRE(l);
  const std::string key = l->files.at(0).key;
  REQUIRE(d.lib->tile(key));  // cached before the host goes away

  // The host goes away: a fresh run of the desktop cannot reach it. The
  // folder shows as it was last listed, said to be stale, and what was
  // fetched still opens from the disk.
  h.stop();
  d.relaunch();
  auto stale = d.lib->list(top->folders.at(0).key);
  REQUIRE(stale);
  CHECK(stale->stale);
  REQUIRE(stale->files.size() == 1);
  CHECK(d.lib->server(*id)->state == rm::reach::unreachable);
  CHECK(d.lib->cached(key));
  CHECK(d.lib->tile(key));
  // In its retry pause a call that is not the person's own fails at once.
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(d.lib->list(top->folders.at(0).key, /*person=*/false)->stale);
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200));
  // A folder never listed here has nothing to show: unreachable, said so.
  CHECK(d.lib->list(rm::server_key(*id) + "a1a1a1a1a1a1a1a1/never/").error() == mv::status::io);

  // A drive unplugged: the folder stays listed, marked offline.
  h.start();
  fs::rename(h.photos, h.dir / "unplugged");
  d.relaunch();
  // The host's port changed (a restart); the code's routes are the old ones
  // in this test, so it re-pairs as a person would after a move.
  REQUIRE(d.lib->pair(h.code(), std::time(nullptr)));
  auto again = d.lib->list(rm::server_key(*id));
  REQUIRE(again);
  REQUIRE(again->folders.size() == 1);
  CHECK(again->folders[0].offline);
  fs::rename(h.dir / "unplugged", h.photos);

  // Revoked on the host: access ended, not "unreachable".
  auto devices = dw::known::load(*h.store);
  std::erase_if(devices, [](const dw::known::device& dv) { return dv.name == "Test Desktop"; });
  REQUIRE(dw::known::save(*h.store, devices));
  REQUIRE(dw::known::load(*h.store).size() == devices.size());
  d.relaunch();
  CHECK_FALSE(d.lib->list(rm::server_key(*id)));
  CHECK(d.lib->server(*id)->state == rm::reach::access_ended);

  // Forget: the keys and the cache go.
  REQUIRE(d.lib->forget(*id));
  CHECK(d.lib->servers().empty());
  CHECK_FALSE(d.keys->get("remote_" + *id));
  CHECK_FALSE(fs::exists(d.dir / "cache" / *id));
}

// ---- timings ------------------------------------------------------------------------------------
// mv_tests "[.desktop-client-perf]" (and in "[.perf-bench]"): a folder of camera-sized JPEGs on
// a host over loopback, the desktop's cold tile (first pixel), full
// resolution (fetch + full decode), and arrow-to-next for a neighbour the
// viewer already fetched (the ±2 prefetch) and for one it did not. Beside
// them, the same file from a local disk. MV_REMOTE_BENCH_JSON=path writes them.
TEST_CASE("remote folder timings over loopback", "[.perf-bench][.desktop-client-perf]") {
  using clock = std::chrono::steady_clock;
  const auto ms = [](clock::time_point t0) { return std::chrono::duration<double, std::milli>(clock::now() - t0).count(); };
  const auto median = [](std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
  };
  host h;
  constexpr int kFiles = 12;
  for (int i = 0; i < kFiles; ++i) {
    char name[32];
    std::snprintf(name, sizeof name, "IMG_%04d.JPG", i);
    put_file(h.photos / name, photo(4032, 3024, 100 + static_cast<std::uint32_t>(i)));
  }
  desktop d;
  auto id = d.lib->pair(h.code(), std::time(nullptr));
  REQUIRE(id);
  auto top = d.lib->list(rm::server_key(*id));
  REQUIRE(top);
  const auto t_list = clock::now();
  auto l = d.lib->list(top->folders.at(0).key);
  const double list_ms = ms(t_list);
  REQUIRE(l);
  REQUIRE(l->files.size() == kFiles);

  std::vector<double> tile_cold, full_cold, arrow_cached, arrow_cold, local_first, local_full;
  for (int i = 0; i < kFiles; ++i) {
    const rm::file& f = l->files[static_cast<std::size_t>(i)];
    if (i % 3 == 0) {
      // First pixel: the tile, cold (fetch whole: a camera JPEG is under 16 MiB).
      const auto t0 = clock::now();
      REQUIRE(d.lib->tile(f.key));
      tile_cold.push_back(ms(t0));
      // Full resolution once it is here.
      const auto t1 = clock::now();
      auto path = d.lib->fetch(f.key);
      REQUIRE(path);
      auto bytes = mv::io::read_all(*path);
      REQUIRE(mv::image::decode_bytes(*bytes, nullptr));
      full_cold.push_back(ms(t0) - 0.0);
      (void)t1;
    } else if (i % 3 == 1) {
      // Arrow to a neighbour the prefetch fetched: a cache hit, then the first pixel.
      REQUIRE(d.lib->fetch(f.key));
      const auto t0 = clock::now();
      auto path = d.lib->cached(f.key);
      REQUIRE(path);
      auto bytes = mv::io::read_all(*path);
      REQUIRE(mv::image::decode_first_pixel(*bytes, nullptr));
      arrow_cached.push_back(ms(t0));
    } else {
      // Arrow to one nobody fetched: the fetch, then the first pixel.
      const auto t0 = clock::now();
      auto path = d.lib->fetch(f.key);
      REQUIRE(path);
      auto bytes = mv::io::read_all(*path);
      REQUIRE(mv::image::decode_first_pixel(*bytes, nullptr));
      arrow_cold.push_back(ms(t0));
    }
    // The same bytes from the host's own disk.
    const std::string local = utf8(h.photos / f.name);
    const auto t2 = clock::now();
    auto bytes = mv::io::read_all(local);
    REQUIRE(mv::image::decode_first_pixel(*bytes, nullptr));
    local_first.push_back(ms(t2));
    const auto t3 = clock::now();
    auto again = mv::io::read_all(local);
    REQUIRE(mv::image::decode_bytes(*again, nullptr));
    local_full.push_back(ms(t3));
  }
  const double mb = static_cast<double>(l->files[0].size) / (1024.0 * 1024.0);
  std::printf("remote folder over loopback (%d JPEGs, 4032x3024, %.1f MiB each)\n", kFiles, mb);
  std::printf("  list folder            %8.2f ms\n", list_ms);
  std::printf("  remote first pixel     %8.2f ms  (tile, cold)\n", median(tile_cold));
  std::printf("  remote full res        %8.2f ms  (tile + fetch + full decode, cold)\n", median(full_cold));
  std::printf("  remote arrow, cached   %8.2f ms  (prefetched neighbour: first pixel)\n", median(arrow_cached));
  std::printf("  remote arrow, cold     %8.2f ms  (fetch + first pixel)\n", median(arrow_cold));
  std::printf("  local first pixel      %8.2f ms\n", median(local_first));
  std::printf("  local full res         %8.2f ms\n", median(local_full));
  if (const char* out = std::getenv("MV_REMOTE_BENCH_JSON")) {
    std::ofstream j(out);
    j << "{\"files\":" << kFiles << ",\"mib_each\":" << mb << ",\"list_ms\":" << list_ms
      << ",\"remote_first_pixel_ms\":" << median(tile_cold) << ",\"remote_full_ms\":" << median(full_cold)
      << ",\"remote_arrow_cached_ms\":" << median(arrow_cached) << ",\"remote_arrow_cold_ms\":" << median(arrow_cold)
      << ",\"local_first_pixel_ms\":" << median(local_first) << ",\"local_full_ms\":" << median(local_full) << "}\n";
  }
}

