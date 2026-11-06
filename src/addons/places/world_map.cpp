// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The world map (world_map.h; docs/plans/places-world-map.md).
#include "addons/places/world_map.h"

#include <algorithm>
#include <cmath>

#include "core/json.h"

namespace mv::places {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr double kMaxMercLat = 85.0;  // as map.cpp's projection
constexpr double kEps = 1e-9;
// What the map can and cannot show (the section says the same).
constexpr std::string_view kCaption = "No street detail \xE2\x80\x94 offline map";

double merc(double lat_deg) noexcept {
  const double phi = std::clamp(lat_deg, -kMaxMercLat, kMaxMercLat) * kDeg;
  return std::log(std::tan(kPi / 4 + phi / 2));
}

double inv_merc(double m) noexcept { return std::atan(std::sinh(m)) / kDeg; }

bool same_view(const map_view& a, const map_view& b) noexcept {
  return a.width == b.width && a.height == b.height && std::fabs(a.scale - b.scale) < 1e-6f &&
         std::fabs(a.span_deg - b.span_deg) < kEps && std::fabs(a.lat - b.lat) < kEps && std::fabs(a.lon - b.lon) < kEps;
}

// "12,345": the count in the status line.
std::string grouped(std::size_t n) {
  std::string digits = std::to_string(n);
  std::string out;
  for (std::size_t i = 0; i < digits.size(); ++i) {
    if (i != 0 && (digits.size() - i) % 3 == 0) out.push_back(',');
    out.push_back(digits[i]);
  }
  return out;
}

// The widest span with the centre on the equator: the map's last zoom-out step.
double widest_at_equator(const map_view& v) noexcept {
  map_view e = v;
  e.lat = 0;
  return world_map::widest_span(e);
}

bool at_widest(const map_view& v) noexcept { return v.span_deg >= widest_at_equator(v) - 1e-6; }
bool at_closest(const map_view& v) noexcept { return v.span_deg <= k_min_span_deg + kEps; }

std::string status_text(const world_state& s) {
  if (s.reading) return "Finding your geotagged photos\xE2\x80\xA6";
  if (!s.points) return "Your library could not be read.";
  const std::size_t n = s.points->size();
  if (s.points->whole_index) {
    if (n == 0) return "Nothing in your library has a place yet.";
    return grouped(n) + (n == 1 ? " photo or video with a place" : " photos and videos with a place");
  }
  return "Showing this folder (" + grouped(n) +
         "). Index your library in Local search to see everything.";
}

// The map surface's moving props. In a diff a pin off the picture is
// removed (null); a whole tree just leaves it out.
void map_props(json::writer& w, const world_state& s, const map_image* img, bool diff) {
  bool pin_on = false;
  double px = 0, py = 0;
  if (s.pin && s.view.width && s.view.height) {
    map_pixel(s.view, s.pin->lat, s.pin->lon, px, py);
    px /= s.view.width;
    py /= s.view.height;
    pin_on = px >= 0 && px <= 1 && py >= 0 && py <= 1;
  }
  if (pin_on) {
    w.key("pin").begin_object().key("x").number(px).key("y").number(py).end_object();
  } else if (diff) {
    w.key("pin").null();
  }
  w.key("marks").begin_array();
  if (img) {
    for (const map_mark& m : img->marks) {
      w.begin_object().key("x").number(m.x).key("y").number(m.y).key("text").string(m.text).end_object();
    }
  }
  w.end_array();
  // How far the chrome may scale the picture live; the host bounds these to
  // [0.01, 1] and [1, 1000].
  const double span = std::max(s.view.span_deg, k_min_span_deg);
  w.key("interactive").begin_object();
  w.key("min_scale").number(std::clamp(span / widest_at_equator(s.view), 0.01, 1.0));
  w.key("max_scale").number(std::clamp(span / k_min_span_deg, 1.0, 1000.0));
  w.end_object();
}

}  // namespace

// ---- world_points ----------------------------------------------------------------------

void world_points::add(std::string_view path, double lat, double lon) {
  if (points_.size() >= k_max_world_items) return;
  points_.push_back({lat, lon});
  paths_.append(path);
  ends_.push_back(paths_.size());
}

std::string_view world_points::path(std::size_t i) const noexcept {
  if (i >= ends_.size()) return {};
  const std::size_t b = i == 0 ? 0 : ends_[i - 1];
  return std::string_view(paths_).substr(b, ends_[i] - b);
}

std::size_t world_points::find(std::string_view p) const noexcept {
  if (p.empty()) return k_no_item;
  for (std::size_t i = 0; i < ends_.size(); ++i) {
    const std::size_t b = i == 0 ? 0 : ends_[i - 1];
    if (ends_[i] - b == p.size() && same_path(std::string_view(paths_).substr(b, ends_[i] - b), p)) return i;
  }
  return k_no_item;
}

// ---- the view ----------------------------------------------------------------------------

map_view world_map::sized(map_view v, float width_pt, float height_pt, float scale) noexcept {
  const double w = std::clamp(std::isfinite(width_pt) ? static_cast<double>(width_pt) : 0.0, 200.0, 20000.0);
  const double h = std::clamp(std::isfinite(height_pt) ? static_cast<double>(height_pt) : 0.0, 120.0, 20000.0);
  double s = std::clamp(std::isfinite(scale) && scale > 0 ? static_cast<double>(scale) : 1.0, 1.0, 3.0);
  s = std::min({s, 4096.0 / w, 4096.0 / h, std::sqrt(k_world_max_pixels / (w * h))});
  v.width = static_cast<std::uint32_t>(std::clamp(std::floor(w * s + 0.5), 1.0, 4096.0));
  v.height = static_cast<std::uint32_t>(std::clamp(std::floor(h * s + 0.5), 1.0, 4096.0));
  v.scale = static_cast<float>(s);
  return v;
}

double world_map::widest_span(const map_view& v) noexcept {
  if (v.width == 0 || v.height == 0) return k_world_span_deg;
  // The projection's k = H cos(lat) / span (per radian): the world's width
  // 2 pi k fills the picture's W when span = 360 H cos(lat) / W degrees.
  const double lat = std::clamp(std::isfinite(v.lat) ? v.lat : 0.0, -80.0, 80.0);
  const double span = 360.0 * v.height * std::cos(lat * kDeg) / v.width;
  return std::clamp(span, k_min_span_deg, k_world_span_deg);
}

map_view world_map::clamp_view(map_view v) noexcept {
  v.max_span_deg = k_world_span_deg;
  if (!std::isfinite(v.lat)) v.lat = 0;
  if (!std::isfinite(v.lon)) v.lon = 0;
  v.lat = std::clamp(v.lat, -80.0, 80.0);
  v.lon = std::fmod(v.lon + 540.0, 360.0) - 180.0;
  if (!std::isfinite(v.span_deg)) v.span_deg = widest_at_equator(v);
  // Out as far as the whole world: there the map sits on the equator, so the
  // last step out is the same wherever the person was.
  const double world = widest_at_equator(v);
  if (v.span_deg >= world * 0.98) {
    v.lat = 0;
    v.span_deg = world;
  } else {
    v.span_deg = std::clamp(v.span_deg, k_min_span_deg, widest_span(v));
  }
  return v;
}

map_view world_map::frame(const map_view& v, std::span<const route_point> points) noexcept {
  map_view out = v;
  out.max_span_deg = k_world_span_deg;
  double lat0 = 90, lat1 = -90, a0 = 180, a1 = -180, b0 = 360, b1 = 0;
  std::size_t n = 0;
  for (const route_point& p : points) {
    if (!std::isfinite(p.lat) || !std::isfinite(p.lon) || std::fabs(p.lat) > 90 || std::fabs(p.lon) > 180) continue;
    ++n;
    lat0 = std::min(lat0, p.lat);
    lat1 = std::max(lat1, p.lat);
    a0 = std::min(a0, p.lon);
    a1 = std::max(a1, p.lon);
    const double b = p.lon < 0 ? p.lon + 360 : p.lon;  // the same, unwrapped across 180 degrees
    b0 = std::min(b0, b);
    b1 = std::max(b1, b);
  }
  if (n == 0 || v.width == 0 || v.height == 0) return clamp_view(out);
  // The narrower of the two longitude boxes: a trip across the Pacific is one box.
  const bool across = b1 - b0 < a1 - a0;
  const double lon0 = across ? b0 : a0;
  const double lon1 = across ? b1 : a1;
  const double m0 = merc(lat0), m1 = merc(lat1);
  out.lat = std::clamp(inv_merc((m0 + m1) / 2), -80.0, 80.0);
  out.lon = (lon0 + lon1) / 2;
  if (out.lon > 180) out.lon -= 360;
  // span (radians) = H cos(lat) / k; the box, with a fifth more round it,
  // must fit both ways.
  const double c = std::cos(out.lat * kDeg);
  const double need_y = (m1 - m0) * c / 0.8;
  const double need_x = (lon1 - lon0) * kDeg * v.height * c / (0.8 * v.width);
  out.span_deg = std::max({need_x, need_y, 0.0}) / kDeg;
  out.span_deg = std::max(out.span_deg, 8 * k_min_span_deg);  // one photo shows its region
  return clamp_view(out);
}

void world_map::open(world_state& s, const theme_tokens& theme, float width_pt, float height_pt, float scale,
                     std::string_view pin_path, std::optional<route_point> pin) const {
  s = world_state{};
  s.theme = theme;
  s.width_pt = width_pt > 0 ? width_pt : k_world_width_pt;
  s.height_pt = height_pt > 0 ? height_pt : k_world_height_pt;
  s.scale = scale;
  s.pin_path = std::string(pin_path);
  if (pin && std::isfinite(pin->lat) && std::isfinite(pin->lon) && std::fabs(pin->lat) <= 90 &&
      std::fabs(pin->lon) <= 180) {
    s.pin = pin;
  }
  map_view v;
  v.lat = s.pin ? s.pin->lat : 20.0;
  v.lon = s.pin ? s.pin->lon : 0.0;
  v = sized(v, s.width_pt, s.height_pt, scale);
  // Until the library is read: the pin's part of the world, or all of it.
  v.span_deg = s.pin ? k_zoom_span_deg[1] : k_world_span_deg;
  s.view = clamp_view(v);
}

bool world_map::resize(world_state& s, float width_pt, float height_pt, float scale) const {
  if (!(width_pt > 0) || !(height_pt > 0) || !std::isfinite(width_pt) || !std::isfinite(height_pt)) return false;
  if (std::fabs(width_pt - s.width_pt) < 0.5f && std::fabs(height_pt - s.height_pt) < 0.5f &&
      std::fabs(scale - s.scale) < 1e-3f) {
    return false;
  }
  // The same ground per point: a bigger window shows more, at the same zoom.
  map_view v = s.view;
  if (s.height_pt > 0) v.span_deg *= static_cast<double>(height_pt) / s.height_pt;
  s.width_pt = width_pt;
  s.height_pt = height_pt;
  s.scale = scale;
  v = clamp_view(sized(v, width_pt, height_pt, scale));
  s.view = v;
  return true;
}

bool world_map::gesture(world_state& s, double scale, double anchor_x, double anchor_y, double pan_x,
                        double pan_y) const {
  const map_view next = clamp_view(apply_view_gesture(s.view, scale, anchor_x, anchor_y, pan_x, pan_y));
  if (same_view(next, s.view)) return false;
  s.view = next;
  s.moved = true;
  return true;
}

bool world_map::zoom(world_state& s, int delta) const {
  if (delta == 0) return false;
  return gesture(s, std::pow(2.0, delta), 0.5, 0.5, 0, 0);
}

void world_map::set_points(world_state& s, std::shared_ptr<const world_points> points) const {
  s.points = std::move(points);
  s.reading = false;
  s.pin_index = s.points ? s.points->find(s.pin_path) : k_no_item;
  if (!s.moved && s.points && s.points->size() != 0) s.view = frame(s.view, s.points->points());
}

// ---- pictures and trees -----------------------------------------------------------------

std::shared_ptr<const map_image> world_map::picture(world_state& s) {
  s.dots.clear();
  const geo::database* db = db_();
  if (!db) return nullptr;
  auto base = cache_.get(*db, s.view, palette_for(s.theme), {});
  if (!base || !s.points || s.points->size() == 0) return base;
  cluster_points(s.view, s.points->points(), s.pin_index, s.dots);
  if (s.dots.empty()) return base;
  auto img = std::make_shared<map_image>(*base);
  if (!draw_dots(s.view, dot_palette_for(s.theme), s.dots, img->bgra)) {
    s.dots.clear();
    return base;
  }
  return img;
}

std::string world_map::tree(const world_state& s, const map_image* img) const {
  json::writer w;
  w.begin_object().key("type").string("stack").key("id").string("root").key("spacing").integer(8);
  w.key("children").begin_array();
  w.begin_object().key("type").string("surface").key("id").string("map").key("surface").string("map");
  w.key("fill").boolean(true);
  // One element for VoiceOver / Narrator; the picture itself is not read.
  w.key("label").string("World map of your photos");
  w.key("pin_label").string("The photo on screen");
  map_props(w, s, img, false);
  w.end_object();
  w.begin_object().key("type").string("stack").key("id").string("bar").key("axis").string("horizontal");
  w.key("spacing").integer(8).key("align").string("center").key("children").begin_array();
  w.begin_object().key("type").string("text").key("id").string("status").key("text").string(status_text(s));
  w.key("style").string("caption").end_object();
  w.begin_object().key("type").string("text").key("id").string("caption").key("text").string(kCaption);
  w.key("style").string("caption").end_object();
  w.begin_object().key("type").string("button").key("id").string("zoom_out").key("label").string("Zoom out");
  w.key("symbol").string("minus").key("disabled").boolean(at_widest(s.view)).end_object();
  w.begin_object().key("type").string("button").key("id").string("zoom_in").key("label").string("Zoom in");
  w.key("symbol").string("plus").key("disabled").boolean(at_closest(s.view)).end_object();
  w.end_array().end_object();
  w.end_array().end_object();
  return w.take();
}

std::string world_map::diff(const world_state& s, const map_image* img) const {
  json::writer w;
  w.begin_array();
  w.begin_object().key("op").string("set").key("id").string("map").key("props").begin_object();
  map_props(w, s, img, true);
  w.end_object().end_object();
  w.begin_object().key("op").string("set").key("id").string("status").key("props").begin_object();
  w.key("text").string(status_text(s)).end_object().end_object();
  w.begin_object().key("op").string("set").key("id").string("zoom_out").key("props").begin_object();
  w.key("disabled").boolean(at_widest(s.view)).end_object().end_object();
  w.begin_object().key("op").string("set").key("id").string("zoom_in").key("props").begin_object();
  w.key("disabled").boolean(at_closest(s.view)).end_object().end_object();
  w.end_array();
  return w.take();
}

// ---- a click --------------------------------------------------------------------------------

world_hit world_map::tap(const world_state& s, double x, double y) const {
  world_hit h;
  if (!s.points || s.dots.empty() || !std::isfinite(x) || !std::isfinite(y)) return h;
  const map_dot* d = dot_at(s.dots, s.view, x * s.view.width, y * s.view.height);
  if (!d) return h;
  const auto index = static_cast<std::uint32_t>(d - s.dots.data());
  // The same clustering as the picture's (same view, points and pin), with
  // each point's dot: the clicked dot's members, in walk order.
  std::vector<map_dot> dots;
  std::vector<std::uint32_t> item_dot;
  cluster_points(s.view, s.points->points(), s.pin_index, dots, &item_dot);
  if (index >= dots.size() || dots[index].count != d->count) return h;
  h.count = d->count;
  for (std::size_t i = 0; i < item_dot.size() && h.items.size() < k_world_max_list; ++i) {
    if (item_dot[i] == index) h.items.push_back(static_cast<std::uint32_t>(i));
  }
  return h;
}

std::string world_map::title_of(const world_state& s, const world_hit& h, const geo::lookup_options& opts) const {
  std::string title;
  const geo::database* db = db_();
  if (db && s.points && !h.items.empty() && h.items.front() < s.points->size()) {
    const route_point p = s.points->points()[h.items.front()];
    title = db->lookup(p.lat, p.lon, opts).label;
  }
  if (title.empty()) title = "Places";
  if (h.count > 1) title += " (" + grouped(h.count) + ")";
  return title;
}

}  // namespace mv::places
