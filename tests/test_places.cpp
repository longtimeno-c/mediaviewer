// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Places add-on's engine (src/addons/places; docs/design/27 "The map").
//
//   mv_tests "[places]"             always, on the committed test blob
//                                   (tests/data/geo/places-test.mvgeo): the
//                                   map's colours at known points (land, water,
//                                   the Portugal/Spain border), the corners,
//                                   the cache and a theme change, the Place
//                                   section's tree, the zoom steps, and search
//                                   by place over a fake folder.
//   mv_tests "[.perf-bench]"        map render time at pane width x 160 pt on
//                                   the full blob (MV_GEO_BLOB or the build's).
//   mv_tests "[.places-map-dump]"   MV_PLACES_MAP_DUMP=<dir>: writes BMPs of the
//                                   full-blob map at a few places, light and
//                                   dark, every zoom step, for a person to look at.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "catch_compat.h"

#include "addons/places/map.h"
#include "addons/places/places.h"
#include "core/json.h"
#include "geo/open.h"

using namespace mv::places;

namespace {

const mv::geo::database& test_db() {
  static auto db = [] {
    auto r = mv::geo::open_database(MV_GEO_TEST_BLOB);
    REQUIRE(r);
    return std::move(*r);
  }();
  return db->db();
}

std::string full_blob() {
  const char* env = std::getenv("MV_GEO_BLOB");
  return env && *env ? env : MV_GEO_FULL_BLOB;
}

theme_tokens light_theme() { return theme_tokens{}; }

theme_tokens dark_theme() {
  theme_tokens t;
  t.canvas = 0x1e1e1eff;
  t.surface = 0x262523ff;
  t.title = 0xf2efe9ff;
  t.body = 0xb9b4aaff;
  t.disabled = 0x6f6b64ff;
  t.hairline = 0x3a3835ff;
  t.accent = 0xe0793aff;
  t.dark = true;
  return t;
}

struct px {
  int r, g, b, a;
};

px pixel_at(const std::vector<std::uint8_t>& bgra, std::uint32_t w, int x, int y) {
  const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
  return {bgra[i + 2], bgra[i + 1], bgra[i + 0], bgra[i + 3]};
}

int distance(px p, std::uint32_t rgb) {
  return std::max({std::abs(p.r - static_cast<int>((rgb >> 16) & 0xff)),
                   std::abs(p.g - static_cast<int>((rgb >> 8) & 0xff)), std::abs(p.b - static_cast<int>(rgb & 0xff))});
}

px pixel_of(const std::vector<std::uint8_t>& bgra, const map_view& v, double lat, double lon) {
  double x = 0;
  double y = 0;
  map_pixel(v, lat, lon, x, y);
  REQUIRE(x >= 0);
  REQUIRE(y >= 0);
  REQUIRE(x < v.width);
  REQUIRE(y < v.height);
  return pixel_at(bgra, v.width, static_cast<int>(x), static_cast<int>(y));
}

std::vector<std::uint8_t> draw(const mv::geo::database& db, const map_view& v, const map_palette& p,
                               std::string_view home) {
  std::vector<std::uint8_t> out(static_cast<std::size_t>(v.width) * v.height * 4);
  REQUIRE(render_map(db, v, p, home, out));
  return out;
}

void write_bmp(const std::filesystem::path& path, const map_image& img) {
  const std::uint32_t w = img.width;
  const std::uint32_t h = img.height;
  const std::uint32_t size = 54 + w * h * 4;
  std::vector<std::uint8_t> b(54, 0);
  auto put32 = [&b](std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
  };
  b[0] = 'B';
  b[1] = 'M';
  put32(2, size);
  put32(10, 54);
  put32(14, 40);
  put32(18, w);
  put32(22, static_cast<std::uint32_t>(-static_cast<std::int32_t>(h)));  // top-down
  b[26] = 1;
  b[28] = 32;
  put32(34, w * h * 4);
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  // Over the pane's colour, as the chrome composites it.
  std::vector<std::uint8_t> flat(img.bgra);
  for (std::size_t i = 0; i < flat.size(); i += 4) {
    const int a = flat[i + 3];
    for (int c = 0; c < 3; ++c) flat[i + c] = static_cast<std::uint8_t>(flat[i + c] + (255 - a) * 0xf5 / 255);
    flat[i + 3] = 255;
  }
  f.write(reinterpret_cast<const char*>(flat.data()), static_cast<std::streamsize>(flat.size()));
}

// A folder's files and their coordinates, as the host's locations_in would
// hand them over.
struct fake_file {
  const char* path;
  bool has;
  double lat, lon;
};

const fake_file kFolder[] = {
    {"/trip/sintra.jpg", true, 38.7967, -9.3903},
    {"/trip/lisboa.heic", true, 38.7077, -9.1365},
    {"/trip/porto.mov", true, 41.1413, -8.6110},
    {"/trip/funchal.jpg", true, 32.6500, -16.9083},
    {"/trip/ponta-delgada.jpg", true, 37.7412, -25.6756},
    {"/trip/elvas.jpg", true, 38.8800, -7.1630},
    {"/trip/badajoz.jpg", true, 38.8786, -6.9703},
    {"/trip/llivia.jpg", true, 42.4642, 1.9806},
    {"/trip/suva.jpg", true, -18.1416, 178.4419},
    {"/trip/at-sea.jpg", true, 38.0, -14.0},
    {"/trip/screenshot.png", false, 0, 0},
};

std::vector<std::string> search(const mv::geo::database& db, std::string_view folded,
                                std::vector<std::string>* whys = nullptr) {
  const auto q = place_query::compile(db, folded);
  std::vector<std::string> hits;
  if (q.empty()) return hits;
  for (const fake_file& f : kFolder) {
    if (!f.has) continue;
    const std::string why = q.why(db, f.lat, f.lon, settings{}.lookup());
    if (!why.empty()) {
      hits.emplace_back(f.path);
      if (whys) whys->push_back(why);
    }
  }
  return hits;
}

}  // namespace

TEST_CASE("places: the palette follows the theme, light and dark", "[places]") {
  const map_palette light = palette_for(light_theme());
  const map_palette dark = palette_for(dark_theme());
  CHECK(light.water != light.land_home);
  CHECK(light.land != light.land_home);
  CHECK(light.border != light.land_home);
  CHECK(dark.water != dark.land_home);
  CHECK_FALSE(light == dark);
  // Dark land is dark; light land is light.
  CHECK(((light.land_home >> 8) & 0xff) > 0xc0);
  CHECK(((dark.land_home >> 8) & 0xff) < 0x60);
  CHECK(light.frame == (light_theme().hairline >> 8));
}

TEST_CASE("places: the map draws land, water and the border at known points", "[places]") {
  const auto& db = test_db();
  const map_palette p = palette_for(light_theme());

  SECTION("Sintra: land in Portugal, the Atlantic to the west") {
    map_view v{38.7967, -9.3903, 320, 160, 1.0f, k_zoom_steps - 1};
    const auto img = draw(db, v, p, "PT");
    CHECK(distance(pixel_of(img, v, 38.7967, -9.3903), p.land_home) <= 2);
    CHECK(distance(pixel_of(img, v, 38.80, -9.65), p.water) <= 2);
    // The Tagus estuary, south of Lisbon's waterfront.
    CHECK(distance(pixel_of(img, v, 38.66, -9.30), p.water) <= 2);
  }

  SECTION("Elvas and Badajoz: Portugal, the border, then Spain") {
    map_view v{38.88, -7.07, 320, 160, 1.0f, k_zoom_steps - 1};
    const auto img = draw(db, v, p, "PT");
    CHECK(distance(pixel_of(img, v, 38.8800, -7.1630), p.land_home) <= 2);  // Elvas
    CHECK(distance(pixel_of(img, v, 38.8786, -6.9703), p.land) <= 2);       // Badajoz, Spain
    // Somewhere on the row between them, a border pixel.
    double x0 = 0, x1 = 0, y = 0;
    map_pixel(v, 38.88, -7.16, x0, y);
    map_pixel(v, 38.88, -6.98, x1, y);
    int best = 255;
    for (int x = static_cast<int>(x0); x <= static_cast<int>(x1); ++x) {
      best = std::min(best, distance(pixel_at(img, v.width, x, static_cast<int>(y)), p.border));
    }
    CHECK(best <= 24);
  }

  SECTION("the corners are rounded and transparent, the middle opaque") {
    map_view v{38.7967, -9.3903, 320, 160, 2.0f, 3};
    const auto img = draw(db, v, p, "PT");
    CHECK(pixel_at(img, v.width, 0, 0).a == 0);
    CHECK(pixel_at(img, v.width, 319, 159).a == 0);
    CHECK(pixel_at(img, v.width, 160, 80).a == 255);
    // The hairline frame runs along the edge, between the corners.
    CHECK(distance(pixel_at(img, v.width, 160, 0), p.frame) <= 40);
  }

  SECTION("Fiji either side of the antimeridian draws whole") {
    map_view v{-16.75, 179.8, 320, 160, 1.0f, k_zoom_steps - 1};
    const auto img = draw(db, v, p, "FJ");
    CHECK(distance(pixel_of(img, v, -16.55, 179.20), p.land_home) <= 2);   // Vanua Levu, west of 180
    CHECK(distance(pixel_of(img, v, -16.83, -179.98), p.land_home) <= 2);  // Taveuni, east of it
    CHECK(distance(pixel_of(img, v, -17.10, 179.60), p.water) <= 2);       // Koro Sea
  }

  SECTION("a bad size draws nothing") {
    std::vector<std::uint8_t> out(16);
    CHECK_FALSE(render_map(db, map_view{0, 0, 0, 0, 1, 0}, p, "", out));
    CHECK_FALSE(render_map(db, map_view{0, 0, 32, 32, 1, 0}, p, "", out));  // buffer too small
  }
}

TEST_CASE("places: pictures are cached by point, size, theme and zoom", "[places]") {
  const auto& db = test_db();
  map_cache cache(4);
  map_view v{38.7967, -9.3903, 300, 160, 1.0f, 4};
  const auto a = cache.get(db, v, palette_for(light_theme()), "PT");
  REQUIRE(a);
  CHECK(cache.misses() == 1);
  const auto b = cache.get(db, v, palette_for(light_theme()), "PT");
  CHECK(cache.hits() == 1);
  CHECK(a.get() == b.get());  // the same picture, not redrawn

  // A theme change is a different picture.
  const auto c = cache.get(db, v, palette_for(dark_theme()), "PT");
  CHECK(cache.misses() == 2);
  REQUIRE(c);
  CHECK(c->bgra != a->bgra);

  // So is a zoom step, and a different size.
  v.zoom = 3;
  (void)cache.get(db, v, palette_for(light_theme()), "PT");
  v.width = 320;
  (void)cache.get(db, v, palette_for(light_theme()), "PT");
  CHECK(cache.misses() == 4);
  CHECK(cache.size() == 4);
  // Capacity holds; the oldest goes first.
  v.lat = 41.1413;
  v.lon = -8.6110;
  (void)cache.get(db, v, palette_for(light_theme()), "PT");
  CHECK(cache.size() == 4);
  cache.clear();
  CHECK(cache.size() == 0);
}

TEST_CASE("places: the map opens framed on the region", "[places]") {
  const auto& db = test_db();
  const auto sintra = db.lookup(38.7967, -9.3903);
  const int z = framing_zoom(db, 38.7967, -9.3903, sintra.country_code, sintra.region_code, 600, 320);
  // Lisbon district is about a degree tall: a region step, not the country.
  CHECK(z >= 3);
  // Open sea, nothing to frame: a wide step.
  CHECK(framing_zoom(db, 38.0, -14.0, "", "", 600, 320) == 2);
}

TEST_CASE("places: the Place section is a label, the coordinates, the map and its zoom", "[places]") {
  engine eng([] { return &test_db(); });
  section_state st;
  const char* coords = "38.79670\xC2\xB0 N, 9.39030\xC2\xB0 W";
  auto out = eng.section(38.7967, -9.3903, coords, light_theme(), 300, 2, settings{}, st);
  CHECK(st.place.label == "Sintra, Lisbon, Portugal");
  const auto tree = mv::json::parse(out.tree_json);
  REQUIRE(tree);
  const auto& kids = tree->find("children")->a;
  REQUIRE(kids.size() == 4);
  CHECK(*kids[0].str("text") == "Sintra, Lisbon, Portugal");
  CHECK(*kids[1].str("text") == coords);
  CHECK(kids[1].boolean("copyable") == true);
  CHECK(*kids[2].str("type") == "surface");
  CHECK(kids[2].integer("height") == 160);
  CHECK(*kids[2].str("label") == "Map of Sintra, Lisbon, Portugal");  // one element, named by the place
  CHECK(*kids[2].str("pin_label") == "Sintra");
  const auto& zoom_row = kids[3].find("children")->a;
  CHECK(*zoom_row[0].str("text") == std::string(k_map_caption));
  CHECK(*zoom_row[1].str("id") == "zoom_out");
  CHECK(*zoom_row[2].str("id") == "zoom_in");
  REQUIRE(out.map);
  CHECK(out.map->width == 600);   // width_pt x scale
  CHECK(out.map->height == 320);  // 160 pt x scale

  // Zoom in to the region step, then out to the country step; no further.
  const int start = st.zoom;
  std::shared_ptr<const map_image> map;
  std::string diff;
  int steps_in = 0;
  while (eng.zoom(st, +1, map, diff)) ++steps_in;
  CHECK(st.zoom == k_zoom_steps - 1);
  CHECK(steps_in == k_zoom_steps - 1 - start);
  if (steps_in > 0) CHECK(diff.find(R"("id":"zoom_in","props":{"disabled":true})") != std::string::npos);
  while (eng.zoom(st, -1, map, diff)) {
  }
  CHECK(st.zoom == 0);
  CHECK(diff.find(R"("id":"zoom_out","props":{"disabled":true})") != std::string::npos);
  CHECK_FALSE(eng.zoom(st, -1, map, diff));

  // Settings: no town, no map.
  settings prefs;
  prefs.show_town = false;
  prefs.show_map = false;
  out = eng.section(38.7967, -9.3903, coords, light_theme(), 300, 2, prefs, st);
  CHECK(st.place.label == "Lisbon, Portugal");
  CHECK_FALSE(out.map);
  CHECK(out.tree_json.find("surface") == std::string::npos);
  CHECK_FALSE(eng.zoom(st, +1, map, diff));

  // No database: the coordinates, and an honest line instead of a place.
  engine none([] { return static_cast<const mv::geo::database*>(nullptr); });
  out = none.section(38.7967, -9.3903, coords, light_theme(), 300, 2, settings{}, st);
  CHECK_FALSE(out.map);
  CHECK(out.tree_json.find("Place names are unavailable") != std::string::npos);
  CHECK(out.tree_json.find("38.79670") != std::string::npos);
}

TEST_CASE("places: search by place finds the files taken there", "[places]") {
  const auto& db = test_db();
  std::vector<std::string> whys;
  const auto pt = search(db, "portugal", &whys);
  CHECK(pt == std::vector<std::string>{"/trip/sintra.jpg", "/trip/lisboa.heic", "/trip/porto.mov",
                                       "/trip/funchal.jpg", "/trip/ponta-delgada.jpg", "/trip/elvas.jpg"});
  REQUIRE(whys.size() == pt.size());
  CHECK(whys[0] == "Sintra, Lisbon, Portugal");  // each result says where
  // Typed with its accents or capitals, the host's fold gives the same word.
  CHECK(search(db, "PORTUGAL") == pt);
  // While typing: 4 letters start a name, 2 do not.
  CHECK(search(db, "portu") == pt);
  CHECK(search(db, "po").empty());
  // A region, a town, a country.
  CHECK(search(db, "lisbon") == std::vector<std::string>{"/trip/sintra.jpg", "/trip/lisboa.heic"});
  CHECK(search(db, "sintra") == std::vector<std::string>{"/trip/sintra.jpg"});
  CHECK(search(db, "spain") == std::vector<std::string>{"/trip/badajoz.jpg", "/trip/llivia.jpg"});
  CHECK(search(db, "fiji") == std::vector<std::string>{"/trip/suva.jpg"});
  // A file at sea off Portugal is not "in" Portugal; a date is not a place.
  CHECK(std::find(pt.begin(), pt.end(), "/trip/at-sea.jpg") == pt.end());
  CHECK(place_query::compile(db, "2024").empty());
  CHECK(place_query::compile(db, " ").empty());
}

TEST_CASE("places: map render time at pane width x 160 pt", "[.perf-bench]") {
  auto opened = mv::geo::open_database(full_blob());
  if (!opened) {
    SKIP("no full place database; build it with: cmake --build build --target mv_geo_data");
  }
  const auto& db = (*opened)->db();
  struct where {
    const char* name;
    double lat, lon;
  };
  const where places[] = {{"Sintra", 38.7967, -9.3903},  {"Paris", 48.8584, 2.2945},
                          {"Denver", 39.7392, -104.9903}, {"Moscow", 55.7558, 37.6173},
                          {"Tokyo", 35.6595, 139.7005},   {"Suva", -18.1416, 178.4419},
                          {"Cape Town", -33.9249, 18.4241}, {"Mid-Atlantic", 30.0, -40.0}};
  const map_palette p = palette_for(light_theme());
  for (float scale : {1.0f, 2.0f}) {
    std::vector<double> ms;
    std::vector<double> framing_ms;
    for (const where& w : places) {
      const auto pl = db.lookup(w.lat, w.lon);
      for (int z = 0; z < k_zoom_steps; ++z) {
        map_view v{w.lat, w.lon, static_cast<std::uint32_t>(320 * scale), static_cast<std::uint32_t>(160 * scale),
                   scale, z};
        std::vector<std::uint8_t> out(static_cast<std::size_t>(v.width) * v.height * 4);
        const auto t0 = std::chrono::steady_clock::now();
        REQUIRE(render_map(db, v, p, pl.country_code, out));
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
      }
      const auto t0 = std::chrono::steady_clock::now();
      (void)framing_zoom(db, w.lat, w.lon, pl.country_code, pl.region_code, static_cast<std::uint32_t>(320 * scale),
                         static_cast<std::uint32_t>(160 * scale));
      framing_ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::sort(ms.begin(), ms.end());
    std::sort(framing_ms.begin(), framing_ms.end());
    std::printf("places map %gx (%ux%u px), %zu renders: median %.2f ms, p90 %.2f ms, max %.2f ms; "
                "framing max %.3f ms\n",
                scale, static_cast<unsigned>(320 * scale), static_cast<unsigned>(160 * scale), ms.size(),
                ms[ms.size() / 2], ms[ms.size() * 9 / 10], ms.back(), framing_ms.back());
  }
  // A cache hit, for the arrow-back case.
  map_cache cache;
  map_view v{38.7967, -9.3903, 640, 320, 2.0f, 4};
  (void)cache.get(db, v, p, "PT");
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 1000; ++i) (void)cache.get(db, v, p, "PT");
  std::printf("places map cache hit: %.3f us\n",
              std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 1000);
}

TEST_CASE("places: dump the map for a person to look at", "[.places-map-dump]") {
  const char* dir = std::getenv("MV_PLACES_MAP_DUMP");
  if (!dir || !*dir) SKIP("set MV_PLACES_MAP_DUMP=<dir>");
  auto opened = mv::geo::open_database(full_blob());
  if (!opened) SKIP("no full place database");
  const auto& db = (*opened)->db();
  std::filesystem::create_directories(dir);
  engine eng([&db] { return &db; });
  struct where {
    const char* name;
    double lat, lon;
  };
  const where places[] = {{"sintra", 38.7967, -9.3903},  {"elvas", 38.8800, -7.1630},  {"paris", 48.8584, 2.2945},
                          {"denver", 39.7392, -104.9903}, {"suva", -18.1416, 178.4419}, {"tokyo", 35.6595, 139.7005},
                          {"sea", 38.0, -14.0}};
  for (const where& w : places) {
    for (int dark = 0; dark < 2; ++dark) {
      section_state st;
      auto out = eng.section(w.lat, w.lon, "", dark ? dark_theme() : light_theme(), 320, 2, settings{}, st);
      REQUIRE(out.map);
      char name[128];
      std::snprintf(name, sizeof name, "%s-%s-open%d.bmp", w.name, dark ? "dark" : "light", st.zoom);
      write_bmp(std::filesystem::path(dir) / name, *out.map);
      std::shared_ptr<const map_image> map;
      std::string diff;
      while (eng.zoom(st, -1, map, diff)) {
      }
      for (;;) {
        std::snprintf(name, sizeof name, "%s-%s-z%d.bmp", w.name, dark ? "dark" : "light", st.zoom);
        const auto img = eng.cache().get(db, map_view{w.lat, w.lon, 640, 320, 2.0f, st.zoom},
                                         palette_for(dark ? dark_theme() : light_theme()),
                                         st.place.at_sea ? "" : st.place.country_code);
        write_bmp(std::filesystem::path(dir) / name, *img);
        if (!eng.zoom(st, +1, map, diff)) break;
      }
    }
  }
}
