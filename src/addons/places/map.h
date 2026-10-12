// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Places map: land, country borders and (at region zoom) admin-1 borders,
// rasterised from the place database's Natural Earth polygons into 8-bit
// BGRA on a worker (docs/design/27 "The map"; plan slice 3).
//
// Rule 6: no tiles, no network. The picture is drawn from the polygons the
// add-on ships, so the photo's coordinates never leave the machine.
//
// The map is centred on the point (the chrome draws the pin there, in the
// accent colour, so it is always at the surface's centre), in Web Mercator,
// at one of a few fixed zoom steps from country scale down to region scale.
// There is no street level: the data has none.
//
// Portable C++ (D9), no platform API, no allocation beyond the output and one
// coverage buffer per call. Thread-safe: a database is immutable, and the
// cache below locks.
#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "geo/geo.h"

namespace mv::places {

// The host's theme tokens, as mediaviewer_addon.h's mv_slot_theme carries
// them (0xRRGGBBAA). Mirrored here so the rasteriser has no ABI dependency.
struct theme_tokens {
  std::uint32_t canvas = 0xffffffff;
  std::uint32_t surface = 0xf5f5f7ff;
  std::uint32_t title = 0x1d1d1fff;
  std::uint32_t body = 0x3c3c43ff;
  std::uint32_t disabled = 0x8e8e93ff;
  std::uint32_t hairline = 0xd1d1d6ff;
  std::uint32_t accent = 0x0a84ffff;
  bool dark = false;
};

// The map's own colours, each 0xRRGGBB (opaque), derived from the theme.
struct map_palette {
  std::uint32_t water = 0;
  std::uint32_t land = 0;         // countries other than the point's
  std::uint32_t land_home = 0;    // the point's own country
  std::uint32_t border = 0;       // land borders between countries
  std::uint32_t region = 0;       // admin-1 borders
  std::uint32_t frame = 0;        // the hairline round the map

  [[nodiscard]] bool operator==(const map_palette&) const = default;
};

// Water, land, borders and frame from the host's tokens, for light or dark:
// the land lifts toward the pane's surface, the water is a quiet blue mixed
// into it, and borders are the body text colour at low weight.
[[nodiscard]] map_palette palette_for(const theme_tokens& t) noexcept;

// Zoom steps: the map's height in degrees of latitude at its centre. Step 0
// is country scale, the last is region scale.
inline constexpr double k_zoom_span_deg[] = {36.0, 18.0, 9.0, 4.5, 2.25, 1.2};
inline constexpr int k_zoom_steps = static_cast<int>(sizeof k_zoom_span_deg / sizeof k_zoom_span_deg[0]);
// Region borders are drawn from this step down (span <= 9 degrees) when the
// picture holds few regions (US states, Portuguese districts), and from the
// next step where it would hold many (French départements, Spanish provinces):
// a map ruled into a hundred cells reads as noise, not as regions.
inline constexpr int k_region_borders_from = 2;
inline constexpr std::size_t k_region_borders_max_early = 48;

struct map_view {
  double lat = 0;
  double lon = 0;
  std::uint32_t width = 0;   // pixels
  std::uint32_t height = 0;  // pixels
  float scale = 1;           // backing scale: line widths and the corner radius are in points
  int zoom = 3;              // 0 .. k_zoom_steps - 1
};

// The step that frames the point's region (or, with none, its country) with a
// margin: what the map opens at. Clamped to the region step at most.
[[nodiscard]] int framing_zoom(const geo::database& db, double lat, double lon, std::string_view country_code,
                               std::string_view region_code, std::uint32_t width, std::uint32_t height);

// Draws the map into `out` (width * height * 4 bytes, BGRA, premultiplied,
// sRGB; rounded corners are transparent). `home_country` (ISO code, may be
// empty) is drawn in land_home. Returns false only for a bad size.
bool render_map(const geo::database& db, const map_view& v, const map_palette& p, std::string_view home_country,
                std::span<std::uint8_t> out);

// Where (lat, lon) falls on the picture `v` describes, in pixels from the
// top-left (may be outside it). For tests and for a host that wants to place
// something other than the centre pin.
void map_pixel(const map_view& v, double lat, double lon, double& x, double& y) noexcept;

// A finished picture, shared between the cache and whoever posts it.
struct map_image {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> bgra;
};

// Pictures by (point, size, theme, zoom). Small: the pane shows one file, and
// arrowing back and forth or zooming out and in again should not redraw.
// A theme change is a new key, so the old pictures simply age out; `clear`
// drops them at once when the host says the theme changed.
class map_cache {
 public:
  explicit map_cache(std::size_t capacity = 12) : capacity_(capacity) {}

  // The picture for `v` / `p`, drawn now on the calling worker on a miss.
  [[nodiscard]] std::shared_ptr<const map_image> get(const geo::database& db, const map_view& v,
                                                     const map_palette& p, std::string_view home_country);
  void clear();

  [[nodiscard]] std::uint64_t hits() const;
  [[nodiscard]] std::uint64_t misses() const;
  [[nodiscard]] std::size_t size() const;

 private:
  struct key {
    std::int64_t lat_e6 = 0, lon_e6 = 0;
    std::uint32_t width = 0, height = 0;
    std::int32_t scale_x100 = 0;
    int zoom = 0;
    map_palette palette;
    std::string home;
    [[nodiscard]] bool operator==(const key&) const = default;
  };
  struct entry {
    key k;
    std::shared_ptr<const map_image> image;
  };
  mutable std::mutex m_;
  std::list<entry> lru_;  // most recent first
  std::size_t capacity_;
  std::uint64_t hits_ = 0;
  std::uint64_t misses_ = 0;
};

}  // namespace mv::places
