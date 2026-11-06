// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The folder map (#601; docs/design/27 "The folder map"): every geotagged
// item of the open folder as dots over the Place section's map. Dots cluster
// on a grid by zoom; the current item is the pin, not a dot; a click on a dot
// or a cluster opens that item, or the cluster's first (in folder order).
//
// Read-only: nothing here writes coordinates (picking a point on the map to
// write it is a different, backlogged feature, docs/design/06).
//
// Portable C++ (D9), no platform API. A layer over a finished picture: the
// base map stays in map_cache untouched, and the dots are drawn into a copy.
// Clustering is one pass over the items (the projection hoisted out of the
// loop) into a dense grid the size of the picture, then one merge pass over
// the few dots that result; drawing touches only the pixels under the dots.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "addons/places/map.h"

namespace mv::places {

// One geotagged item of the folder, as the host's locations_in hands it over.
struct folder_item {
  std::string path;
  double lat = 0;
  double lon = 0;
};

// The folder's geotagged items, in the host's walk order (its folder order).
struct folder_points {
  std::string folder;  // as the host spells it
  std::vector<folder_item> items;
};

// At most this many items are kept: a folder of 50 000 geotagged files still
// clusters in a few milliseconds; past it the rest are not drawn.
inline constexpr std::size_t k_max_folder_items = 50'000;
// The clustering grid's cell, in points: items in one cell are one dot.
inline constexpr double k_folder_cell_pt = 22.0;
// A single item's dot radius, in points; a cluster grows with log2 of its
// count up to the largest.
inline constexpr double k_folder_dot_pt = 3.5;
inline constexpr double k_folder_dot_max_pt = 8.5;
// How far outside a dot a click still hits it, in points.
inline constexpr double k_folder_hit_slop_pt = 5.0;
inline constexpr std::size_t k_no_item = std::numeric_limits<std::size_t>::max();
inline constexpr std::uint32_t k_no_dot = std::numeric_limits<std::uint32_t>::max();

// One dot on the picture: a lone item or a cluster.
struct map_dot {
  double x = 0, y = 0;       // picture pixels
  double r = 0;              // radius, pixels
  std::uint32_t count = 0;   // items in it
  std::uint32_t first = 0;   // its first item (lowest index into the items)

  [[nodiscard]] bool operator==(const map_dot&) const = default;
};

// The dots' colours (0xRRGGBB), from the host's tokens: the fill sits between
// the accent (the pin's colour) and the title text, so a dot never reads as
// the pin; the ring is the land colour, so a dot reads on any border.
struct dot_palette {
  std::uint32_t fill = 0;
  std::uint32_t ring = 0;
  [[nodiscard]] bool operator==(const dot_palette&) const = default;
};
[[nodiscard]] dot_palette dot_palette_for(const theme_tokens& t) noexcept;

// The items of `items` that fall on the picture `v` describes, clustered:
// binned into a grid of k_folder_cell_pt cells anchored to the world (so a
// pan does not reshuffle the clusters), each cell one dot at its items' mean,
// then dots that would overlap merged. Item `skip` (the file on screen: the
// pin) is left out. Fills `out` (cleared first), largest count first.
void cluster_dots(const map_view& v, std::span<const folder_item> items, std::size_t skip, std::vector<map_dot>& out);

// The same for points with no path (the world map's library, kept compact):
// every point clustered as cluster_dots does, no cap. With `item_dot`, point
// i's dot (an index into `out`), or k_no_dot when it is off the picture or
// `skip`: a click's dot gives its members.
void cluster_points(const map_view& v, std::span<const route_point> points, std::size_t skip, std::vector<map_dot>& out,
                    std::vector<std::uint32_t>* item_dot = nullptr);

// Draws `dots` over a picture of `v` (BGRA, premultiplied, as render_map
// writes it). Returns false for a bad size.
bool draw_dots(const map_view& v, const dot_palette& p, std::span<const map_dot> dots, std::span<std::uint8_t> bgra);

// The dot under picture pixel (x, y): the nearest whose radius plus the click
// slop reaches it, or nullptr.
[[nodiscard]] const map_dot* dot_at(std::span<const map_dot> dots, const map_view& v, double x, double y) noexcept;

// The folder part of a path ("" when it has none, as a Photos library key, or
// it is "/"), either separator; and two paths compared with either separator.
[[nodiscard]] std::string_view folder_of(std::string_view path) noexcept;
[[nodiscard]] bool same_path(std::string_view a, std::string_view b) noexcept;

}  // namespace mv::places
