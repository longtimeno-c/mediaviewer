// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness: the Places add-on's map (src/addons/places, docs/design/27
// "The map"). The input is not a file but a stream of view, size, scale, zoom
// and gesture numbers, raw doubles included (NaN, infinities, 1e300), driving
// render_map, apply_gesture / keep_pin_in_view, the engine's section and its
// zoom / gesture steps, and the picture cache, over the committed test blob
// (tests/data/geo/places-test.mvgeo, read once). Whatever comes in, every
// write must stay inside the w x h x 4 buffer and every call must return.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "addons/places/map.h"
#include "addons/places/places.h"
#include "geo/geo.h"

#ifndef MV_FUZZ_GEO_BLOB
#define MV_FUZZ_GEO_BLOB "tests/data/geo/places-test.mvgeo"
#endif

namespace {

struct blob {
  std::vector<std::uint64_t> words;  // 8-byte aligned, as from_bytes wants
  mv::geo::database db;
};

const mv::geo::database* test_db() {
  static blob* b = [] {
    const char* env = std::getenv("MV_FUZZ_GEO_BLOB");
    std::ifstream f(env && *env ? env : MV_FUZZ_GEO_BLOB, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto* out = new blob;
    out->words.resize((raw.size() + 7) / 8);
    if (!raw.empty()) std::memcpy(out->words.data(), raw.data(), raw.size());
    auto db = mv::geo::database::from_bytes(
        {reinterpret_cast<const std::uint8_t*>(out->words.data()), raw.size()}, true);
    if (db) out->db = std::move(*db);
    return out;
  }();
  return b->db.valid() ? &b->db : nullptr;
}

// Reads the input front to back; past the end everything is zero.
struct reader {
  const std::uint8_t* p;
  std::size_t n;
  template <typename T>
  T take() {
    T v{};
    const std::size_t k = std::min(sizeof v, n);
    std::memcpy(&v, p, k);
    p += k;
    n -= k;
    return v;
  }
  // Mostly a sensible value in [lo, hi], sometimes the raw double.
  double number(double lo, double hi) {
    const std::uint8_t mode = take<std::uint8_t>();
    const double raw = take<double>();
    if (mode % 8 == 0) return raw;
    const double t = static_cast<double>(take<std::uint16_t>()) / 65535.0;
    return lo + (hi - lo) * t;
  }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const mv::geo::database* db = test_db();
  if (!db) std::abort();  // the seed blob must load
  reader in{data, size};
  using namespace mv::places;

  // render_map directly: a size up to 640 x 400 (sometimes past the 4096 cap),
  // a buffer sometimes one byte short.
  map_view v;
  v.lat = in.number(-90, 90);
  v.lon = in.number(-180, 180);
  const std::uint16_t wsel = in.take<std::uint16_t>();
  const std::uint16_t hsel = in.take<std::uint16_t>();
  v.width = wsel % 97 == 0 ? 4097u : static_cast<std::uint32_t>(wsel % 641);
  v.height = hsel % 89 == 0 ? 4097u : static_cast<std::uint32_t>(hsel % 401);
  v.scale = static_cast<float>(in.number(0.5, 3));
  v.span_deg = in.number(0.01, 60);
  const bool short_buffer = in.take<std::uint8_t>() % 16 == 0;
  std::size_t bytes = static_cast<std::size_t>(v.width) * v.height * 4;
  if (v.width <= 640 && v.height <= 400) {
    if (short_buffer && bytes) --bytes;
    std::vector<std::uint8_t> out(bytes);
    std::vector<map_mark> marks;
    theme_tokens t;
    t.surface = in.take<std::uint32_t>();
    t.canvas = in.take<std::uint32_t>();
    t.dark = in.take<std::uint8_t>() & 1;
    (void)render_map(*db, v, palette_for(t), in.take<std::uint8_t>() & 1 ? "PT" : "", out, &marks);
    for (const map_mark& m : marks) {
      volatile std::size_t len = m.text.size();
      (void)len;
    }
  }

  // The view maths alone, on raw input.
  const double pin_lat = in.number(-90, 90);
  const double pin_lon = in.number(-180, 180);
  const map_view g = apply_gesture(v, in.number(0.05, 20), in.number(0, 1), in.number(0, 1), in.number(-10, 10),
                                   in.number(-10, 10), pin_lat, pin_lon);
  (void)keep_pin_in_view(g, pin_lat, pin_lon, in.number(0, 0.5));
  double x = 0, y = 0;
  map_pixel(v, pin_lat, pin_lon, x, y);
  map_point(v, x, y, x, y);
  if (std::isfinite(pin_lat) && std::isfinite(pin_lon)) {
    (void)framing_zoom(*db, pin_lat, pin_lon, "PT", "", v.width, v.height);
  }

  // The engine as the add-on drives it: a section for an in-range point (the
  // host's contract), then a few settled gestures and button presses, small
  // so each run stays quick.
  static engine eng([db] { return db; });
  section_state st;
  settings prefs;
  prefs.show_map = in.take<std::uint8_t>() % 8 != 0;
  prefs.show_town = in.take<std::uint8_t>() & 1;
  prefs.town_radius_km = in.number(1, 50);
  const double lat = std::clamp(std::isfinite(pin_lat) ? pin_lat : 0.0, -90.0, 90.0);
  const double lon = std::clamp(std::isfinite(pin_lon) ? pin_lon : 0.0, -180.0, 180.0);
  const float width_pt = static_cast<float>(in.number(0, 400));
  const float scale = static_cast<float>(in.number(0, 2));
  (void)eng.section(lat, lon, "", theme_tokens{}, width_pt, scale, prefs, st);
  const int steps = in.take<std::uint8_t>() % 6;
  for (int i = 0; i < steps; ++i) {
    std::shared_ptr<const map_image> map;
    std::string diff;
    if (in.take<std::uint8_t>() & 1) {
      (void)eng.zoom(st, in.take<std::uint8_t>() & 1 ? 1 : -1, map, diff);
    } else {
      (void)eng.gesture(st, in.number(0.05, 20), in.number(0, 1), in.number(0, 1), in.number(-10, 10),
                        in.number(-10, 10), map, diff);
    }
    if (map && map->bgra.size() != static_cast<std::size_t>(map->width) * map->height * 4) std::abort();
  }
  return 0;
}
