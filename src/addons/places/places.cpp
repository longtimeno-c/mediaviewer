// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Places engine (places.h).
#include "addons/places/places.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/json.h"
#include "geo/fold.h"

namespace mv::places {
namespace {

// The zoom glyphs, from the host's symbol table (src/addon/contrib.h kSymbols;
// a tree naming an unknown symbol is refused whole).
constexpr std::string_view kZoomOutSymbol = "minus";
constexpr double kEps = 1e-9;

bool at_widest(const map_view& v) noexcept { return v.span_deg >= k_max_span_deg - kEps; }
bool at_closest(const map_view& v) noexcept { return v.span_deg <= k_min_span_deg + kEps; }

// The buttons, as screen.h nodes.
void zoom_buttons(json::writer& w, const map_view& v) {
  w.begin_object().key("type").string("button").key("id").string("zoom_out");
  w.key("label").string("Zoom out");
  w.key("symbol").string(kZoomOutSymbol);
  w.key("disabled").boolean(at_widest(v)).end_object();
  w.begin_object().key("type").string("button").key("id").string("zoom_in");
  w.key("label").string("Zoom in").key("symbol").string("plus");
  w.key("disabled").boolean(at_closest(v)).end_object();
}

// The pin (the photo's point) on the picture, 0..1.
void pin_of(const section_state& s, double& x, double& y) {
  map_pixel(s.view, s.lat, s.lon, x, y);
  x = std::clamp(s.view.width ? x / s.view.width : 0.5, 0.0, 1.0);
  y = std::clamp(s.view.height ? y / s.view.height : 0.5, 0.0, 1.0);
}

// The surface node's moving props: the pin, the town names, and how far the
// chrome may scale the picture live before it must ask for a new one.
void surface_props(json::writer& w, const section_state& s, const map_image* img) {
  double px = 0.5, py = 0.5;
  pin_of(s, px, py);
  w.key("pin").begin_object().key("x").number(px).key("y").number(py).end_object();
  w.key("marks").begin_array();
  if (img) {
    for (const map_mark& m : img->marks) {
      w.begin_object().key("x").number(m.x).key("y").number(m.y).key("text").string(m.text).end_object();
    }
  }
  w.end_array();
  // Scaling the picture by s divides the span by s; the host bounds these to
  // [0.01, 1] and [1, 1000].
  const double span = clamp_span(s.view.span_deg);
  w.key("interactive").begin_object();
  w.key("min_scale").number(std::clamp(span / k_max_span_deg, 0.01, 1.0));
  w.key("max_scale").number(std::clamp(span / k_min_span_deg, 1.0, 1000.0));
  w.end_object();
}

std::string trimmed(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return std::string(s);
}

std::size_t utf8_length(std::string_view s) noexcept {
  std::size_t n = 0;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80;
  return n;
}

}  // namespace

namespace {
float pane_points(float width_pt) noexcept { return std::clamp(width_pt > 0 ? width_pt : 280.0f, 120.0f, 1600.0f); }
}  // namespace

float engine::surface_scale(float width_pt, float scale) noexcept {
  const float s = std::clamp(scale > 0 ? scale : 1.0f, 1.0f, 3.0f);
  // The host takes a surface of 4096 pixels a side at most: a very wide pane
  // on a 3x display gets the map at a lower backing scale, not no map at all.
  return std::min(s, k_max_surface_px / pane_points(width_pt));
}

std::uint32_t engine::surface_width(float width_pt, float scale) noexcept {
  const float px = pane_points(width_pt) * surface_scale(width_pt, scale);
  return static_cast<std::uint32_t>(std::min(std::floor(px + 0.5f), k_max_surface_px));
}

std::uint32_t engine::surface_height(float scale) noexcept {
  const float s = std::clamp(scale > 0 ? scale : 1.0f, 1.0f, 3.0f);
  return static_cast<std::uint32_t>(std::lround(k_map_height_pt * s));
}

std::shared_ptr<const map_image> engine::picture(section_state& s) {
  const geo::database* db = db_();
  s.dots.clear();
  if (!db || !s.prefs.show_map) return nullptr;
  auto base = cache_.get(*db, s.view, palette_for(s.theme), s.place.at_sea ? std::string_view{} : s.place.country_code);
  if (base && !s.route.empty()) {
    // #606: the route on a copy of the cached picture, in the accent colour
    // (the pin's) on a casing of the pane's surface; #601's dots go over it.
    std::lock_guard lock(routed_m_);
    if (!(routed_ && routed_base_ == base && routed_id_ == s.route_id)) {
      auto img = std::make_shared<map_image>(*base);
      (void)draw_route(s.view, s.route, s.theme.accent >> 8, s.theme.surface >> 8, img->bgra);
      routed_base_ = base;
      routed_id_ = s.route_id;
      routed_ = std::move(img);
    }
    base = routed_;
  }
  return with_dots(s, std::move(base));
}

map_view frame_route(const map_view& v, double pin_lat, double pin_lon, std::span<const route_point> route) noexcept {
  if (route.empty() || v.width == 0 || v.height == 0) return v;
  // The route's box, longitudes unwrapped from the pin so a route across 180
  // degrees is one box.
  double lat0 = pin_lat, lat1 = pin_lat, dl0 = 0, dl1 = 0;
  for (const route_point& p : route) {
    if (!std::isfinite(p.lat) || !std::isfinite(p.lon)) continue;
    double dl = std::fmod(p.lon - pin_lon + 540.0, 360.0) - 180.0;
    if (!std::isfinite(dl)) continue;
    lat0 = std::min(lat0, p.lat);
    lat1 = std::max(lat1, p.lat);
    dl0 = std::min(dl0, dl);
    dl1 = std::max(dl1, dl);
  }
  const double mid_lat = std::clamp((lat0 + lat1) / 2, -85.0, 85.0);
  // The span (degrees of latitude the picture is tall) that holds the box
  // with a quarter more round it: the width holds w/h times as many degrees
  // of longitude, fewer by cos(latitude) on the ground.
  const double aspect = static_cast<double>(v.width) / v.height;
  const double need = 1.25 * std::max(lat1 - lat0, (dl1 - dl0) * std::cos(mid_lat * 3.14159265358979323846 / 180.0) / aspect);
  if (!std::isfinite(need)) return v;
  // Centred on the route's box, at the wider of the two spans.
  map_view out = v;
  out.span_deg = clamp_span(std::max(need, v.span_deg));
  out.lat = mid_lat;
  out.lon = pin_lon + (dl0 + dl1) / 2;
  if (out.lon > 180) out.lon -= 360;
  if (out.lon < -180) out.lon += 360;
  return keep_pin_in_view(out, pin_lat, pin_lon);
}

std::string route_why(const place_query& q, const geo::database& db, std::span<const route_point> route,
                      const geo::lookup_options& opts, std::size_t max_points) {
  if (q.empty() || route.empty() || max_points == 0) return {};
  const std::size_t n = std::min(route.size(), max_points);
  const std::size_t last = route.size() - 1;
  for (std::size_t k = 0; k < n; ++k) {
    const std::size_t i = n == 1 ? 0 : k * last / (n - 1);
    const route_point& p = route[i];
    if (!std::isfinite(p.lat) || !std::isfinite(p.lon) || std::fabs(p.lat) > 90 || std::fabs(p.lon) > 180) continue;
    std::string why = q.why(db, p.lat, p.lon, opts);
    if (!why.empty()) return why;
  }
  return {};
}

section_result engine::section(double lat, double lon, std::string_view location_text, const theme_tokens& theme,
                               float width_pt, float scale, const settings& prefs, section_state& state,
                               std::string_view path, std::span<const route_point> route) {
  state = section_state{};
  state.path = std::string(path);
  state.lat = lat;
  state.lon = lon;
  state.location_text = std::string(location_text);
  state.theme = theme;
  state.width_pt = width_pt;
  state.scale = scale;
  state.prefs = prefs;
  state.view.lat = lat;
  state.view.lon = lon;
  const float backing = surface_scale(width_pt, scale);
  state.view.width = surface_width(width_pt, scale);
  state.view.height = surface_height(backing);
  state.view.scale = backing;
  state.view.span_deg = k_zoom_span_deg[k_region_zoom_step];
  const geo::database* db = db_();
  if (db) {
    state.place = db->lookup(lat, lon, prefs.lookup());
    const int z = framing_zoom(*db, lat, lon, state.place.at_sea ? std::string_view{} : state.place.country_code,
                               state.place.region_code, state.view.width, state.view.height);
    state.view.span_deg = k_zoom_span_deg[std::clamp(z, 0, k_zoom_steps - 1)];
  }
  state.view = keep_pin_in_view(state.view, lat, lon);
  if (!route.empty()) {
    // #606: the clip's route, capped as draw_route reads it, and the first
    // view wide enough to show it.
    state.route.assign(route.begin(), route.begin() + static_cast<std::ptrdiff_t>(std::min(route.size(), k_max_route_points)));
    std::uint64_t id = 1469598103934665603ull;  // FNV-1a over the coordinates
    for (const route_point& p : state.route) {
      for (const double d : {p.lat, p.lon}) {
        std::uint64_t bits;
        std::memcpy(&bits, &d, sizeof bits);
        id = (id ^ bits) * 1099511628211ull;
      }
    }
    state.route_id = id;
    state.view = frame_route(state.view, lat, lon, state.route);
  }
  const std::string& label = state.place.label;

  section_result out;
  json::writer w;
  w.begin_object().key("type").string("stack").key("id").string("root").key("spacing").integer(6);
  w.key("children").begin_array();
  if (!label.empty()) {
    w.begin_object().key("type").string("text").key("id").string("label").key("text").string(label);
    w.key("style").string("title").key("copyable").boolean(true).end_object();
  } else {
    // No place database (damaged, or removed under us): say so, keep the coordinates.
    w.begin_object().key("type").string("text").key("id").string("label");
    w.key("text").string("Place names are unavailable").key("style").string("secondary").end_object();
  }
  if (!state.location_text.empty()) {
    w.begin_object().key("type").string("text").key("id").string("coords").key("text").string(state.location_text);
    w.key("style").string("secondary").key("copyable").boolean(true).end_object();
  }
  if (db && prefs.show_map) {
    out.map = picture(state);
    // One element for VoiceOver / Narrator, named by the place: the picture
    // itself is not read.
    const std::string& pin = state.place.town.empty() ? label : state.place.town;
    w.begin_object().key("type").string("surface").key("id").string("map").key("surface").string("map");
    w.key("height").integer(static_cast<std::int64_t>(k_map_height_pt));
    w.key("label").string("Map of " + label);
    surface_props(w, state, out.map.get());
    w.key("pin_label").string(pin).end_object();
    w.begin_object().key("type").string("stack").key("id").string("zoom").key("axis").string("horizontal");
    w.key("spacing").integer(4).key("align").string("center").key("children").begin_array();
    w.begin_object().key("type").string("text").key("id").string("caption").key("text").string(k_map_caption);
    w.key("style").string("caption").end_object();
    zoom_buttons(w, state.view);
    w.end_array().end_object();
    // #601: the folder's geotagged items on the same map, for a file that
    // has a folder (not a Photos library item).
    if (!folder_of(state.path).empty()) {
      w.begin_object().key("type").string("toggle").key("id").string("folder");
      w.key("label").string(k_folder_toggle).key("value").boolean(folder_layer_).end_object();
    }
  }
  w.end_array().end_object();
  out.tree_json = w.take();
  return out;
}

bool engine::move_to(section_state& state, const map_view& next, std::shared_ptr<const map_image>& map,
                     std::string& diff_json) {
  if (!state.prefs.show_map) return false;
  const map_view& cur = state.view;
  if (std::fabs(next.span_deg - cur.span_deg) < 1e-9 && std::fabs(next.lat - cur.lat) < 1e-9 &&
      std::fabs(next.lon - cur.lon) < 1e-9) {
    return false;
  }
  state.view = next;
  map = picture(state);
  if (!map) return false;
  json::writer w;
  w.begin_array();
  w.begin_object().key("op").string("set").key("id").string("map").key("props").begin_object();
  surface_props(w, state, map.get());
  w.end_object().end_object();
  w.begin_object().key("op").string("set").key("id").string("zoom_out").key("props").begin_object();
  w.key("disabled").boolean(at_widest(state.view)).end_object().end_object();
  w.begin_object().key("op").string("set").key("id").string("zoom_in").key("props").begin_object();
  w.key("disabled").boolean(at_closest(state.view)).end_object().end_object();
  w.end_array();
  diff_json = w.take();
  return true;
}

bool engine::zoom(section_state& state, int delta, std::shared_ptr<const map_image>& map, std::string& diff_json) {
  if (delta == 0) return false;
  // About the picture's centre, as a map app's buttons do.
  return gesture(state, std::pow(2.0, delta), 0.5, 0.5, 0, 0, map, diff_json);
}

bool engine::gesture(section_state& state, double scale, double anchor_x, double anchor_y, double pan_x,
                     double pan_y, std::shared_ptr<const map_image>& map, std::string& diff_json) {
  const map_view next = apply_gesture(state.view, scale, anchor_x, anchor_y, pan_x, pan_y, state.lat, state.lon);
  return move_to(state, next, map, diff_json);
}

// ---- the folder map (#601) ------------------------------------------------------------

std::shared_ptr<const map_image> engine::with_dots(section_state& s, std::shared_ptr<const map_image> base) {
  s.dots.clear();
  if (!base || !folder_layer_ || !folder_ || folder_->items.empty()) return base;
  const std::string_view dir = folder_of(s.path);
  if (dir.empty() || !same_path(dir, folder_->folder)) return base;
  // The file on screen is the pin, not a dot.
  std::size_t skip = k_no_item;
  for (std::size_t i = 0; i < folder_->items.size(); ++i) {
    if (same_path(folder_->items[i].path, s.path)) {
      skip = i;
      break;
    }
  }
  cluster_dots(s.view, folder_->items, skip, s.dots);
  if (s.dots.empty()) return base;
  auto img = std::make_shared<map_image>(*base);
  if (!draw_dots(s.view, dot_palette_for(s.theme), s.dots, img->bgra)) {
    s.dots.clear();
    return base;
  }
  return img;
}

bool engine::wants_folder(const section_state& s) const {
  if (!folder_layer_ || !s.prefs.show_map) return false;
  const std::string_view dir = folder_of(s.path);
  if (dir.empty()) return false;
  if (!folder_ || !same_path(dir, folder_->folder)) return true;
  // A file new to the folder since it was read (it is geotagged: the section
  // is asked only for those) means the folder changed: read it again.
  return std::none_of(folder_->items.begin(), folder_->items.end(),
                      [&s](const folder_item& it) { return same_path(it.path, s.path); });
}

bool engine::refresh(section_state& state, std::shared_ptr<const map_image>& map, std::string& diff_json) {
  if (!state.prefs.show_map) return false;
  map = picture(state);
  if (!map) return false;
  json::writer w;
  w.begin_array();
  w.begin_object().key("op").string("set").key("id").string("map").key("props").begin_object();
  surface_props(w, state, map.get());
  w.end_object().end_object();
  if (!folder_of(state.path).empty()) {
    w.begin_object().key("op").string("set").key("id").string("folder").key("props").begin_object();
    w.key("value").boolean(folder_layer_).end_object().end_object();
  }
  w.end_array();
  diff_json = w.take();
  return true;
}

bool engine::show_folder(section_state& state, bool on, std::shared_ptr<const map_image>& map,
                         std::string& diff_json) {
  const bool had_dots = !state.dots.empty();
  folder_layer_ = on;
  // Off with nothing drawn: the toggle already shows it, there are no pixels
  // to change. On without the folder's items: the caller reads them first.
  if ((!on && !had_dots) || (on && wants_folder(state))) return false;
  return refresh(state, map, diff_json);
}

std::string engine::tap(const section_state& state, double x, double y) const {
  if (!folder_layer_ || !folder_ || state.dots.empty() || !std::isfinite(x) || !std::isfinite(y)) return {};
  const map_dot* d = dot_at(state.dots, state.view, x * state.view.width, y * state.view.height);
  if (!d || d->first >= folder_->items.size()) return {};
  return folder_->items[d->first].path;
}

// ---- search -------------------------------------------------------------------------

place_query place_query::compile(const geo::database& db, std::string_view folded) {
  place_query q;
  q.text_ = geo::fold(trimmed(folded));  // the host folded it already; folding again is idempotent
  const std::size_t letters = utf8_length(q.text_);
  if (letters < 2 || (q.text_[0] >= '0' && q.text_[0] <= '9')) return q;
  q.prefix_ = letters >= 4;
  for (const geo::name_match& m : db.match(q.text_, q.prefix_)) {
    if (m.kind == geo::name_kind::country) {
      if (std::find(q.countries_.begin(), q.countries_.end(), m.country_code) == q.countries_.end()) {
        q.countries_.push_back(m.country_code);
      }
    } else if (!m.region_code.empty() &&
               std::find(q.regions_.begin(), q.regions_.end(), m.region_code) == q.regions_.end()) {
      q.regions_.push_back(m.region_code);
    }
  }
  q.empty_ = false;
  return q;
}

std::string place_query::why(const geo::database& db, double lat, double lon, const geo::lookup_options& opts) const {
  if (empty_) return {};
  // The town is matched on the town the label names, so the person's own
  // radius setting decides it too.
  const geo::place p = db.lookup(lat, lon, opts);
  if (p.label.empty() || p.at_sea) return {};
  const auto has = [](const std::vector<std::string>& v, const std::string& x) {
    return !x.empty() && std::find(v.begin(), v.end(), x) != v.end();
  };
  if (has(countries_, p.country_code) || has(regions_, p.region_code)) return p.label;
  if (!p.town.empty()) {
    const std::string town = geo::fold(p.town);
    if (town == text_ || (prefix_ && town.rfind(text_, 0) == 0)) return p.label;
  }
  return {};
}

}  // namespace mv::places
