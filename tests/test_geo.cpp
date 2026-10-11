// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The offline reverse geocoder (docs/design/06 "Place", src/geo).
//
//   mv_tests "[geo]"           always: fold, the committed test blob
//                              (tests/data/geo, a few regions around Portugal,
//                              Llívia, Baarle, Fiji and Tarawa), a damaged-blob
//                              sweep, and the full 300-row table when the full
//                              blob has been built (cmake --build build
//                              --target mv_geo_data); MV_REQUIRE_GEO=1 makes
//                              its absence a failure instead of a skip.
//   mv_tests "[.perf-bench]"   load and lookup timings on the full blob.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "catch_compat.h"

#include "geo/fold.h"
#include "geo/geo.h"
#include "geo/open.h"
#include "io/file.h"

namespace {

struct table_row {
  double lat, lon;
  const char* country;  // ISO code, "SEA", "<cc>SEA", "SEA*"
  const char* region;   // exact, "*" any, "-" none
  const char* town;
  const char* what;
};

constexpr table_row k_table[] = {
#include "geo_places_table.inc"
};

// Rows inside the committed test blob's boxes (build_places.py SUBSETS["test"]).
constexpr const char* k_test_blob_rows[] = {
    "Sintra, Palácio da Vila",
    "Lisbon, Praça do Comércio",
    "Belém tower",
    "Cascais",
    "Porto, Ribeira",
    "Douro valley vineyards near Pinhão",
    "Funchal, Madeira",
    "Porto Santo",
    "Ponta Delgada, São Miguel",
    "Horta, Faial",
    "Angra do Heroísmo, Terceira",
    "Elvas, 10 km from the border",
    "Badajoz, across from Elvas",
    "Llívia enclave (Spain inside France)",
    "France just south-west of Llívia",
    "Baarle-Nassau centre",
    "Taveuni, west of 180",
    "Taveuni, east of 180",
    "Taveuni, longitude given as -539.95",
    "Bairiki, Tarawa",
    "Off Cabo da Roca, 14 km out",
    "Off Madeira",
    "Mid-Atlantic",
};

std::string check_row(const mv::geo::database& db, const table_row& r) {
  const mv::geo::place p = db.lookup(r.lat, r.lon);
  const std::string want = r.country;
  bool ok = true;
  if (want == "SEA") {
    ok = p.at_sea && p.country_code.empty();
  } else if (want == "SEA*") {
    ok = p.at_sea;
  } else if (want.size() == 5 && want.substr(2) == "SEA") {
    ok = p.at_sea && p.country_code == want.substr(0, 2);
  } else {
    ok = !p.at_sea && p.country_code == want;
    const std::string reg = r.region, town = r.town;
    if (reg == "-") ok = ok && p.region.empty();
    else if (reg != "*") ok = ok && p.region == reg;
    if (town == "-") ok = ok && p.town.empty();
    else if (town != "*") ok = ok && p.town == town;
  }
  ok = ok && !p.label.empty();
  if (ok) return {};
  char buf[512];
  std::snprintf(buf, sizeof buf, "%s (%.4f, %.4f): want %s / %s / %s, got \"%s\" [%s %s]", r.what, r.lat, r.lon,
                r.country, r.region, r.town, p.label.c_str(), p.country_code.c_str(), p.region_code.c_str());
  return buf;
}

std::vector<std::uint8_t> read_blob(const char* path) {
  auto bytes = mv::io::read_all(path);
  REQUIRE(bytes.has_value());
  return std::move(bytes.value());
}

// from_bytes needs 8-byte alignment; a vector<uint8_t> gives no promise.
struct aligned_copy {
  std::vector<std::uint64_t> words;
  std::size_t size = 0;
  explicit aligned_copy(const std::vector<std::uint8_t>& b) : words((b.size() + 7) / 8), size(b.size()) {
    if (size) std::memcpy(words.data(), b.data(), size);
  }
  std::span<const std::uint8_t> span(std::size_t n) const {
    return {reinterpret_cast<const std::uint8_t*>(words.data()), std::min(n, size)};
  }
  std::uint8_t* data() { return reinterpret_cast<std::uint8_t*>(words.data()); }
};

}  // namespace

TEST_CASE("geo: fold matches the hosts' name fold", "[geo]") {
  using mv::geo::fold;
  CHECK(fold("Portugal") == "portugal");
  CHECK(fold("PORTUGÁLIA") == "portugalia");
  CHECK(fold("Portuga\xCC\x81lia") == "portugalia");  // decomposed: a + U+0301
  CHECK(fold("España") == "espana");
  CHECK(fold("Île-de-France") == "ile-de-france");
  CHECK(fold("Łódź") == "łodz");  // ł has no decomposition: lowercased, kept (as on the hosts)
  CHECK(fold("Øresund") == "øresund");
  CHECK(fold("Straße") == "straße");
  CHECK(fold("Θεσσαλονίκη") == "θεσσαλονικη");
  CHECK(fold("МОСКВА") == "москва");
  CHECK(fold("Việt Nam") == "viet nam");
  CHECK(fold("İstanbul") == "istanbul");
  CHECK(fold("東京") == "東京");
  CHECK(fold(std::string("a\xFF" "b")) == std::string("a\xFF" "b"));  // invalid UTF-8 passes through
  CHECK(fold(fold("Ångström")) == fold("Ångström"));                 // idempotent
}

TEST_CASE("geo: the test blob loads and answers", "[geo]") {
  const auto bytes = read_blob(MV_GEO_TEST_BLOB);
  const aligned_copy buf(bytes);
  auto db = mv::geo::database::from_bytes(buf.span(bytes.size()));
  REQUIRE(db.has_value());
  CHECK(db->country_count() > 0);
  CHECK(db->town_count() > 0);
  CHECK(db->meta().find("GeoNames") != std::string_view::npos);

  SECTION("hand-checked rows inside the test boxes") {
    int n = 0;
    for (const char* what : k_test_blob_rows) {
      const auto* r = std::find_if(std::begin(k_table), std::end(k_table),
                                   [&](const table_row& t) { return std::strcmp(t.what, what) == 0; });
      REQUIRE(r != std::end(k_table));
      const std::string bad = check_row(*db, *r);
      CHECK(bad.empty());
      if (!bad.empty()) UNSCOPED_INFO(bad);
      ++n;
    }
    CHECK(n == static_cast<int>(std::size(k_test_blob_rows)));
  }

  SECTION("the label rule") {
    const mv::geo::place sintra = db->lookup(38.7967, -9.3903);
    CHECK(sintra.label == "Sintra, Lisbon, Portugal");
    CHECK(sintra.country_code == "PT");
    CHECK(sintra.region_code == "PT.14");
    CHECK(sintra.town_km >= 0);
    CHECK(sintra.town_km < 2);

    // No town within the radius: region and country only.
    mv::geo::lookup_options no_town;
    no_town.town_radius_km = 0;
    CHECK(db->lookup(38.7967, -9.3903, no_town).label == "Lisbon, Portugal");

    CHECK(db->lookup(38.78, -9.65).label == "At sea, off Portugal");
    CHECK(db->lookup(0, -30).label == "At sea");

    mv::geo::lookup_options native;
    native.native_names = true;
    CHECK(db->lookup(38.8786, -6.9703, native).country == "España");
    CHECK(db->lookup(38.8786, -6.9703).country == "Spain");

    // Bad input never throws or crashes; NaN gives an empty place.
    CHECK(db->lookup(std::nan(""), 0).label.empty());
    CHECK(db->lookup(0, INFINITY).label.empty());
    CHECK_FALSE(db->lookup(1e6, -1e6).label.empty());  // clamped and wrapped
  }

  SECTION("names people type") {
    auto has = [](const std::vector<mv::geo::name_match>& v, const char* cc, const char* rc) {
      return std::any_of(v.begin(), v.end(), [&](const mv::geo::name_match& m) {
        return m.country_code == cc && m.region_code == rc;
      });
    };
    CHECK(has(db->match("portugal"), "PT", ""));
    CHECK(has(db->match("  PORTUGAL "), "PT", ""));
    CHECK(has(db->match("Portugália"), "PT", ""));  // Natural Earth's Hungarian name
    CHECK(has(db->match("espana"), "ES", ""));
    CHECK(has(db->match("lisbon"), "PT", "PT.14"));
    CHECK(has(db->match("madeira"), "PT", "PT.10"));
    CHECK(db->match("portu").empty());
    CHECK(has(db->match("portu", true), "PT", ""));
    CHECK_FALSE(db->match("portu", true).front().exact);
    CHECK(db->match("").empty());
    CHECK(db->match("PT").empty());  // codes are not names
  }

  SECTION("polygons for the map") {
    const auto polys = db->polygons_in(mv::geo::layer::countries, {38.0, -10.0, 39.5, -8.0});
    REQUIRE_FALSE(polys.empty());
    bool portugal = false;
    for (std::uint32_t p : polys) {
      if (db->polygon_country_code(mv::geo::layer::countries, p) == "PT") portugal = true;
      REQUIRE(db->ring_count(mv::geo::layer::countries, p) > 0);
      const auto ring = db->ring(mv::geo::layer::countries, p, 0);
      REQUIRE(ring.size() >= 4);
      CHECK(ring.front().x == ring.back().x);
      CHECK(ring.front().y == ring.back().y);
    }
    CHECK(portugal);
    CHECK(db->ring(mv::geo::layer::countries, 0xFFFFFF, 0).empty());
    // Across the antimeridian (min_lon > max_lon): Fiji's halves.
    CHECK_FALSE(db->polygons_in(mv::geo::layer::countries, {-17.5, 179.0, -16.0, -179.0}).empty());
  }
}

TEST_CASE("geo: a damaged or foreign blob fails safely", "[geo]") {
  const auto bytes = read_blob(MV_GEO_TEST_BLOB);
  aligned_copy buf(bytes);

  // Truncated anywhere: rejected (the header names the length).
  for (std::size_t n : {std::size_t{0}, std::size_t{7}, std::size_t{63}, std::size_t{64}, std::size_t{200},
                        bytes.size() / 2, bytes.size() - 8, bytes.size() - 1}) {
    CHECK_FALSE(mv::geo::database::from_bytes(buf.span(n)).has_value());
    CHECK_FALSE(mv::geo::database::from_bytes(buf.span(n), false).has_value());
  }
  // A flipped bit anywhere fails the checksum.
  buf.data()[bytes.size() / 3] ^= 0x10;
  CHECK(mv::geo::database::from_bytes(buf.span(bytes.size())).error() == mv::status::corrupt);
  buf.data()[bytes.size() / 3] ^= 0x10;
  // A newer format is unsupported, not corrupt.
  buf.data()[8] = 99;
  CHECK(mv::geo::database::from_bytes(buf.span(bytes.size())).error() == mv::status::unsupported_format);
  buf.data()[8] = 1;
  REQUIRE(mv::geo::database::from_bytes(buf.span(bytes.size())).has_value());

  // With the checksum off, random damage reaches the bounds checks: every
  // result is either a clean refusal or a database whose lookups stay inside
  // the buffer (ASan in CI proves the second half).
  std::mt19937 rng(344);
  int loaded = 0;
  for (int i = 0; i < 1500; ++i) {
    aligned_copy m(bytes);
    const int flips = 1 + static_cast<int>(rng() % 8);
    for (int k = 0; k < flips; ++k) {
      const std::size_t at = 64 + rng() % (bytes.size() - 64);
      m.data()[at] = static_cast<std::uint8_t>(rng());
    }
    auto db = mv::geo::database::from_bytes(m.span(bytes.size()), false);
    if (!db) continue;
    ++loaded;
    for (const auto& r : k_table) (void)db->lookup(r.lat, r.lon);
    (void)db->match("portugal", true);
    (void)db->polygons_in(mv::geo::layer::regions, {});
  }
  INFO("mutants that still loaded: " << loaded);
  SUCCEED();
}

TEST_CASE("geo: a missing blob means no places, quietly", "[geo]") {
  auto missing = mv::geo::open_database("/nonexistent/places.mvgeo");
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error() == mv::status::not_found);
  // The process-wide handle: no path, or a bad one, is nullptr, not an error.
  mv::geo::set_data_path("");
  CHECK(mv::geo::shared() == nullptr);
  mv::geo::set_data_path("/nonexistent/places.mvgeo");
  CHECK(mv::geo::shared() == nullptr);
  CHECK(mv::geo::shared() == nullptr);  // tried once; no retry storm
  mv::geo::set_data_path(MV_GEO_TEST_BLOB);
  const mv::geo::database* db = mv::geo::shared();
  REQUIRE(db != nullptr);
  CHECK(db->lookup(38.7967, -9.3903).country_code == "PT");
  CHECK(mv::geo::shared() == db);
  mv::geo::set_data_path("");
}

TEST_CASE("geo: 300 hand-checked coordinates on the full blob", "[geo]") {
  if (!std::filesystem::exists(MV_GEO_FULL_BLOB)) {
    const char* req = std::getenv("MV_REQUIRE_GEO");
    if (req && *req == '1') FAIL("MV_REQUIRE_GEO=1 and " MV_GEO_FULL_BLOB " is missing; build target mv_geo_data");
    SKIP("full place database not built (cmake --build build --target mv_geo_data)");
  }
  auto db = mv::geo::open_database(MV_GEO_FULL_BLOB);
  REQUIRE(db.has_value());
  int bad = 0;
  for (const table_row& r : k_table) {
    const std::string why = check_row((*db)->db(), r);
    if (!why.empty()) {
      ++bad;
      UNSCOPED_INFO(why);
    }
  }
  CHECK(bad == 0);
  CHECK(std::size(k_table) >= 200);
}

TEST_CASE("geo: load and lookup timings", "[.perf-bench]") {
  if (!std::filesystem::exists(MV_GEO_FULL_BLOB)) SKIP("full place database not built");
  using clock = std::chrono::steady_clock;
  std::vector<double> loads;
  for (int i = 0; i < 9; ++i) {
    const auto t0 = clock::now();
    auto db = mv::geo::open_database(MV_GEO_FULL_BLOB);
    loads.push_back(std::chrono::duration<double, std::milli>(clock::now() - t0).count());
    REQUIRE(db.has_value());
  }
  std::sort(loads.begin(), loads.end());
  auto db = mv::geo::open_database(MV_GEO_FULL_BLOB);
  REQUIRE(db.has_value());
  const mv::geo::database& d = (*db)->db();

  // Every table row, then a uniform sweep of the globe (mostly sea, the
  // slower path: it measures distance to the nearest coast).
  std::vector<std::pair<double, double>> pts;
  for (const auto& r : k_table) pts.emplace_back(r.lat, r.lon);
  std::mt19937 rng(7);
  std::uniform_real_distribution<double> lat(-85, 85), lon(-180, 180);
  for (int i = 0; i < 2000; ++i) pts.emplace_back(lat(rng), lon(rng));
  for (const auto& p : pts) (void)d.lookup(p.first, p.second);  // warm
  // Land (the table rows) and the sweep reported apart: a point far out at sea
  // finds no polygon cell and returns at once, which would flatter the median.
  const auto time_all = [&](std::size_t from, std::size_t to) {
    std::vector<double> us;
    for (std::size_t i = from; i < to; ++i) {
      const auto t0 = clock::now();
      const auto place = d.lookup(pts[i].first, pts[i].second);
      us.push_back(std::chrono::duration<double, std::micro>(clock::now() - t0).count());
      CHECK_FALSE(place.label.empty());
    }
    std::sort(us.begin(), us.end());
    return us;
  };
  const auto land = time_all(0, std::size(k_table));
  const auto sweep = time_all(std::size(k_table), pts.size());
  const auto pct = [](const std::vector<double>& v, int p) { return v[v.size() * static_cast<std::size_t>(p) / 100]; };
  std::printf("geo: load median %.2f ms (min %.2f, max %.2f)\n", loads[loads.size() / 2], loads.front(), loads.back());
  std::printf("geo: lookup, %zu table rows: p50 %.1f us, p99 %.1f us, max %.1f us\n", land.size(), pct(land, 50),
              pct(land, 99), land.back());
  std::printf("geo: lookup, %zu random points: p50 %.1f us, p99 %.1f us, max %.1f us\n", sweep.size(),
              pct(sweep, 50), pct(sweep, 99), sweep.back());
  const double p99 = std::max(pct(land, 99), pct(sweep, 99));
  CHECK(loads[loads.size() / 2] < 30.0);
  CHECK(p99 < 50.0);
}
