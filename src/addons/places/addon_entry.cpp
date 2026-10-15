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
    const auto out = g->eng->section(lat, lon, r->location_text ? r->location_text : "",
                                     tokens(r->theme), r->width_pt, r->scale, read_settings(), state);
    if (g->host->screen_post(g->host->host, r->section_id, r->generation, out.tree_json.c_str()) != MV_OK) {
      return MV_OK;  // stale: another file is on screen now
    }
    if (out.map) post_map(r->section_id, r->generation, *out.map);
    std::lock_guard lock(g->section_m);
    g->section_id = r->section_id;
    g->section_generation = r->generation;
    g->section = std::move(state);
    return MV_OK;
  });
}

double num(const mv::json::value* v, double fallback) {
  if (!v || v->k != mv::json::kind::number) return fallback;
  return v->is_integer ? static_cast<double>(v->i) : v->d;
}

void MV_CALL on_event(void*, const mv_slot_event* e) {
  (void)guard([&] {
    if (!e || !e->node_id || !e->screen_id) return MV_OK;
    const std::string node = e->node_id;
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
    ++c.seen;
    if (e && e->path_utf8 && e->location.has) {
      std::string why = c.q->why(*c.db, e->location.lat, e->location.lon, c.opts);
      if (!why.empty()) {
        c.paths.emplace_back(e->path_utf8);
        c.whys.push_back(std::move(why));
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

const mv_addon_slots kSlots{sizeof(mv_addon_slots), 0, &info_section, &search, nullptr, nullptr, &on_event};

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
