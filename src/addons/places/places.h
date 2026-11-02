// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Places add-on's engine (docs/design/27 "Delivery: the Places add-on"):
// the Metadata pane's Place section as a view tree plus its map, the zoom
// steps, and search by place. ABI-free so the tests drive it directly;
// addon_entry.cpp is the mv.slots.1 glue around it.
//
// Every call runs on the host's worker for this add-on (rule 1); none of it
// touches the network (rule 6).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "addons/places/folder_map.h"
#include "addons/places/map.h"
#include "geo/geo.h"

namespace mv::places {

// The add-on's settings page (PR 56 schema; manifest.json "settings").
struct settings {
  bool show_map = true;
  bool show_town = true;
  double town_radius_km = geo::k_town_radius_km;  // 1 .. 50
  bool local_names = false;                       // label_names: "english" | "local"

  [[nodiscard]] geo::lookup_options lookup() const noexcept {
    geo::lookup_options o;
    o.town_radius_km = show_town ? town_radius_km : 0;
    o.native_names = local_names;
    return o;
  }
};

// The map's height in points (the slot contract: a 160 pt surface).
inline constexpr float k_map_height_pt = 160.0f;
// The host's largest surface side (mediaviewer_addon.h surface_post).
inline constexpr float k_max_surface_px = 4096.0f;
// What the map can and cannot show, said once under it.
inline constexpr std::string_view k_map_caption = "No street detail \xE2\x80\x94 offline map";
// #601: the folder map's toggle.
inline constexpr std::string_view k_folder_toggle = "This folder";

// One file's section: what the host sent and what was worked out from it.
struct section_state {
  double lat = 0;
  double lon = 0;
  std::string location_text;  // the host's own coordinate string
  theme_tokens theme;
  float width_pt = 0;
  float scale = 1;
  settings prefs;
  geo::place place;           // label "" when the place database is missing
  map_view view;              // what the map shows now: centre and span (the pin is at lat/lon)
  // #601: the file on screen ("" when the host gave none), and the folder
  // map's dots on the picture now (empty while the layer is off).
  std::string path;
  std::vector<map_dot> dots;
};

struct section_result {
  std::string tree_json;                   // the whole view tree (screen.h vocabulary)
  std::shared_ptr<const map_image> map;    // null when the map is off or unavailable
};

class engine {
 public:
  // `db` returns the place database, opening it on first use (on the calling
  // worker), or nullptr when it is missing or damaged.
  explicit engine(std::function<const geo::database*()> db) : db_(std::move(db)) {}

  // The Place section for a geotagged file. Fills `state` for later zoom steps.
  // `path` is the file on screen: with a folder, the section offers the
  // folder map (#601).
  [[nodiscard]] section_result section(double lat, double lon, std::string_view location_text,
                                       const theme_tokens& theme, float width_pt, float scale,
                                       const settings& prefs, section_state& state, std::string_view path = {});

  // One zoom step in (+1) or out (-1) about the picture's centre: the -/+
  // buttons and the keyboard. Returns false at the end of the range (nothing
  // changes). `diff_json` moves the pin, the town names and the buttons.
  bool zoom(section_state& state, int delta, std::shared_ptr<const map_image>& map, std::string& diff_json);

  // A settled chrome gesture on the map (pinch, wheel, drag, double-click):
  // see apply_gesture. Returns false when the view does not change.
  bool gesture(section_state& state, double scale, double anchor_x, double anchor_y, double pan_x, double pan_y,
               std::shared_ptr<const map_image>& map, std::string& diff_json);

  // The surface's pixel size for a pane width and backing scale. The backing
  // scale is the display's (1..3), lowered so the width stays within the
  // host's 4096-pixel surface limit; surface_height takes that scale.
  [[nodiscard]] static float surface_scale(float width_pt, float scale) noexcept;
  [[nodiscard]] static std::uint32_t surface_width(float width_pt, float scale) noexcept;
  [[nodiscard]] static std::uint32_t surface_height(float scale) noexcept;

  // ---- the folder map (#601; docs/design/27 "The folder map") --------------------
  // Whether the layer is on: one choice for the session, kept across files.
  [[nodiscard]] bool folder_layer() const noexcept { return folder_layer_; }
  // The folder's items are needed (the layer is on and what is held is
  // another folder's, or misses the file on screen): the caller walks the
  // folder (locations_in) and hands them to set_folder.
  [[nodiscard]] bool wants_folder(const section_state& s) const;
  void set_folder(std::shared_ptr<const folder_points> points) { folder_ = std::move(points); }
  [[nodiscard]] const folder_points* folder() const noexcept { return folder_.get(); }
  // The toggle: turns the layer on or off and redraws. Returns false when
  // nothing changes on screen, and when the layer went on but wants_folder:
  // the caller reads the folder, hands it to set_folder, then refreshes.
  bool show_folder(section_state& state, bool on, std::shared_ptr<const map_image>& map, std::string& diff_json);
  // The same view again with what set_folder handed over.
  bool refresh(section_state& state, std::shared_ptr<const map_image>& map, std::string& diff_json);
  // A click at (x, y), 0..1 of the picture: the path of the dot under it (a
  // cluster's first item), or "".
  [[nodiscard]] std::string tap(const section_state& state, double x, double y) const;

  [[nodiscard]] map_cache& cache() noexcept { return cache_; }
  [[nodiscard]] const geo::database* database() const { return db_(); }

 private:
  [[nodiscard]] std::shared_ptr<const map_image> picture(section_state& s);
  // The base picture with the folder's dots over it (a copy; the cache keeps
  // the base), or the base itself while the layer is off.
  [[nodiscard]] std::shared_ptr<const map_image> with_dots(section_state& s, std::shared_ptr<const map_image> base);
  // Moves to `next` and describes the change for the chrome.
  bool move_to(section_state& state, const map_view& next, std::shared_ptr<const map_image>& map,
               std::string& diff_json);

  std::function<const geo::database*()> db_;
  map_cache cache_;
  bool folder_layer_ = false;
  std::shared_ptr<const folder_points> folder_;
};

// ---- search by place (the "place" search provider) -------------------------------

// What a typed word names: countries, regions and a town name. Compiled once
// per query, then tested against each file's place.
//
// The rule: the folded query equals a country's or region's name in any of
// the database's languages, or starts one when it is 4 letters or more
// ("portu" finds Portugal while typing; "po" does not). It may also equal the
// file's own town name (or start it, at 4 letters). A query starting with a
// digit is never a place: `in:2024` stays Local search's date term.
class place_query {
 public:
  [[nodiscard]] static place_query compile(const geo::database& db, std::string_view folded);
  [[nodiscard]] bool empty() const noexcept { return empty_; }
  // The file's place label when it matches ("Sintra, Lisbon, Portugal": the
  // result's "why"), else "".
  [[nodiscard]] std::string why(const geo::database& db, double lat, double lon,
                                const geo::lookup_options& opts) const;

 private:
  bool empty_ = true;
  bool prefix_ = false;
  std::string text_;  // folded
  std::vector<std::string> countries_;
  std::vector<std::string> regions_;
};

}  // namespace mv::places
