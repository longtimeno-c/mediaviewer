// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness: the place database loader (geo::database::from_bytes,
// docs/design/06 "Place") and every reader behind it. The checksum is off so
// mutations reach the bounds checks it would otherwise hide; whatever loads
// must then answer lookups, name matches and ring reads inside the buffer.
// Seed: tests/data/geo/places-test.mvgeo.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "geo/geo.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  // from_bytes wants the 8-byte alignment a mapping gives.
  std::vector<std::uint64_t> words((size + 7) / 8);
  if (size) std::memcpy(words.data(), data, size);
  const std::span<const std::uint8_t> bytes{reinterpret_cast<const std::uint8_t*>(words.data()), size};
  auto db = mv::geo::database::from_bytes(bytes, /*verify_checksum=*/false);
  if (!db) return 0;
  static constexpr double k_points[][2] = {
      {38.7967, -9.3903}, {32.65, -16.91}, {42.4642, 1.9806}, {51.44, 4.93}, {-16.8, 179.99},
      {-16.75, -179.95},  {1.329, 172.979}, {0, -30},          {90, 0},       {-90, 0},
  };
  for (const auto& p : k_points) (void)db->lookup(p[0], p[1]);
  (void)db->match("portugal", true);
  (void)db->match("lisboa");
  // The map's town dots (Places add-on), a whole-world box and one across 180.
  for (const auto& t : db->towns_in({}, 64)) {
    volatile std::size_t n = t.name.size();
    (void)n;
  }
  (void)db->towns_in(mv::geo::box{-18.5, 177.0, -16.0, -179.5}, 8);
  for (int l = 0; l < 2; ++l) {
    const auto layer = static_cast<mv::geo::layer>(l);
    for (std::uint32_t p : db->polygons_in(layer, {})) {
      for (std::size_t r = 0; r < db->ring_count(layer, p); ++r) {
        const auto ring = db->ring(layer, p, r);
        volatile std::int64_t sum = 0;
        for (const auto& pt : ring) sum = sum + pt.x + pt.y;
      }
      (void)db->polygon_country_code(layer, p);
    }
  }
  return 0;
}
