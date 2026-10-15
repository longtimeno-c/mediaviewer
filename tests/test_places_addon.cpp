// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Places add-on through the real loader (docs/design/27 "Delivery: the
// Places add-on"; docs/design/25 PRs 58-59).
//
// Its manifest (src/addons/places/manifest.json, which tools/package/
// addon-pack.py packs) is accepted and declares exactly the info section, the
// search provider and the settings page: no bar, path or status item, no
// window (owner, 2026-10-12). Then mv_places is installed in a signed store
// with the committed test blob as its data/places.mvgeo, loaded through the v3
// host table, and answers the Place section, its zoom buttons and search by
// place over a fake locations_in.
#include "catch_compat.h"

#include <sodium.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "addon/host.h"
#include "addon/manifest.h"
#include "addon/open_json.h"
#include "addon/open_store.h"
#include "addon/screen.h"
#include "addon/settings_values.h"
#include "addon/slots.h"
#include "addon/store.h"
#include "core/json.h"
#include "import_fixture.h"
#include "io/paths.h"

using namespace mv::test;
using mv::addon::rejection;
using mv::addon::slot_registry;
using mv::status;

namespace {

struct keypair {
  std::vector<std::uint8_t> pub = std::vector<std::uint8_t>(crypto_sign_PUBLICKEYBYTES);
  std::vector<std::uint8_t> sec = std::vector<std::uint8_t>(crypto_sign_SECRETKEYBYTES);
  keypair() {
    REQUIRE(sodium_init() >= 0);
    crypto_sign_keypair(pub.data(), sec.data());
  }
  [[nodiscard]] std::vector<std::uint8_t> sign(const std::string& text) const {
    std::vector<std::uint8_t> sig(crypto_sign_BYTES);
    crypto_sign_detached(sig.data(), nullptr, reinterpret_cast<const unsigned char*>(text.data()), text.size(),
                         sec.data());
    return sig;
  }
};

std::span<const std::uint8_t> bytes_of(const std::string& s) {
  return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

template <typename F>
bool eventually(F f) {
  for (int i = 0; i < 400; ++i) {
    if (f()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return f();
}

// The shipped manifest's description and contributions, its settings page
// (PR 56) included.
std::string shipped_contributes(std::string* description) {
  const std::vector<std::uint8_t> raw = read_bytes(MV_PLACES_MANIFEST_JSON);
  const auto doc = mv::json::parse(std::string(raw.begin(), raw.end()));
  REQUIRE(doc);
  if (description) *description = *doc->str("description");
  const mv::json::value* c = doc->find("contributes");
  REQUIRE(c);
  mv::json::writer w;
  w.begin_object();
  for (const auto& [key, value] : c->o) {
    w.key(key);
    mv::addon::screen::write_value(w, value);
  }
  w.end_object();
  return w.take();
}

struct packed_file {
  std::string path;
  std::vector<std::uint8_t> bytes;
  std::string licence;
};

std::string places_manifest(const std::string& native, const std::vector<packed_file>& files) {
  std::string description;
  const std::string contributes = shipped_contributes(&description);
  std::int64_t size = 0;
  for (const auto& f : files) size += static_cast<std::int64_t>(f.bytes.size());
  mv::json::writer w;
  w.begin_object();
  w.key("schema").integer(1);
  w.key("id").string("places");
  w.key("name").string("Places");
  w.key("version").string("1.0.0");
  w.key("platform").string(std::string(mv::addon::kPlatform));
  w.key("host_api").begin_object().key("min").integer(3).key("max").integer(3).end_object();
  w.key("installed_size").integer(size);
  w.key("native").string(native);
  w.key("chrome").string("");  // the hosts render its tree; it has no chrome of its own
  w.key("archive").begin_object().key("path").string("places.zip").key("sha256").string(std::string(64, 'a'))
      .key("size").integer(1).end_object();
  w.key("files").begin_array();
  for (const auto& f : files) {
    w.begin_object().key("path").string(f.path).key("sha256").string(mv::addon::sha256_hex(f.bytes))
        .key("size").integer(static_cast<std::int64_t>(f.bytes.size())).key("licence").string(f.licence)
        .end_object();
  }
  w.end_array();
  w.key("description").string(description);
  w.key("contributes").raw(contributes);
  w.end_object();
  return w.take();
}

mv_slot_theme light_theme() {
  mv_slot_theme t{};
  t.canvas = 0xffffffff;
  t.surface = 0xf5f5f7ff;
  t.title = 0x1d1d1fff;
  t.body = 0x3c3c43ff;
  t.disabled = 0x8e8e93ff;
  t.hairline = 0xd1d1d6ff;
  t.accent = 0x0a84ffff;
  t.dark = 0;
  return t;
}

}  // namespace

TEST_CASE("places: the shipped manifest declares the section, the search provider and settings only",
          "[addon][places]") {
  keypair k;
  const std::vector<packed_file> files{{"libmv_places.dylib", {1, 2, 3}, "GPL-3.0-or-later"},
                                       {"data/places.mvgeo", {4, 5, 6}, "CC-BY-4.0"}};
  const std::string m = places_manifest("libmv_places.dylib", files);
  const auto d = mv::addon::check_manifest(bytes_of(m), k.sign(m), k.pub, MV_ADDON_HOST_API);
  REQUIRE(rejection_name(d.why) == std::string("ok"));
  const auto& c = d.m.slots;
  REQUIRE(c.info_sections.size() == 1);
  CHECK(c.info_sections[0].id == "place");
  CHECK(c.info_sections[0].title == "Place");
  CHECK(c.info_sections[0].needs_location);  // asked only for a geotagged file
  REQUIRE(c.search_providers.size() == 1);
  CHECK(c.search_providers[0].id == "place");
  CHECK(c.search_providers[0].prefixes == std::vector<std::string>{"place:", "in:"});
  CHECK(c.search_providers[0].files);
  // Owner, 2026-10-12: the map lives only at the foot of the Metadata pane.
  CHECK(c.items.empty());
  CHECK(c.screens.empty());
  CHECK(c.badges.empty());
  CHECK(d.m.commands.empty());
  CHECK(d.m.chrome.empty());
  // The settings page: four items, in the manifest's order.
  REQUIRE(d.m.settings.sections.size() == 1);
  REQUIRE(d.m.settings.sections[0].items.size() == 4);
  CHECK(d.m.settings.find("town_radius_km"));
}

#if defined(MV_PLACES_MODULE_PATH)
TEST_CASE("places: the add-on answers the Place section and search through the v3 host table",
          "[addon][places]") {
  keypair k;
  scratch_dir s("addons_places");
  mv::addon::reset_settings_cache_for_tests();
  mv::io::set_addon_settings_dir_override(utf8(s / "Add-on Settings"));  // never the real profile
  mv::addon::store st(utf8(s.root() / "addons"), k.pub, MV_ADDON_HOST_API);
  fs::create_directories(s / "addons");
  const std::string native = utf8(fs::path(MV_PLACES_MODULE_PATH).filename());
  const std::vector<packed_file> files{{native, read_bytes(MV_PLACES_MODULE_PATH), "GPL-3.0-or-later"},
                                       {"data/places.mvgeo", read_bytes(MV_PLACES_TEST_BLOB), "CC-BY-4.0"}};
  REQUIRE(files[0].bytes.size() > 1000);
  REQUIRE(files[1].bytes.size() > 1000);
  const std::string m = places_manifest(native, files);
  auto staged = st.make_staging();
  REQUIRE(staged);
  for (const auto& f : files) write_bytes(fs::path(*staged) / fs::path(f.path), f.bytes);
  write_text(fs::path(*staged) / "manifest.json", m);
  write_bytes(fs::path(*staged) / "manifest.json.sig", k.sign(m));
  REQUIRE(st.install(*staged));

  // A folder from a trip: Portugal, Spain, and a screenshot with no place.
  const fs::path folder = s / "trip";
  fs::create_directories(folder);
  struct fixture {
    const char* name;
    double lat, lon;
  };
  const fixture trip[] = {{"sintra.jpg", 38.7967, -9.3903},   {"porto.mov", 41.1413, -8.6110},
                          {"funchal.heic", 32.6500, -16.9083}, {"badajoz.jpg", 38.8786, -6.9703},
                          {"llivia.jpg", 42.4642, 1.9806}};
  for (const auto& f : trip) write_text(folder / f.name, "x");
  write_text(folder / "screenshot.png", "x");
  mv::addon::clear_location_cache();

  slot_registry reg;
  mv::addon::host_services svc;
  svc.slots = &reg;
  svc.location = [&trip](const std::string& path) -> std::optional<mv_addon_location> {
    for (const auto& f : trip) {
      if (path.find(f.name) != std::string::npos) {
        mv_addon_location l{};
        l.has = 1;
        l.lat = f.lat;
        l.lon = f.lon;
        return l;
      }
    }
    return std::nullopt;
  };
  // #398: the Local search index's coordinates, as the AI pack would serve
  // them (keys under /index, none of them files on disk).
  std::atomic<int> index_walks{0};
  std::atomic<bool> index_attached{true};
  svc.index_locations = [&](const mv::addon::index_location_visit& visit) -> mv::expected {
    if (!index_attached) return mv::err(status::not_found);
    ++index_walks;
    for (const auto& f : trip) {
      const std::string key = std::string("/index/") + f.name;
      mv_addon_location_entry e{};
      e.path_utf8 = key.c_str();
      e.location.has = 1;
      e.location.lat = f.lat;
      e.location.lon = f.lon;
      if (!visit(e)) return mv::err(status::cancelled);
    }
    return {};
  };
  auto loaded = mv::addon::loaded_addon::load(st, "places", svc);
  REQUIRE(loaded);
  REQUIRE((*loaded)->slots_attached());
  CHECK(reg.addon_calls() == 0);  // nothing opened, nothing read, until a file is shown

  // The Place section: the label, the coordinates beneath, the map with its pin.
  reg.request_info(utf8(folder / "sintra.jpg"), "38.79670\xC2\xB0 N, 9.39030\xC2\xB0 W", light_theme(), 300, 2);
  reg.drain();
  const std::string info = reg.info_json();
  INFO(info);
  CHECK(info.find("\"Sintra, Lisbon, Portugal\"") != std::string::npos);
  CHECK(info.find("38.79670") != std::string::npos);
  CHECK(info.find("\"pin\":{\"x\":0.500,\"y\":0.500}") != std::string::npos);
  CHECK(info.find("Map of Sintra, Lisbon, Portugal") != std::string::npos);
  CHECK(info.find("No street detail") != std::string::npos);
  mv::addon::surface_pixels px;
  REQUIRE(reg.surface("places", "place", "map", px));
  CHECK(px.width == 600);
  CHECK(px.height == 320);
  const std::vector<std::uint8_t> before = px.bgra;

  // Zoom: the click comes back to the add-on, which posts a new picture and
  // moves the buttons' state; the screen keeps its generation.
  REQUIRE(reg.send_event("places", "place", "zoom_out", MV_SLOT_EVENT_CLICK, "") == status::ok);
  reg.drain();
  REQUIRE(eventually([&] {
    mv::addon::surface_pixels now;
    return reg.surface("places", "place", "map", now) && now.bgra != before;
  }));

  // Cursor zoom, drag and double-click on the map (the chrome's settled
  // gestures): each answered with a new picture and the pin moved to where
  // the photo's point now is.
  const auto pixels_now = [&] {
    mv::addon::surface_pixels now;
    REQUIRE(reg.surface("places", "place", "map", now));
    return now.bgra;
  };
  CHECK(info.find("\"interactive\"") != std::string::npos);
  CHECK(info.find("\"marks\"") != std::string::npos);
  std::vector<std::uint8_t> last = pixels_now();
  REQUIRE(reg.send_event("places", "place", "map", MV_SLOT_EVENT_ZOOM, R"({"scale":2,"x":0.4,"y":0.5})") ==
          status::ok);
  reg.drain();
  REQUIRE(eventually([&] { return pixels_now() != last; }));
  // About x = 0.4: the pin (at the centre before) is now at 0.4 + 0.1 * 2.
  CHECK(reg.info_json().find("\"pin\":{\"x\":0.600") != std::string::npos);
  last = pixels_now();
  REQUIRE(reg.send_event("places", "place", "map", MV_SLOT_EVENT_PAN, R"({"dx":-0.1,"dy":0})") == status::ok);
  reg.drain();
  REQUIRE(eventually([&] { return pixels_now() != last; }));
  CHECK(reg.info_json().find("\"pin\":{\"x\":0.500") != std::string::npos);
  last = pixels_now();
  REQUIRE(reg.send_event("places", "place", "map", MV_SLOT_EVENT_DOUBLE, R"({"x":0.5,"y":0.5})") == status::ok);
  reg.drain();
  REQUIRE(eventually([&] { return pixels_now() != last; }));

  // Search by place over the folder.
  reg.search(MV_SLOT_SCOPE_FILES, "portugal", utf8(folder));
  reg.drain();
  auto res = mv::json::parse(reg.search_json(MV_SLOT_SCOPE_FILES));
  REQUIRE(res);
  std::vector<std::string> found;
  for (const auto& row : res->find("rows")->a) found.push_back(utf8(fs::path(*row.str("path")).filename()));
  std::sort(found.begin(), found.end());
  CHECK(found == std::vector<std::string>{"funchal.heic", "porto.mov", "sintra.jpg"});
  CHECK(res->find("rows")->a[0].str("why")->find("Portugal") != std::string::npos);

  reg.search(MV_SLOT_SCOPE_FILES, "place:spain", utf8(folder));
  reg.drain();
  res = mv::json::parse(reg.search_json(MV_SLOT_SCOPE_FILES));
  CHECK(res->find("rows")->a.size() == 2);

  // `in:2024` is a date, not a place: nothing from Places.
  reg.search(MV_SLOT_SCOPE_FILES, "in:2024", utf8(folder));
  reg.drain();
  CHECK(mv::json::parse(reg.search_json(MV_SLOT_SCOPE_FILES))->find("rows")->a.empty());

  // #398: Local search with no folder open walks the index's coordinates
  // (locations_of_index over the fake index above): no file is read.
  const auto local_found = [&] {
    std::vector<std::string> out;
    auto doc = mv::json::parse(reg.search_json(MV_SLOT_SCOPE_LOCAL));
    REQUIRE(doc);
    for (const auto& row : doc->find("rows")->a) out.push_back(*row.str("path"));
    std::sort(out.begin(), out.end());
    return out;
  };
  const auto local_done = [&] {
    auto doc = mv::json::parse(reg.search_json(MV_SLOT_SCOPE_LOCAL));
    return doc && !doc->find("searching")->b;
  };
  reg.search(MV_SLOT_SCOPE_LOCAL, "place:portugal", "");
  reg.drain();
  CHECK(local_found() == std::vector<std::string>{"/index/funchal.heic", "/index/porto.mov", "/index/sintra.jpg"});
  CHECK(local_done());
  CHECK(index_walks.load() == 1);
  reg.search(MV_SLOT_SCOPE_LOCAL, "in:spain", "");
  reg.drain();
  CHECK(local_found() == std::vector<std::string>{"/index/badajoz.jpg", "/index/llivia.jpg"});
  // No index attached (the AI pack not loaded): done, with no rows.
  index_attached = false;
  reg.search(MV_SLOT_SCOPE_LOCAL, "place:portugal", "");
  reg.drain();
  CHECK(local_found().empty());
  CHECK(local_done());
  index_attached = true;

  // Settings take effect at once (docs/design/27 "Settings"): a change re-asks
  // the section for the file on screen, once, and Places reads the new value.
  const mv::addon::open_store open(utf8(s / "Open Add-ons"));
  reg.request_info(utf8(folder / "sintra.jpg"), "38.79670\xC2\xB0 N, 9.39030\xC2\xB0 W", light_theme(), 300, 2);
  reg.drain();
  REQUIRE(reg.info_json().find("\"Sintra, Lisbon, Portugal\"") != std::string::npos);
  auto calls = reg.addon_calls();
  REQUIRE(mv::json::parse(mv::addon::addon_setting_set_json(open, &st, "places", "show_town", "false"))
              ->find("ok")
              ->b);
  reg.drain();
  CHECK(reg.addon_calls() == calls + 1);  // one info request; Places has no badges
  CHECK(reg.info_json().find("Sintra") == std::string::npos);
  CHECK(reg.info_json().find("\"Lisbon, Portugal\"") != std::string::npos);
  CHECK(reg.info_json().find("Map of") != std::string::npos);
  calls = reg.addon_calls();
  REQUIRE(mv::json::parse(mv::addon::addon_setting_set_json(open, &st, "places", "show_map", "false"))
              ->find("ok")
              ->b);
  reg.drain();
  CHECK(reg.addon_calls() == calls + 1);
  CHECK(reg.info_json().find("Map of") == std::string::npos);
  CHECK(reg.info_json().find("\"Lisbon, Portugal\"") != std::string::npos);
  // A click meant for the map it no longer shows changes nothing.
  calls = reg.addon_calls();
  (void)reg.send_event("places", "place", "zoom_out", MV_SLOT_EVENT_CLICK, "");
  reg.drain();
  CHECK(reg.info_json().find("Map of") == std::string::npos);
  // Search reads the radius fresh too: with towns off, "sintra" names nothing.
  reg.search(MV_SLOT_SCOPE_FILES, "sintra", utf8(folder));
  reg.drain();
  CHECK(mv::json::parse(reg.search_json(MV_SLOT_SCOPE_FILES))->find("rows")->a.empty());
  REQUIRE(mv::addon::addon_settings_forget("places"));
  reg.drain();
  CHECK(reg.info_json().find("\"Sintra, Lisbon, Portugal\"") != std::string::npos);
  CHECK(reg.info_json().find("Map of") != std::string::npos);
  reg.search(MV_SLOT_SCOPE_FILES, "sintra", utf8(folder));
  reg.drain();
  CHECK(mv::json::parse(reg.search_json(MV_SLOT_SCOPE_FILES))->find("rows")->a.size() == 1);

  // Removing the add-on removes its section and its answers, with no restart.
  loaded->reset();
  CHECK_FALSE(reg.attached("places"));
  CHECK(mv::json::parse(reg.info_json())->find("sections")->a.empty());
  mv::io::set_addon_settings_dir_override("");
  mv::addon::reset_settings_cache_for_tests();
}
#endif
