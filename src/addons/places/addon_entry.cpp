// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// mv_places's one export, mv_addon_get, and its mv.slots.1 interface
// (docs/design/27 "Delivery: the Places add-on"; docs/design/25 PRs 58-59).
//
// Places fills two slots the app already has: the Metadata pane's "Place"
// section (a label, the coordinates, a map surface the chrome pins) and a
// search provider ("portugal", "place:sintra"). Both run on the host's worker
// for this add-on, never the UI or render thread (rule 1). Everything is
// computed from data/places.mvgeo in the add-on's own verified folder; nothing
// leaves the machine (rule 6). The add-on links nothing of the core: the host
// function table is its only way in, and src/geo is its own code.
//
// The folder map (#601): with the section's "This folder" toggle on, the
// folder's geotagged items (locations_in, served from the host's metadata
// cache) are dots on the same map, and a click on one asks the host to show
// that file (open_item). Read-only.
//
// The world map (docs/plans/places-world-map.md): the window "world", opened
// from the section's Open map button or the command bar, draws every
// geotagged item of the Local search index (locations_of_index; the open
// folder without one) on the same map, filling the window, from the whole
// world to a town; a click on a dot shows its items in the viewer as a list
// (open_list).
//
// Every thunk is the ABI boundary: no exception crosses it (docs/design/14),
// and nothing here logs a path, a place or a coordinate (rule 6).
#include <mediaviewer/mediaviewer_addon.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <vector>

#include "addons/places/places.h"
#include "addons/places/self_dir.h"
#include "addons/places/world_map.h"
#include "core/json.h"
#include "geo/open.h"

#ifndef MV_PLACES_VERSION
#define MV_PLACES_VERSION "0.0.0-dev"
#endif

namespace {

using mv::places::engine;

// The host tables this build understands (the manifest says the same): v3
// carries the screen and slot calls.
constexpr uint32_t kHostApiMin = 3;
constexpr uint32_t kHostApiMax = 3;
// Search rows go out in batches so a long folder shows results as it reads.
constexpr uint32_t kSearchBatch = 128;
// #606: the points of a clip's route a search looks up, evenly spaced along
// it: about one a kilometre on a two-hour drive, finer than a town's radius.
constexpr uint32_t kRouteSearchPoints = 128;

struct addon_state {
  const mv_host_api* host = nullptr;

  // The place database: opened on first use, on whichever worker asks.
  std::mutex db_m;
  bool db_tried = false;
  std::unique_ptr<mv::geo::mapped_database> db;

  std::unique_ptr<engine> eng;

  // The section on screen, for zoom clicks (one worker calls in at a time,
  // but the lock keeps that an assumption rather than a requirement).
  std::mutex section_m;
  std::string section_id;
  uint64_t section_generation = 0;
  mv::places::section_state section;

  // The world map's window while it is open (generation 0: closed).
  std::unique_ptr<mv::places::world_map> world;
  std::mutex world_m;
  uint64_t world_generation = 0;
  mv::places::world_state world_state;
};

addon_state* g = nullptr;

const mv::geo::database* database() {
  std::lock_guard lock(g->db_m);
  if (!g->db_tried) {
    g->db_tried = true;
    const std::string dir = mv::places::self_dir();
    if (!dir.empty()) {
      auto r = mv::geo::open_database(dir + "/data/places.mvgeo");
      if (r) {
        g->db = std::move(*r);
      } else if (g->host && g->host->log) {
        g->host->log(g->host->host, 1, "places: the place database could not be opened");
      }
    }
  }
  return g->db ? &g->db->db() : nullptr;
}

template <typename Fn>
mv_status guard(Fn&& fn) noexcept {
  try {
    return fn();
  } catch (const std::bad_alloc&) {
    return MV_ERR_OUT_OF_MEMORY;
  } catch (...) {
    return MV_ERR_INTERNAL;
  }
}

// One value from the add-on's settings page, as JSON; nullopt when the host
// has no settings_get (before PR 56) or the key is unknown.
std::optional<mv::json::value> setting(const char* key) {
  const mv_host_api* h = g->host;
  if (h->struct_size < offsetof(mv_host_api, settings_get) + sizeof(h->settings_get) || !h->settings_get) {
    return std::nullopt;
  }
  char buf[256];
  uint32_t size = 0;
  if (h->settings_get(h->host, key, buf, sizeof buf, &size) != MV_OK) return std::nullopt;
  return mv::json::parse(buf);
}

mv::places::settings read_settings() {
  mv::places::settings s;
  if (auto v = setting("show_map"); v && v->k == mv::json::kind::boolean) s.show_map = v->b;
  if (auto v = setting("show_town"); v && v->k == mv::json::kind::boolean) s.show_town = v->b;
  if (auto v = setting("town_radius_km"); v && v->k == mv::json::kind::number) {
    s.town_radius_km = std::clamp(v->is_integer ? static_cast<double>(v->i) : v->d, 1.0, 50.0);
  }
  if (auto v = setting("label_names"); v && v->k == mv::json::kind::string) s.local_names = v->s == "local";
  return s;
}

mv::places::theme_tokens tokens(const mv_slot_theme& t) {
  mv::places::theme_tokens o;
  o.canvas = t.canvas;
  o.surface = t.surface;
  o.title = t.title;
  o.body = t.body;
  o.disabled = t.disabled;
  o.hairline = t.hairline;
  o.accent = t.accent;
  o.dark = t.dark != 0;
  return o;
}

// #606: appended after open_item; an older host's table stops before it.
bool has_route_of(const mv_host_api* host) {
  return host->struct_size >= offsetof(mv_host_api, route_of) + sizeof(host->route_of) && host->route_of != nullptr;
}

// A clip's telemetry route from the host (cached there by path + size +
// mtime), at most `cap` points; empty for a still, a clip without one, or a
// host without route_of.
std::vector<mv::places::route_point> route_of(const char* path, uint32_t cap) {
  std::vector<mv::places::route_point> out;
  if (!path || !*path || !has_route_of(g->host)) return out;
  std::vector<mv_addon_route_point> pts(cap);
  uint32_t n = 0;
  if (g->host->route_of(g->host->host, path, pts.data(), cap, &n) != MV_OK) return out;
  out.reserve(std::min(n, cap));
  for (uint32_t i = 0; i < n && i < cap; ++i) out.push_back({pts[i].lat, pts[i].lon});
  return out;
}

// #601: appended after still_write; an older host's table stops before it.
bool has_open_item(const mv_host_api* host) {
  return host->struct_size >= offsetof(mv_host_api, open_item) + sizeof(host->open_item) && host->open_item != nullptr;
}

// The folder of the file on screen, every geotagged item in it, read through
// the host (its metadata cache: a folder the viewer has shown costs no read)
// under the request's generation. nullptr when the request went stale or the
// walk failed: nothing is drawn, and the next file asks again.
struct folder_walk {
  mv::places::folder_points* out = nullptr;
};

int32_t MV_CALL visit_folder(void* user, const mv_addon_location_entry* e) {
  auto& w = *static_cast<folder_walk*>(user);
  try {
    if (e && e->path_utf8 && e->location.has && std::isfinite(e->location.lat) && std::isfinite(e->location.lon) &&
        std::fabs(e->location.lat) <= 90 && std::fabs(e->location.lon) <= 180) {
      w.out->items.push_back({e->path_utf8, e->location.lat, e->location.lon});
    }
    return w.out->items.size() < mv::places::k_max_folder_items ? 1 : 0;
  } catch (...) {
    return 0;
  }
}

std::shared_ptr<const mv::places::folder_points> read_folder(const std::string& path, uint64_t generation) {
  const std::string_view dir = mv::places::folder_of(path);
  if (dir.empty()) return nullptr;
  auto points = std::make_shared<mv::places::folder_points>();
  points->folder = std::string(dir);
  folder_walk w{points.get()};
  if (g->host->locations_in(g->host->host, points->folder.c_str(), generation, &visit_folder, &w) != MV_OK) {
    return nullptr;
  }
  return points;
}

void post_map(const char* section_id, uint64_t generation, const mv::places::map_image& m) {
  (void)g->host->surface_post(g->host->host, section_id, "map", generation, m.bgra.data(), m.width, m.height,
                              m.width * 4);
}

mv_status MV_CALL info_section(void*, const mv_slot_info_request* r) {
  return guard([&] {
    if (!r || !r->section_id || std::strcmp(r->section_id, "place") != 0) return MV_ERR_INVALID_ARG;
    if (!r->location.has) return MV_OK;  // asked only for geotagged files ("when": "location")
    // The host parses the coordinates in range; the boundary checks anyway, as
    // the map's pixel maths converts them to integers.
    const double lat = r->location.lat, lon = r->location.lon;
    if (!std::isfinite(lat) || !std::isfinite(lon) || std::fabs(lat) > 90 || std::fabs(lon) > 180) {
      return MV_ERR_INVALID_ARG;
    }
    mv::places::section_state state;
    const mv::places::settings prefs = read_settings();
    // #606: a GoPro or DJI clip's route goes on the map (only when there is a
    // map to draw it on; the host answers a still from its first kilobyte).
    std::vector<mv::places::route_point> route;
    if (prefs.show_map) route = route_of(r->path_utf8, static_cast<uint32_t>(mv::places::k_max_route_points));
    const auto out = g->eng->section(lat, lon, r->location_text ? r->location_text : "",
                                     tokens(r->theme), r->width_pt, r->scale, prefs, state,
                                     r->path_utf8 ? r->path_utf8 : "", route);
    if (g->host->screen_post(g->host->host, r->section_id, r->generation, out.tree_json.c_str()) != MV_OK) {
      return MV_OK;  // stale: another file is on screen now
    }
    if (out.map) post_map(r->section_id, r->generation, *out.map);
    const bool wants_folder = g->eng->wants_folder(state);
    const std::string path = state.path;
    {
      std::lock_guard lock(g->section_m);
      g->section_id = r->section_id;
      g->section_generation = r->generation;
      g->section = std::move(state);
    }
    // #601: the section is on screen first; the folder's dots follow once its
    // items are read (once per folder: arrowing through it reads nothing).
    if (wants_folder) {
      auto points = read_folder(path, r->generation);
      if (!points) return MV_OK;
      std::lock_guard lock(g->section_m);
      g->eng->set_folder(std::move(points));
      if (g->section_generation != r->generation) return MV_OK;
      std::shared_ptr<const mv::places::map_image> map;
      std::string diff;
      if (g->eng->refresh(g->section, map, diff)) {
        post_map(r->section_id, r->generation, *map);
        (void)g->host->screen_diff(g->host->host, r->section_id, r->generation, diff.c_str());
      }
    }
    return MV_OK;
  });
}

double num(const mv::json::value* v, double fallback) {
  if (!v || v->k != mv::json::kind::number) return fallback;
  return v->is_integer ? static_cast<double>(v->i) : v->d;
}

// ---- the world map (docs/plans/places-world-map.md) ---------------------------------

bool has_locations_of_index(const mv_host_api* host);

const char* const kWorld = "world";

// 0.38: appended after still_mask; an older host's table stops before it.
bool has_open_list(const mv_host_api* host) {
  return host->struct_size >= offsetof(mv_host_api, open_list) + sizeof(host->open_list) && host->open_list != nullptr;
}

// The world's pixels, then its tree (whole) or a diff: the chrome re-reads a
// surface when the screen's revision moves. Under world_m.
void post_world(uint64_t generation, bool whole) {
  const auto img = g->world->picture(g->world_state);
  if (whole) {
    if (g->host->screen_post(g->host->host, kWorld, generation, g->world->tree(g->world_state, img.get()).c_str()) !=
        MV_OK) {
      return;
    }
  }
  if (img) {
    (void)g->host->surface_post(g->host->host, kWorld, "map", generation, img->bgra.data(), img->width, img->height,
                                img->width * 4);
  }
  if (!whole) (void)g->host->screen_diff(g->host->host, kWorld, generation, g->world->diff(g->world_state, img.get()).c_str());
}

int32_t MV_CALL visit_world(void* user, const mv_addon_location_entry* e) {
  auto& out = *static_cast<mv::places::world_points*>(user);
  try {
    // A clip is one dot at its first fix: the rest of its route is not.
    if (e && e->path_utf8 && *e->path_utf8 && e->location.has &&
        (e->location.has & MV_ADDON_LOCATION_ROUTE_POINT) == 0 && std::isfinite(e->location.lat) &&
        std::isfinite(e->location.lon) && std::fabs(e->location.lat) <= 90 && std::fabs(e->location.lon) <= 180) {
      out.add(e->path_utf8, e->location.lat, e->location.lon);
    }
    return out.size() < mv::places::k_max_world_items ? 1 : 0;
  } catch (...) {
    return 0;
  }
}

// Every geotagged item of the library (the index), else of `path`'s folder.
// nullptr when the window closed or went stale meanwhile.
std::shared_ptr<mv::places::world_points> read_world(uint64_t generation, const std::string& path) {
  auto points = std::make_shared<mv::places::world_points>();
  if (has_locations_of_index(g->host)) {
    const mv_status st = g->host->locations_of_index(g->host->host, generation, &visit_world, points.get());
    if (st == MV_ERR_CANCELLED) return nullptr;
    // No index (no AI pack), or nothing in it has a place yet: the folder.
    if (st == MV_OK && points->size() != 0) {
      points->whole_index = true;
      return points;
    }
  }
  points = std::make_shared<mv::places::world_points>();
  const std::string_view dir = mv::places::folder_of(path);
  if (dir.empty()) return points;
  const std::string folder(dir);
  if (g->host->locations_in(g->host->host, folder.c_str(), generation, &visit_world, points.get()) == MV_ERR_CANCELLED) {
    return nullptr;
  }
  return points;
}

mv_status MV_CALL screen_open(void*, const mv_slot_screen_request* r) {
  return guard([&] {
    if (!r || !r->screen_id || std::strcmp(r->screen_id, kWorld) != 0) return MV_ERR_INVALID_ARG;
    const std::string path = r->path_utf8 ? r->path_utf8 : "";
    // The file on screen is the pin, when it has a place.
    std::optional<mv::places::route_point> pin;
    if (!path.empty()) {
      mv_addon_location loc{};
      if (g->host->location_of(g->host->host, path.c_str(), &loc) == MV_OK && loc.has) pin = mv::places::route_point{loc.lat, loc.lon};
    }
    {
      std::lock_guard lock(g->world_m);
      g->world_generation = r->generation;
      g->world->open(g->world_state, tokens(r->theme), r->width_pt, r->height_pt, r->scale, path, pin);
      post_world(r->generation, true);
    }
    // The window is up first; the dots follow once the library is read.
    auto points = read_world(r->generation, path);
    if (!points) return MV_OK;
    std::lock_guard lock(g->world_m);
    if (g->world_generation != r->generation) return MV_OK;
    g->world->set_points(g->world_state, std::move(points));
    post_world(r->generation, false);
    return MV_OK;
  });
}

void world_event(const mv_slot_event* e) {
  std::lock_guard lock(g->world_m);
  if (e->kind == MV_SLOT_EVENT_CLOSE) {
    g->world_generation = 0;
    g->world_state = mv::places::world_state{};  // the library's points go with the window
    g->world->cache().clear();
    return;
  }
  if (g->world_generation == 0 || e->generation != g->world_generation) return;
  const std::string node = e->node_id;
  auto& s = g->world_state;
  const auto v = mv::json::parse(e->value_json ? e->value_json : "");
  const auto obj = v && v->k == mv::json::kind::object;
  bool changed = false;
  if (e->kind == MV_SLOT_EVENT_CLICK) {
    const int delta = node == "zoom_in" ? 1 : node == "zoom_out" ? -1 : 0;
    changed = delta != 0 && g->world->zoom(s, delta);
  } else if (node != "map" || !obj) {
    return;
  } else if (e->kind == MV_SLOT_EVENT_RESIZE) {
    changed = g->world->resize(s, static_cast<float>(num(v->find("width"), 0)), static_cast<float>(num(v->find("height"), 0)),
                               static_cast<float>(num(v->find("scale"), 1)));
  } else if (e->kind == MV_SLOT_EVENT_ZOOM) {
    changed = g->world->gesture(s, num(v->find("scale"), 1.0), num(v->find("x"), 0.5), num(v->find("y"), 0.5), 0, 0);
  } else if (e->kind == MV_SLOT_EVENT_PAN) {
    changed = g->world->gesture(s, 1.0, 0.5, 0.5, num(v->find("dx"), 0), num(v->find("dy"), 0));
  } else if (e->kind == MV_SLOT_EVENT_DOUBLE) {
    changed = g->world->gesture(s, 2.0, num(v->find("x"), 0.5), num(v->find("y"), 0.5), 0, 0);
  } else if (e->kind == MV_SLOT_EVENT_TAP) {
    // A dot's items, shown in the viewer as a list titled by its place.
    const auto hit = g->world->tap(s, num(v->find("x"), -1), num(v->find("y"), -1));
    if (hit.items.empty() || !has_open_list(g->host) || !s.points) return;
    std::vector<std::string> paths;
    paths.reserve(hit.items.size());
    for (const std::uint32_t i : hit.items) paths.emplace_back(s.points->path(i));
    std::vector<const char*> ptrs;
    ptrs.reserve(paths.size());
    for (const std::string& p : paths) ptrs.push_back(p.c_str());
    const std::string title = g->world->title_of(s, hit, read_settings().lookup());
    (void)g->host->open_list(g->host->host, kWorld, e->generation, title.c_str(), ptrs.data(),
                             static_cast<uint32_t>(ptrs.size()), 0);
    return;
  }
  if (changed) post_world(e->generation, false);
}

void MV_CALL on_event(void*, const mv_slot_event* e) {
  (void)guard([&] {
    if (!e || !e->node_id || !e->screen_id) return MV_OK;
    const std::string node = e->node_id;
    // The command bar's World Map: the window (screen_open draws it).
    if (e->kind == MV_SLOT_EVENT_COMMAND) {
      if (node == kWorld) (void)g->host->screen_show(g->host->host, kWorld, 1);
      return MV_OK;
    }
    if (std::strcmp(e->screen_id, kWorld) == 0) {
      world_event(e);
      return MV_OK;
    }
    // The section's Open map button.
    if (e->kind == MV_SLOT_EVENT_CLICK && node == "open_world") {
      (void)g->host->screen_show(g->host->host, kWorld, 1);
      return MV_OK;
    }
    std::lock_guard lock(g->section_m);
    if (g->section_id != e->screen_id || g->section_generation != e->generation) return MV_OK;
    // Fresh on every request, never cached across a change (docs/design/27
    // "Settings"): a change also re-asks the section, under a new generation.
    g->section.prefs = read_settings();
    std::shared_ptr<const mv::places::map_image> map;
    std::string diff;
    if (e->kind == MV_SLOT_EVENT_CLICK) {
      // The -/+ buttons (and the keyboard, which presses them).
      const int delta = node == "zoom_in" ? 1 : node == "zoom_out" ? -1 : 0;
      if (delta == 0 || !g->eng->zoom(g->section, delta, map, diff)) return MV_OK;
    } else if (e->kind == MV_SLOT_EVENT_CHANGE && node == "folder") {
      // #601: the "This folder" toggle (the host checked the value is a boolean).
      const bool on = e->value_json && std::strcmp(e->value_json, "true") == 0;
      if (!g->eng->show_folder(g->section, on, map, diff)) {
        if (!on || !g->eng->wants_folder(g->section)) return MV_OK;
        auto points = read_folder(g->section.path, e->generation);
        if (!points) return MV_OK;
        g->eng->set_folder(std::move(points));
        if (!g->eng->refresh(g->section, map, diff)) return MV_OK;
      }
    } else if (e->kind == MV_SLOT_EVENT_TAP && node == "map") {
      // #601: a click on a dot or a cluster opens that item, or its first.
      const auto v = mv::json::parse(e->value_json ? e->value_json : "");
      if (!v || v->k != mv::json::kind::object || !has_open_item(g->host)) return MV_OK;
      const std::string path = g->eng->tap(g->section, num(v->find("x"), -1), num(v->find("y"), -1));
      if (!path.empty()) (void)g->host->open_item(g->host->host, e->screen_id, e->generation, path.c_str());
      return MV_OK;
    } else if ((e->kind == MV_SLOT_EVENT_ZOOM || e->kind == MV_SLOT_EVENT_PAN || e->kind == MV_SLOT_EVENT_DOUBLE) &&
               node == "map") {
      // A settled pinch / wheel (zoom about the cursor), drag (pan) or
      // double-click (one step in, about the click) over the map. The chrome
      // showed pinch, wheel and drag live by moving the last picture; this
      // draws the new one where that left it. Deltas from what is on screen.
      const auto v = mv::json::parse(e->value_json ? e->value_json : "");
      if (!v || v->k != mv::json::kind::object) return MV_OK;
      double scale = 1, ax = 0.5, ay = 0.5, px = 0, py = 0;
      if (e->kind == MV_SLOT_EVENT_ZOOM) {
        scale = num(v->find("scale"), 1.0);
        ax = num(v->find("x"), 0.5);
        ay = num(v->find("y"), 0.5);
      } else if (e->kind == MV_SLOT_EVENT_PAN) {
        px = num(v->find("dx"), 0);
        py = num(v->find("dy"), 0);
      } else {
        scale = 2;
        ax = num(v->find("x"), 0.5);
        ay = num(v->find("y"), 0.5);
      }
      if (!g->eng->gesture(g->section, scale, ax, ay, px, py, map, diff)) return MV_OK;
    } else {
      return MV_OK;
    }
    // Pixels first, then the diff: the chrome re-reads the surface when the
    // tree's revision moves.
    post_map(e->screen_id, e->generation, *map);
    (void)g->host->screen_diff(g->host->host, e->screen_id, e->generation, diff.c_str());
    return MV_OK;
  });
}

struct search_ctx {
  const mv_slot_search_request* r = nullptr;
  const mv::geo::database* db = nullptr;
  const mv::places::place_query* q = nullptr;
  mv::geo::lookup_options opts;
  uint32_t seen = 0;
  // #606: the index walk sends a clip's route points as rows after its own,
  // under the same path; once that path matched, the rest are skipped.
  std::string last_path;
  bool last_matched = false;
  std::vector<std::string> paths;
  std::vector<std::string> whys;
  bool stale = false;
  bool refused = false;  // the host took no more rows (its result cap): stop walking
};

void flush(search_ctx& c, bool done) {
  std::vector<mv_slot_search_row> rows;
  rows.reserve(c.paths.size());
  for (std::size_t i = 0; i < c.paths.size(); ++i) rows.push_back({c.paths[i].c_str(), c.whys[i].c_str()});
  const mv_status st = g->host->search_post(g->host->host, c.r->provider_id, c.r->generation, rows.data(),
                                            static_cast<uint32_t>(rows.size()), done ? 1 : 0);
  if (st == MV_ERR_CANCELLED) {
    c.stale = true;
  } else if (st != MV_OK) {
    c.refused = true;
    // A refused batch drops its `done` too: say done without rows, or the
    // chrome keeps saying "searching" for this provider.
    if (done) (void)g->host->search_post(g->host->host, c.r->provider_id, c.r->generation, nullptr, 0, 1);
  }
  c.paths.clear();
  c.whys.clear();
}

int32_t MV_CALL visit_location(void* user, const mv_addon_location_entry* e) {
  auto& c = *static_cast<search_ctx*>(user);
  try {
    const bool point_of_route = e && (e->location.has & MV_ADDON_LOCATION_ROUTE_POINT) != 0;
    if (!point_of_route) ++c.seen;  // progress counts files, not the points of their routes
    if (e && e->path_utf8 && e->location.has) {
      const bool same = point_of_route && c.last_path == e->path_utf8;
      if (!same) {
        c.last_path = e->path_utf8;
        c.last_matched = false;
      }
      if (!c.last_matched) {
        std::string why = c.q->why(*c.db, e->location.lat, e->location.lon, c.opts);
        // #606: a clip from a folder whose own position (the route's first
        // fix) is elsewhere: does the rest of its route pass through?
        if (why.empty() && (e->location.has & MV_ADDON_LOCATION_ROUTE)) {
          const auto route = route_of(e->path_utf8, kRouteSearchPoints);
          why = mv::places::route_why(*c.q, *c.db, route, c.opts, kRouteSearchPoints);
        }
        if (!why.empty()) {
          c.last_matched = true;
          c.paths.emplace_back(e->path_utf8);
          c.whys.push_back(std::move(why));
        }
      }
    }
    if (c.seen % kSearchBatch == 0) {
      g->host->search_progress(g->host->host, c.r->provider_id, c.r->generation, c.seen, 0);
      if (!c.paths.empty()) flush(c, false);
    }
    return c.stale || c.refused ? 0 : 1;
  } catch (...) {
    return 0;
  }
}

// #398: appended after read_cues; an older host's table stops before it.
bool has_locations_of_index(const mv_host_api* host) {
  return host->struct_size >= offsetof(mv_host_api, locations_of_index) + sizeof(host->locations_of_index) &&
         host->locations_of_index != nullptr;
}

mv_status MV_CALL search(void*, const mv_slot_search_request* r) {
  return guard([&] {
    if (!r || !r->provider_id) return MV_ERR_INVALID_ARG;
    const mv::geo::database* db = database();
    const char* folder = r->folder_utf8 ? r->folder_utf8 : "";
    // Local search over the whole index (no folder) walks the index's own
    // coordinates (#398, locations_of_index), served from what the index
    // holds. A host without that row gets done with no rows.
    const bool whole_index = !*folder;
    if (!db || (whole_index && !has_locations_of_index(g->host))) {
      return g->host->search_post(g->host->host, r->provider_id, r->generation, nullptr, 0, 1);
    }
    const auto q = mv::places::place_query::compile(*db, r->folded_utf8 ? r->folded_utf8 : "");
    if (q.empty()) return g->host->search_post(g->host->host, r->provider_id, r->generation, nullptr, 0, 1);
    search_ctx c;
    c.r = r;
    c.db = db;
    c.q = &q;
    c.opts = read_settings().lookup();
    // No index attached (MV_ERR_NOT_FOUND) or a failed walk: `done` with what
    // was found, as below.
    const mv_status st =
        whole_index ? g->host->locations_of_index(g->host->host, r->generation, &visit_location, &c)
                    : g->host->locations_in(g->host->host, folder, r->generation, &visit_location, &c);
    if (st == MV_ERR_CANCELLED || c.stale) return MV_ERR_CANCELLED;
    g->host->search_progress(g->host->host, r->provider_id, r->generation, c.seen, c.seen);
    if (c.refused) {  // past the cap: post only `done`
      c.paths.clear();
      c.whys.clear();
    }
    flush(c, true);
    return MV_OK;
  });
}

const mv_addon_slots kSlots{sizeof(mv_addon_slots), 0, &info_section, &search, &screen_open, nullptr, &on_event};

const void* MV_CALL query(void*, const char* id) {
  if (id && std::strcmp(id, MV_SLOTS_INTERFACE) == 0) return &kSlots;
  return nullptr;
}

void MV_CALL shutdown(void*) {
  delete g;
  g = nullptr;
}

}  // namespace

extern "C" MV_ADDON_EXPORT mv_status MV_CALL mv_addon_get(uint32_t host_api, const mv_host_api* host,
                                                          mv_addon_api* out) {
  return guard([&] {
    if (host_api < kHostApiMin || host_api > kHostApiMax || !host || !out ||
        host->struct_size < offsetof(mv_host_api, locations_in) + sizeof(host->locations_in)) {
      return MV_ERR_UNSUPPORTED_FORMAT;
    }
    if (g) return MV_ERR_INVALID_ARG;  // one instance per process
    auto state = std::make_unique<addon_state>();
    state->host = host;
    state->eng = std::make_unique<engine>([] { return database(); });
    state->world = std::make_unique<mv::places::world_map>([] { return database(); });
    g = state.release();
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    out->id = "places";
    out->version = MV_PLACES_VERSION;
    out->addon = g;
    out->shutdown = &shutdown;
    out->query = &query;
    return MV_OK;
  });
}
