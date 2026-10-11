// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Offline reverse geocoder: coordinates -> "Sintra, Lisbon, Portugal"
// (docs/design/06 "Place"; plan: docs/plans/metadata-search-and-place.md slice 2).
//
// Rule 6: everything is computed from the bundled place database, a
// memory-mapped blob built by tools/geo/build_places.py from Natural Earth
// (public domain) and GeoNames (CC-BY 4.0). No geocoder service, no network.
//
// Portable (D9): no platform API here. The database reads bytes it is given;
// geo/open.h maps a file through io/. A database is immutable once opened, so
// every const call is safe from any number of worker threads at once. None of
// it belongs on the UI or render thread: opening is I/O, and a lookup is tens
// of microseconds.
//
// With no database (the Places add-on not installed, a missing or damaged
// file) there is no place: callers keep showing coordinates, as before.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "geo/blob.h"

namespace mv::geo {

// The label rule's constants (docs/design/06 "Place").
inline constexpr double k_town_radius_km = 15.0;  // nearest town within this, else none
inline constexpr double k_off_coast_km = 25.0;    // "At sea, off <country>" within this of land
// A point this close outside every country polygon still counts as land in the
// nearest one. The polygons are simplified (~1 km on coasts, ~250 m on
// borders), so a beach, a harbour wall or a pier can fall just outside them.
inline constexpr double k_coast_snap_km = 1.5;

struct lookup_options {
  double town_radius_km = k_town_radius_km;  // <= 0: never name a town
  bool native_names = false;  // the country's own name ("España") where the data has it
};

struct place {
  std::string town;     // "Sintra"; empty when no town is near enough or at sea
  std::string region;   // "Lisbon" (GeoNames admin-1); empty when none applies
  std::string country;  // "Portugal"; at sea, the country it is off, if any
  std::string country_code;  // ISO 3166-1 alpha-2 ("PT"); empty for open sea or an area with none
  std::string region_code;   // GeoNames admin-1 ("PT.14"); empty when none
  bool at_sea = false;
  double town_km = -1;  // distance to `town`, or -1
  // What the pane shows: "Sintra, Lisbon, Portugal", "Douro, Portugal",
  // "At sea, off Portugal", "At sea". Never empty for a valid lookup.
  std::string label;
};

enum class name_kind : std::uint8_t { country, region };

struct name_match {
  name_kind kind = name_kind::country;
  bool exact = true;          // the whole name matched (false: a prefix of it)
  std::string country_code;   // "PT" (a region's country for a region)
  std::string region_code;    // "PT.14" for a region; empty for a country
  std::string name;           // the display name ("Portugal", "Lisbon")
};

enum class layer : std::uint8_t { countries = 0, regions = 1 };

// A bounding box in degrees. min_lon > max_lon means it crosses the antimeridian.
struct box {
  double min_lat = -90, min_lon = -180, max_lat = 90, max_lon = 180;
};

using point = blob::point;  // int32 microdegrees: x = longitude, y = latitude

class database {
 public:
  database() noexcept = default;
  database(database&&) noexcept = default;
  database& operator=(database&&) noexcept = default;
  database(const database&) = delete;
  database& operator=(const database&) = delete;
  ~database() = default;

  // Validates and indexes `bytes`, which must outlive the database and start
  // 8-byte aligned (a mapping always does). Every offset, count and index the
  // lookups use is bounds-checked here, once, and the checksum is verified;
  // a truncated, damaged or foreign file is status::corrupt (or
  // unsupported_format for a newer version), never a crash.
  [[nodiscard]] static result<database> from_bytes(std::span<const std::uint8_t> bytes);

  // The place at (lat, lon) in degrees (WGS 84). Longitude is wrapped,
  // latitude clamped to the poles. NaN or infinite input gives an empty place
  // (label "").
  [[nodiscard]] place lookup(double lat, double lon, const lookup_options& opts = {}) const;

  // Countries and regions a typed word names, matched on folded names (fold.h)
  // in English, the local name, and Natural Earth's ~25 languages
  // ("portugal", "Portugália", "españa", "lisboa"). `text` is raw; it is
  // folded here. `prefix` also returns names the text starts (typeahead).
  [[nodiscard]] std::vector<name_match> match(std::string_view text, bool prefix = false) const;

  // ---- for the map (plan slice 3) ----
  // Polygons of a layer whose bounding box meets `b`.
  [[nodiscard]] std::vector<std::uint32_t> polygons_in(layer l, const box& b) const;
  [[nodiscard]] std::size_t polygon_count(layer l) const noexcept;
  [[nodiscard]] std::size_t ring_count(layer l, std::uint32_t polygon) const noexcept;
  // A closed ring (last point == first). Rings after the first may be holes;
  // fill with the even-odd rule.
  [[nodiscard]] std::span<const point> ring(layer l, std::uint32_t polygon, std::size_t i) const noexcept;
  // The ISO code ("PT") of the country that owns a polygon (a region's country).
  [[nodiscard]] std::string_view polygon_country_code(layer l, std::uint32_t polygon) const noexcept;

  // Counts, for tests and the About line.
  [[nodiscard]] std::size_t country_count() const noexcept { return countries_.size(); }
  [[nodiscard]] std::size_t region_count() const noexcept { return regions_.size(); }
  [[nodiscard]] std::size_t town_count() const noexcept { return towns_.size(); }
  // The build's provenance (JSON: inputs and their SHA-256, attribution).
  [[nodiscard]] std::string_view meta() const noexcept { return meta_; }

  [[nodiscard]] bool valid() const noexcept { return !countries_.empty(); }

 private:
  struct grid {
    std::span<const blob::row> rows;
    std::span<const std::uint32_t> cells;
  };
  struct poly_layer {
    std::span<const blob::polygon> polys;
    std::span<const std::uint32_t> rings;
    std::span<const blob::point> points;
    std::span<const std::uint32_t> bands;
    std::span<const std::uint32_t> edges;
    grid index;
    std::span<const std::uint32_t> cands;
  };
  struct nearest_hit {
    std::uint32_t polygon = UINT32_MAX;
    double km = 0;
  };
  struct name_entry {
    std::string folded;
    name_kind kind;
    std::uint16_t index;
  };

  [[nodiscard]] std::string_view str(std::uint32_t offset) const noexcept;
  [[nodiscard]] std::span<const std::uint32_t> cell(const grid& g, int row, int col) const noexcept;
  [[nodiscard]] bool contains(const poly_layer& l, std::uint32_t p, std::int32_t x, std::int32_t y) const noexcept;
  [[nodiscard]] std::uint32_t find(const poly_layer& l, std::int32_t x, std::int32_t y) const noexcept;
  [[nodiscard]] nearest_hit nearest(const poly_layer& l, std::int32_t x, std::int32_t y, double km) const;
  [[nodiscard]] const blob::town* nearest_town(std::int32_t x, std::int32_t y, double km, std::uint16_t country,
                                               double& out_km) const noexcept;
  [[nodiscard]] std::string country_code(std::uint16_t c) const;
  [[nodiscard]] const poly_layer& layer_of(layer l) const noexcept { return l == layer::countries ? l0_ : l1_; }

  std::span<const char> strings_;
  std::span<const blob::country> countries_;
  std::span<const blob::region> regions_;
  std::span<const blob::town> towns_;
  grid town_grid_;
  poly_layer l0_, l1_;
  std::string_view meta_;
  std::vector<name_entry> names_;  // sorted by folded
};

}  // namespace mv::geo
