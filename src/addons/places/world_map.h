// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The world map (docs/plans/places-world-map.md): Places' own window, the
// same offline map as the Place section filling it, from the whole world down
// to a town, with every geotagged item of the library (the Local search
// index; the open folder without one) as dots that cluster by zoom. A click
// on a dot hands its items to the viewer as a result list. Read-only.
//
// ABI-free so the tests drive it directly; addon_entry.cpp walks the index and
// posts what this returns. Every call runs on the host's worker for Places
// (rule 1); nothing touches the network (rule 6).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "addons/places/folder_map.h"
#include "addons/places/map.h"
#include "geo/geo.h"

namespace mv::places {

// Items past this are not kept (a library of 400 000 geotagged files is
// about 50 MB here; clustering it is one pass).
inline constexpr std::size_t k_max_world_items = 400'000;
// The window's size before the chrome reports one (manifest.json "world").
inline constexpr float k_world_width_pt = 1100.0f;
inline constexpr float k_world_height_pt = 720.0f;
// A picture's pixel budget: a full-screen 5K window draws at a lower backing
// scale rather than 15 MP per gesture. Each side also stays within 4096 px.
inline constexpr double k_world_max_pixels = 8.0e6;
// The most items one click hands the viewer (the host's open_list cap).
inline constexpr std::size_t k_world_max_list = 10'000;
// The window's id in manifest.json.
inline constexpr std::string_view k_world_screen = "world";

// The library's geotagged items, compact: points apart from one buffer of
// paths, in the host's walk order.
class world_points {
 public:
  // false: the open folder's items (no index to walk).
  bool whole_index = false;

  void add(std::string_view path, double lat, double lon);
  [[nodiscard]] std::size_t size() const noexcept { return points_.size(); }
  [[nodiscard]] std::span<const route_point> points() const noexcept { return points_; }
  [[nodiscard]] std::string_view path(std::size_t i) const noexcept;
  // The item at `path` (either separator), or k_no_item.
  [[nodiscard]] std::size_t find(std::string_view path) const noexcept;

 private:
  std::vector<route_point> points_;
  std::string paths_;
  std::vector<std::size_t> ends_;
};

// One open window.
struct world_state {
  theme_tokens theme;
  float width_pt = k_world_width_pt;
  float height_pt = k_world_height_pt;
  float scale = 1;      // the display's backing scale, as the chrome reports it
  map_view view;        // what the picture shows now
  // The file on screen when the window opened: the pin.
  std::string pin_path;
  std::optional<route_point> pin;
  std::shared_ptr<const world_points> points;
  std::size_t pin_index = k_no_item;  // the pin's item in `points`: not a dot
  bool reading = true;  // the walk has not finished
  bool moved = false;   // the person zoomed or panned: a late walk does not reframe
  std::vector<map_dot> dots;  // on the picture now
};

// A click on a dot: its items (indices into the points, walk order, at most
// k_world_max_list), and the first one's place for the list's title.
struct world_hit {
  std::vector<std::uint32_t> items;
  std::uint32_t count = 0;  // the dot's whole count (may exceed items)
};

class world_map {
 public:
  explicit world_map(std::function<const geo::database*()> db) : db_(std::move(db)), cache_(2) {}

  // A window that opened at (width_pt, height_pt), backing `scale`, with the
  // file on screen at `pin` (none for a file without coordinates).
  void open(world_state& s, const theme_tokens& theme, float width_pt, float height_pt, float scale,
            std::string_view pin_path, std::optional<route_point> pin) const;

  // The window's whole view tree, and a diff for what moved since.
  [[nodiscard]] std::string tree(const world_state& s, const map_image* img) const;
  [[nodiscard]] std::string diff(const world_state& s, const map_image* img) const;
  // The picture for the view now: the base map (cached) with the dots over it.
  [[nodiscard]] std::shared_ptr<const map_image> picture(world_state& s);

  // The chrome laid the map out at a new size. False when nothing changes.
  bool resize(world_state& s, float width_pt, float height_pt, float scale) const;
  // A settled pinch, wheel, drag or double-click (as the section's), and the
  // -/+ steps about the centre. False when the view does not change.
  bool gesture(world_state& s, double scale, double anchor_x, double anchor_y, double pan_x, double pan_y) const;
  bool zoom(world_state& s, int delta) const;
  // The walk finished: these are the dots. Frames them unless the person
  // has moved the map meanwhile.
  void set_points(world_state& s, std::shared_ptr<const world_points> points) const;
  // The dot under (x, y), 0..1 of the picture: its items, or none.
  [[nodiscard]] world_hit tap(const world_state& s, double x, double y) const;
  // A list title for a hit: the first item's place label, with the count.
  [[nodiscard]] std::string title_of(const world_state& s, const world_hit& h, const geo::lookup_options& opts) const;

  // The picture's pixel size and backing scale for a window size: the
  // display's scale, lowered for the 4096 px side and the pixel budget.
  [[nodiscard]] static map_view sized(map_view v, float width_pt, float height_pt, float scale) noexcept;
  // The widest span `v` may take: the whole world's width fills the picture's.
  [[nodiscard]] static double widest_span(const map_view& v) noexcept;
  // `v` centred on the points' box with a margin (the whole world for points
  // all over it), at least k_min_span_deg * 8 wide so one photo shows its region.
  [[nodiscard]] static map_view frame(const map_view& v, std::span<const route_point> points) noexcept;

  [[nodiscard]] map_cache& cache() noexcept { return cache_; }

 private:
  [[nodiscard]] static map_view clamp_view(map_view v) noexcept;

  std::function<const geo::database*()> db_;
  map_cache cache_;
};

}  // namespace mv::places
