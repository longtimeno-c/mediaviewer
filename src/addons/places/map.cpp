// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Places map rasteriser (map.h). See docs/design/27 "The map".
//
// Fill: a signed-area accumulation rasteriser (the font-rs method): every
// polygon edge adds its exact area coverage to a float buffer, one prefix sum
// per row turns that into coverage, and an even-odd fold of the winding keeps
// holes (lakes, enclaves) open. Each polygon is oriented so its outer ring is
// positive, so two countries sharing a border add up to full coverage along it
// instead of leaving a hairline gap. Anti-aliased by construction.
//
// Borders: each edge is stroked by distance to the segment, with a one-pixel
// soft edge, into an alpha buffer (max, so joints do not darken). Only an
// edge two polygons share is a border; an edge of one polygon alone is a
// coast and is skipped: the land/water edge is already crisp, and a coast
// line would double it.
#include "addons/places/map.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mv::places {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr double kMaxMercLat = 85.0;

// ---- colour ----------------------------------------------------------------

struct rgb {
  float r, g, b;
};

rgb unpack(std::uint32_t c) noexcept {
  return {static_cast<float>((c >> 16) & 0xff), static_cast<float>((c >> 8) & 0xff),
          static_cast<float>(c & 0xff)};
}

std::uint32_t pack(rgb c) noexcept {
  const auto q = [](float v) { return static_cast<std::uint32_t>(std::clamp(v + 0.5f, 0.0f, 255.0f)); };
  return (q(c.r) << 16) | (q(c.g) << 8) | q(c.b);
}

std::uint32_t mix(std::uint32_t a, std::uint32_t b, float t) noexcept {
  const rgb x = unpack(a);
  const rgb y = unpack(b);
  return pack({x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t});
}

inline rgb lerp(rgb a, rgb b, float t) noexcept {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

// ---- projection (Web Mercator, centred on the point) -------------------------

double merc(double lat_deg) noexcept {
  const double phi = std::clamp(lat_deg, -kMaxMercLat, kMaxMercLat) * kDeg;
  return std::log(std::tan(kPi / 4 + phi / 2));
}

double inv_merc(double m) noexcept { return std::atan(std::sinh(m)) / kDeg; }

double wrap180(double d) noexcept {
  while (d >= 180) d -= 360;
  while (d < -180) d += 360;
  return d;
}

struct projection {
  double cx = 0, cy = 0;  // pixel centre
  double k = 1;           // pixels per radian (x) and per Mercator unit (y)
  double lon0 = 0;
  double m0 = 0;

  projection(const map_view& v) {
    const double lat0 = std::clamp(v.lat, -kMaxMercLat, kMaxMercLat);
    cx = v.width / 2.0;
    cy = v.height / 2.0;
    lon0 = wrap180(v.lon);
    m0 = merc(lat0);
    // The map's height covers the span in latitude at its centre:
    // d(merc)/d(lat) = 1 / cos(lat).
    k = v.height * std::cos(lat0 * kDeg) / (clamp_span(v.span_deg) * kDeg);
  }
  // The point under pixel (x, y).
  void inverse(double x, double y, double& lat, double& lon) const noexcept {
    lon = wrap180(lon0 + (x - cx) / k / kDeg);
    lat = inv_merc(m0 - (y - cy) / k);
  }
  [[nodiscard]] double x(double dlon_deg) const noexcept { return cx + dlon_deg * kDeg * k; }
  [[nodiscard]] double y(double lat_deg) const noexcept { return cy - (merc(lat_deg) - m0) * k; }

  // The degrees the picture covers, with a margin for strokes, as a query box.
  [[nodiscard]] geo::box bounds(const map_view& v) const noexcept {
    const double margin_px = 8;
    const double half_lon = ((v.width / 2.0 + margin_px) / k) / kDeg;
    const double half_m = (v.height / 2.0 + margin_px) / k;
    geo::box b;
    b.min_lat = inv_merc(m0 - half_m);
    b.max_lat = inv_merc(m0 + half_m);
    if (b.max_lat >= kMaxMercLat - 1e-9) b.max_lat = 90;
    if (b.min_lat <= -kMaxMercLat + 1e-9) b.min_lat = -90;
    if (half_lon >= 180) {
      b.min_lon = -180;
      b.max_lon = 180;
      return b;
    }
    b.min_lon = lon0 - half_lon;
    b.max_lon = lon0 + half_lon;
    if (b.min_lon < -180) b.min_lon += 360;  // min > max: crosses the antimeridian
    if (b.max_lon > 180) b.max_lon -= 360;
    return b;
  }
};

// One ring, projected: longitudes are unwrapped point to point from the first
// (taken nearest the centre), so a ring near the antimeridian stays whole.
void project_ring(const projection& pr, std::span<const geo::point> pts, std::vector<double>& xy,
                  std::vector<std::uint8_t>* seam) {
  xy.clear();
  if (seam) seam->clear();
  if (pts.empty()) return;
  double prev = pts[0].x * 1e-6;
  double dl = wrap180(prev - pr.lon0);
  for (const geo::point& p : pts) {
    const double lon = p.x * 1e-6;
    dl += wrap180(lon - prev);
    prev = lon;
    xy.push_back(pr.x(dl));
    xy.push_back(pr.y(p.y * 1e-6));
    // Natural Earth splits features at +/-180 degrees; that cut is not a border.
    if (seam) seam->push_back(p.x == 180'000'000 || p.x == -180'000'000 ? 1 : 0);
  }
}

// ---- fill --------------------------------------------------------------------

struct accumulator {
  std::uint32_t w = 0, h = 0, stride = 0;
  std::vector<float> a;

  void reset(std::uint32_t width, std::uint32_t height) {
    w = width;
    h = height;
    stride = width + 2;
    a.assign(static_cast<std::size_t>(stride) * h, 0.0f);
  }

  // Adds the signed area of the edge (x0,y0)-(x1,y1), times `sign`.
  void line(double x0, double y0, double x1, double y1, float sign) noexcept {
    if (y0 == y1) return;
    float dir = sign;
    if (y0 > y1) {
      std::swap(x0, x1);
      std::swap(y0, y1);
      dir = -dir;
    }
    if (y1 <= 0 || y0 >= h) return;
    if (std::max(x0, x1) <= 0) {
      // Wholly left of the picture: it still turns the winding for every
      // pixel to its right, as a vertical edge at x = 0.
      x0 = x1 = 0;
    } else if (std::min(x0, x1) >= w) {
      return;
    }
    const double dxdy = (x1 - x0) / (y1 - y0);
    double x = x0;
    double ya = y0;
    if (ya < 0) {
      x += dxdy * (0 - ya);
      ya = 0;
    }
    const double yb = std::min<double>(y1, h);
    const int row0 = static_cast<int>(ya);
    const int row1 = static_cast<int>(std::ceil(yb));
    const double fw = w;
    for (int row = row0; row < row1; ++row) {
      const double top = std::max<double>(row, ya);
      const double bottom = std::min<double>(row + 1, yb);
      const double dy = bottom - top;
      if (dy <= 0) continue;
      const double xnext = x + dxdy * dy;
      const float d = static_cast<float>(dy) * dir;
      double xa = std::clamp(x, 0.0, fw);
      double xb = std::clamp(xnext, 0.0, fw);
      if (xa > xb) std::swap(xa, xb);
      float* ls = a.data() + static_cast<std::size_t>(row) * stride;
      const double xa_floor = std::floor(xa);
      const int xai = static_cast<int>(xa_floor);
      const double xb_ceil = std::ceil(xb);
      const int xbi = static_cast<int>(xb_ceil);
      if (xbi <= xai + 1) {
        const float xmf = static_cast<float>(0.5 * (xa + xb) - xa_floor);
        ls[xai] += d - d * xmf;
        ls[xai + 1] += d * xmf;
      } else {
        const double s = 1.0 / (xb - xa);
        const double xaf = xa - xa_floor;
        const double a0 = 0.5 * s * (1 - xaf) * (1 - xaf);
        const double xbf = xb - xb_ceil + 1;
        const double am = 0.5 * s * xbf * xbf;
        ls[xai] += static_cast<float>(d * a0);
        if (xbi == xai + 2) {
          ls[xai + 1] += static_cast<float>(d * (1 - a0 - am));
        } else {
          const double a1 = s * (1.5 - xaf);
          ls[xai + 1] += static_cast<float>(d * (a1 - a0));
          for (int xi = xai + 2; xi < xbi - 1; ++xi) ls[xi] += static_cast<float>(d * s);
          const double a2 = a1 + (xbi - xai - 3) * s;
          ls[xbi - 1] += static_cast<float>(d * (1 - a2 - am));
        }
        ls[xbi] += static_cast<float>(d * am);
      }
      x = xnext;
    }
  }

  // Prefix sums into coverage 0..1, even-odd.
  void resolve(std::vector<float>& cov) const {
    cov.resize(static_cast<std::size_t>(w) * h);
    for (std::uint32_t y = 0; y < h; ++y) {
      const float* ls = a.data() + static_cast<std::size_t>(y) * stride;
      float* out = cov.data() + static_cast<std::size_t>(y) * w;
      float sum = 0;
      for (std::uint32_t x = 0; x < w; ++x) {
        sum += ls[x];
        float f = std::fabs(sum);
        if (f > 1.0f) {
          f = std::fmod(f, 2.0f);
          if (f > 1.0f) f = 2.0f - f;
        }
        out[x] = f;
      }
    }
  }
};

double signed_area(const std::vector<double>& xy) noexcept {
  double s = 0;
  const std::size_t n = xy.size() / 2;
  for (std::size_t i = 0; i + 1 < n; ++i) {
    s += xy[2 * i] * xy[2 * i + 3] - xy[2 * i + 2] * xy[2 * i + 1];
  }
  return s;
}

// ---- strokes ---------------------------------------------------------------

struct stroker {
  std::uint32_t w = 0, h = 0;
  std::vector<float> a;

  void reset(std::uint32_t width, std::uint32_t height) {
    w = width;
    h = height;
    a.assign(static_cast<std::size_t>(w) * h, 0.0f);
  }

  // A round-capped segment of half-width `half` pixels, alpha `alpha`.
  void segment(double ax, double ay, double bx, double by, double half, float alpha) noexcept {
    const double r = half + 1.0;
    const double y_lo = std::max(0.0, std::floor(std::min(ay, by) - r));
    const double y_hi = std::min<double>(h, std::ceil(std::max(ay, by) + r));
    if (y_lo >= y_hi) return;
    const double vx = bx - ax;
    const double vy = by - ay;
    const double len2 = vx * vx + vy * vy;
    // A thin line is fainter rather than thinner than a pixel.
    const float thin = static_cast<float>(std::min(1.0, 2.0 * half));
    for (int py = static_cast<int>(y_lo); py < static_cast<int>(y_hi); ++py) {
      const double yc = py + 0.5;
      double x_lo;
      double x_hi;
      if (std::fabs(vy) < 1e-9) {
        x_lo = std::min(ax, bx) - r;
        x_hi = std::max(ax, bx) + r;
      } else {
        double t0 = (yc - r - ay) / vy;
        double t1 = (yc + r - ay) / vy;
        if (t0 > t1) std::swap(t0, t1);
        t0 = std::clamp(t0, 0.0, 1.0);
        t1 = std::clamp(t1, 0.0, 1.0);
        x_lo = std::min(ax + vx * t0, ax + vx * t1) - r;
        x_hi = std::max(ax + vx * t0, ax + vx * t1) + r;
      }
      const int xs = std::max(0, static_cast<int>(std::floor(x_lo)));
      const int xe = std::min(static_cast<int>(w), static_cast<int>(std::ceil(x_hi)));
      float* row = a.data() + static_cast<std::size_t>(py) * w;
      for (int px = xs; px < xe; ++px) {
        const double xc = px + 0.5;
        double t = len2 > 0 ? ((xc - ax) * vx + (yc - ay) * vy) / len2 : 0;
        t = std::clamp(t, 0.0, 1.0);
        const double dx = xc - (ax + t * vx);
        const double dy = yc - (ay + t * vy);
        const double dist = std::sqrt(dx * dx + dy * dy);
        const float c = static_cast<float>(std::clamp(half + 0.5 - dist, 0.0, 1.0)) * thin * alpha;
        if (c > row[px]) row[px] = c;
      }
    }
  }
};

// Strokes the edges two polygons of `layer` share: the land borders. The
// database simplifies each shared arc once and gives both neighbours the same
// points (docs/design/27 "Polygons"), so a border edge appears exactly twice
// and a coast edge once. Exact, with no guess from the pixels: a narrow
// estuary is still a coast.
struct edge_rec {
  std::uint64_t k0, k1;  // the two end points, packed, in order
  double ax, ay, bx, by;
};

void stroke_borders(const geo::database& db, geo::layer layer, const std::vector<std::uint32_t>& polys,
                    const projection& pr, stroker& s, double half, float alpha) {
  std::vector<double> xy;
  std::vector<std::uint8_t> seam;
  std::vector<edge_rec> edges;
  const double w = s.w;
  const double h = s.h;
  const auto pack_point = [](const geo::point& q) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(q.x)) << 32) | static_cast<std::uint32_t>(q.y);
  };
  for (std::uint32_t p : polys) {
    const std::size_t rings = db.ring_count(layer, p);
    for (std::size_t r = 0; r < rings; ++r) {
      const auto pts = db.ring(layer, p, r);
      project_ring(pr, pts, xy, &seam);
      const std::size_t n = xy.size() / 2;
      for (std::size_t i = 0; i + 1 < n; ++i) {
        if (seam[i] && seam[i + 1]) continue;  // Natural Earth's cut at +/-180, not a border
        const double ax = xy[2 * i], ay = xy[2 * i + 1];
        const double bx = xy[2 * i + 2], by = xy[2 * i + 3];
        if ((ax < -2 && bx < -2) || (ax > w + 2 && bx > w + 2) || (ay < -2 && by < -2) ||
            (ay > h + 2 && by > h + 2)) {
          continue;  // off the picture
        }
        std::uint64_t a = pack_point(pts[i]);
        std::uint64_t b = pack_point(pts[i + 1]);
        if (a == b) continue;
        if (a > b) std::swap(a, b);
        edges.push_back({a, b, ax, ay, bx, by});
      }
    }
  }
  std::sort(edges.begin(), edges.end(), [](const edge_rec& x, const edge_rec& y) {
    return x.k0 != y.k0 ? x.k0 < y.k0 : x.k1 < y.k1;
  });
  for (std::size_t i = 0; i < edges.size();) {
    std::size_t j = i + 1;
    while (j < edges.size() && edges[j].k0 == edges[i].k0 && edges[j].k1 == edges[i].k1) ++j;
    if (j - i >= 2) s.segment(edges[i].ax, edges[i].ay, edges[i].bx, edges[i].by, half, alpha);
    i = j;
  }
}

void fill(const geo::database& db, geo::layer layer, const std::vector<std::uint32_t>& polys,
          const projection& pr, accumulator& acc, std::string_view only_country) {
  std::vector<double> xy;
  for (std::uint32_t p : polys) {
    if (!only_country.empty() && db.polygon_country_code(layer, p) != only_country) continue;
    const std::size_t rings = db.ring_count(layer, p);
    float sign = 1.0f;
    for (std::size_t r = 0; r < rings; ++r) {
      project_ring(pr, db.ring(layer, p, r), xy, nullptr);
      if (r == 0) sign = signed_area(xy) < 0 ? -1.0f : 1.0f;
      const std::size_t n = xy.size() / 2;
      for (std::size_t i = 0; i + 1 < n; ++i) {
        acc.line(xy[2 * i], xy[2 * i + 1], xy[2 * i + 2], xy[2 * i + 3], sign);
      }
    }
  }
}

}  // namespace

void map_pixel(const map_view& v, double lat, double lon, double& x, double& y) noexcept {
  const projection pr(v);
  x = pr.x(wrap180(lon - pr.lon0));
  y = pr.y(lat);
}

map_palette palette_for(const theme_tokens& t) noexcept {
  const std::uint32_t surface = t.surface >> 8;
  const std::uint32_t canvas = t.canvas >> 8;
  const std::uint32_t body = t.body >> 8;
  const std::uint32_t disabled = t.disabled >> 8;
  map_palette p;
  if (t.dark) {
    p.land_home = mix(surface, 0xffffff, 0.14f);
    p.land = mix(surface, 0xffffff, 0.07f);
    p.water = mix(mix(canvas, surface, 0.5f), 0x163452, 0.40f);
    p.border = mix(p.land_home, body, 0.50f);
    p.region = mix(p.land_home, body, 0.24f);
  } else {
    p.land_home = mix(surface, 0xffffff, 0.75f);
    p.land = mix(mix(surface, 0xffffff, 0.35f), disabled, 0.10f);
    p.water = mix(surface, 0x8fc1ec, 0.62f);
    p.border = mix(p.land_home, body, 0.42f);
    p.region = mix(p.land_home, body, 0.20f);
  }
  p.frame = t.hairline >> 8;
  p.town = mix(p.land_home, t.title >> 8, t.dark ? 0.70f : 0.62f);
  return p;
}

int framing_zoom(const geo::database& db, double lat, double lon, std::string_view country_code,
                 std::string_view region_code, std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0) return k_zoom_steps - 1;
  if (country_code.empty()) return 2;  // open sea: enough to see the nearest coast
  const double y0 = std::clamp(lat, -89.0, 89.0);
  const geo::box here{y0 - 0.01, wrap180(lon) - 0.01, y0 + 0.01, wrap180(lon) + 0.01};
  // The smallest polygon box round the point, regions first (of the point's
  // country), then the country. A box is not the shape, but it frames it.
  const auto smallest = [&](geo::layer l, double& out_lat_span, double& out_lon_span) {
    bool found = false;
    double best = 1e18;
    for (std::uint32_t p : db.polygons_in(l, here)) {
      if (db.polygon_country_code(l, p) != country_code) continue;
      const auto ring = db.ring(l, p, 0);
      if (ring.empty()) continue;
      double a = 90, b = -90, c = 1e9, d = -1e9;
      double prev = ring[0].x * 1e-6;
      double dl = wrap180(prev - lon);
      for (const geo::point& pt : ring) {
        const double plon = pt.x * 1e-6;
        dl += wrap180(plon - prev);
        prev = plon;
        a = std::min(a, pt.y * 1e-6);
        b = std::max(b, pt.y * 1e-6);
        c = std::min(c, dl);
        d = std::max(d, dl);
      }
      if (lat < a || lat > b || c > 0 || d < 0) continue;
      const double area = (b - a) * (d - c);
      if (area < best) {
        best = area;
        out_lat_span = b - a;
        out_lon_span = d - c;
        found = true;
      }
    }
    return found;
  };
  double lat_span = 0;
  double lon_span = 0;
  if (region_code.empty() || !smallest(geo::layer::regions, lat_span, lon_span)) {
    if (!smallest(geo::layer::countries, lat_span, lon_span)) return 2;
  }
  // The span of latitude the picture must show, so the shape fits both ways
  // with a margin: longitude spans are narrower on the map by cos(lat).
  const double aspect = static_cast<double>(width) / height;
  const double need = 1.3 * std::max(lat_span, lon_span * std::cos(y0 * kDeg) / aspect);
  for (int z = k_region_zoom_step; z >= 0; --z) {
    if (k_zoom_span_deg[z] >= need) return z;
  }
  return 0;
}

double clamp_span(double span_deg) noexcept {
  if (!std::isfinite(span_deg)) return k_zoom_span_deg[k_region_zoom_step];
  return std::clamp(span_deg, k_min_span_deg, k_max_span_deg);
}

void map_point(const map_view& v, double x, double y, double& lat, double& lon) noexcept {
  projection(v).inverse(x, y, lat, lon);
}

map_view keep_pin_in_view(const map_view& v, double pin_lat, double pin_lon, double margin) noexcept {
  map_view out = v;
  out.span_deg = clamp_span(out.span_deg);
  out.lat = std::clamp(out.lat, -80.0, 80.0);
  if (v.width == 0 || v.height == 0) return out;
  double x = 0, y = 0;
  map_pixel(out, pin_lat, pin_lon, x, y);
  const double lo_x = margin * v.width, hi_x = (1 - margin) * v.width;
  const double lo_y = margin * v.height, hi_y = (1 - margin) * v.height;
  const double dx = x < lo_x ? x - lo_x : x > hi_x ? x - hi_x : 0;
  const double dy = y < lo_y ? y - lo_y : y > hi_y ? y - hi_y : 0;
  if (dx == 0 && dy == 0) return out;
  // Move the centre by the overshoot, at the same pixel scale.
  const projection pr(out);
  double lat = 0, lon = 0;
  pr.inverse(out.width / 2.0 + dx, out.height / 2.0 + dy, lat, lon);
  const double k_before = std::cos(out.lat * kDeg) / out.span_deg;
  out.lat = std::clamp(lat, -80.0, 80.0);
  out.lon = lon;
  out.span_deg = clamp_span(std::cos(out.lat * kDeg) / k_before);
  return out;
}

map_view apply_gesture(const map_view& v, double scale, double anchor_x, double anchor_y, double pan_x, double pan_y,
                       double pin_lat, double pin_lon) noexcept {
  map_view out = v;
  if (!(scale > 0) || !std::isfinite(scale) || !std::isfinite(anchor_x) || !std::isfinite(anchor_y) ||
      !std::isfinite(pan_x) || !std::isfinite(pan_y) || v.width == 0 || v.height == 0) {
    return keep_pin_in_view(out, pin_lat, pin_lon);
  }
  // The old picture's point that now shows at the centre:
  // centre = a + (p - a) * s + pan  =>  p = a + (centre - pan - a) / s.
  const double px = anchor_x + (0.5 - pan_x - anchor_x) / scale;
  const double py = anchor_y + (0.5 - pan_y - anchor_y) / scale;
  double lat = 0, lon = 0;
  map_point(v, px * v.width, py * v.height, lat, lon);
  // Same pixels per Mercator unit as the gesture showed: k = H cos(lat) / span.
  const double lat_old = std::clamp(v.lat, -kMaxMercLat, kMaxMercLat);
  const double lat_new = std::clamp(lat, -80.0, 80.0);
  out.lat = lat_new;
  out.lon = lon;
  out.span_deg = clamp_span(clamp_span(v.span_deg) * std::cos(lat_new * kDeg) / std::cos(lat_old * kDeg) / scale);
  return keep_pin_in_view(out, pin_lat, pin_lon);
}

bool render_map(const geo::database& db, const map_view& v, const map_palette& p, std::string_view home_country,
                std::span<std::uint8_t> out, std::vector<map_mark>* marks) {
  const std::uint32_t w = v.width;
  const std::uint32_t h = v.height;
  if (w == 0 || h == 0 || w > 4096 || h > 4096 || out.size() < static_cast<std::size_t>(w) * h * 4) return false;
  const float scale = v.scale > 0 ? v.scale : 1.0f;
  const projection pr(v);
  const geo::box b = pr.bounds(v);
  const double span = clamp_span(v.span_deg);

  std::vector<std::uint32_t> countries;
  if (db.valid()) countries = db.polygons_in(geo::layer::countries, b);

  accumulator acc;
  std::vector<float> land;
  std::vector<float> home;
  acc.reset(w, h);
  fill(db, geo::layer::countries, countries, pr, acc, {});
  acc.resolve(land);
  if (!home_country.empty()) {
    acc.reset(w, h);
    fill(db, geo::layer::countries, countries, pr, acc, home_country);
    acc.resolve(home);
  }

  stroker region;
  stroker border;
  region.reset(w, h);
  border.reset(w, h);
  if (span <= k_region_borders_span && db.valid()) {
    const std::vector<std::uint32_t> regions = db.polygons_in(geo::layer::regions, b);
    if (span <= k_region_borders_span / 2 || regions.size() <= k_region_borders_max_early) {
      stroke_borders(db, geo::layer::regions, regions, pr, region, 0.35 * scale, 1.0f);
    }
  }
  stroke_borders(db, geo::layer::countries, countries, pr, border, 0.6 * scale, 1.0f);

  // Town dots, with a halo in the land colour so they read on any border.
  std::vector<float> halo;
  std::vector<float> dots;
  if (marks) marks->clear();
  if (span <= k_towns_span && db.valid()) {
    const double min_people = 1000.0 * std::pow(span / k_min_span_deg, 1.5);
    halo.assign(static_cast<std::size_t>(w) * h, 0.0f);
    dots.assign(static_cast<std::size_t>(w) * h, 0.0f);
    std::size_t drawn = 0;
    for (const auto& t : db.towns_in(b, 512)) {
      if (t.population < min_people || drawn >= k_max_town_dots) break;  // most populous first
      double x = 0, y = 0;
      map_pixel(v, t.lat, t.lon, x, y);
      const double inset = 4.0 * scale;
      if (x < inset || y < inset || x > w - inset || y > h - inset) continue;
      const double r = (t.population >= 100'000 ? 2.4 : t.population >= 20'000 ? 1.9 : 1.5) * scale;
      const double rh = r + 1.2 * scale;
      const int y0 = std::max(0, static_cast<int>(y - rh - 1));
      const int y1 = std::min(static_cast<int>(h) - 1, static_cast<int>(y + rh + 1));
      const int x0 = std::max(0, static_cast<int>(x - rh - 1));
      const int x1 = std::min(static_cast<int>(w) - 1, static_cast<int>(x + rh + 1));
      for (int py = y0; py <= y1; ++py) {
        for (int px = x0; px <= x1; ++px) {
          const double d = std::hypot(px + 0.5 - x, py + 0.5 - y);
          const std::size_t i = static_cast<std::size_t>(py) * w + static_cast<std::size_t>(px);
          halo[i] = std::max(halo[i], static_cast<float>(std::clamp(rh + 0.5 - d, 0.0, 1.0)));
          dots[i] = std::max(dots[i], static_cast<float>(std::clamp(r + 0.5 - d, 0.0, 1.0)));
        }
      }
      if (marks && marks->size() < k_max_marks && t.name.size() <= 64) {
        marks->push_back({x / w, y / h, std::string(t.name)});  // the host takes names of 64 bytes at most
      }
      ++drawn;
    }
  }

  const rgb water = unpack(p.water);
  const rgb land_c = unpack(p.land);
  const rgb home_c = unpack(p.land_home);
  const rgb region_c = unpack(p.region);
  const rgb border_c = unpack(p.border);
  const rgb frame_c = unpack(p.frame);
  const rgb town_c = unpack(p.town);
  // The rounded corners and a hairline frame, as the pane's other cards.
  const double radius = 8.0 * scale;
  const double frame_w = std::max(1.0, 0.5 * scale);
  const double hw = w / 2.0;
  const double hh = h / 2.0;
  for (std::uint32_t y = 0; y < h; ++y) {
    std::uint8_t* row = out.data() + static_cast<std::size_t>(y) * w * 4;
    const std::size_t base = static_cast<std::size_t>(y) * w;
    const double py = y + 0.5;
    for (std::uint32_t x = 0; x < w; ++x) {
      const std::size_t i = base + x;
      rgb c = lerp(water, land_c, land[i]);
      if (!home.empty()) c = lerp(c, home_c, home[i]);
      c = lerp(c, region_c, region.a[i]);
      c = lerp(c, border_c, border.a[i]);
      if (!dots.empty()) {
        c = lerp(c, home_c, halo[i] * 0.85f);
        c = lerp(c, town_c, dots[i]);
      }
      // Signed distance to the rounded rectangle (negative inside).
      const double px = x + 0.5;
      const double qx = std::fabs(px - hw) - (hw - radius);
      const double qy = std::fabs(py - hh) - (hh - radius);
      const double ox = std::max(qx, 0.0);
      const double oy = std::max(qy, 0.0);
      const double sd = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0) - radius;
      const float alpha = static_cast<float>(std::clamp(0.5 - sd, 0.0, 1.0));
      const double depth = -sd;
      const float fa = static_cast<float>(std::clamp(std::min(depth + 0.5, frame_w + 0.5 - depth), 0.0, 1.0));
      c = lerp(c, frame_c, fa);
      // BGRA, premultiplied.
      row[4 * x + 0] = static_cast<std::uint8_t>(std::clamp(c.b * alpha + 0.5f, 0.0f, 255.0f));
      row[4 * x + 1] = static_cast<std::uint8_t>(std::clamp(c.g * alpha + 0.5f, 0.0f, 255.0f));
      row[4 * x + 2] = static_cast<std::uint8_t>(std::clamp(c.r * alpha + 0.5f, 0.0f, 255.0f));
      row[4 * x + 3] = static_cast<std::uint8_t>(std::clamp(255.0f * alpha + 0.5f, 0.0f, 255.0f));
    }
  }
  return true;
}

// ---- cache ---------------------------------------------------------------------

std::shared_ptr<const map_image> map_cache::get(const geo::database& db, const map_view& v, const map_palette& p,
                                                std::string_view home_country) {
  key k;
  k.lat_e6 = std::llround(v.lat * 1e6);
  k.lon_e6 = std::llround(v.lon * 1e6);
  k.width = v.width;
  k.height = v.height;
  k.scale_x100 = static_cast<std::int32_t>(std::lround(v.scale * 100));
  k.span_e6 = std::llround(clamp_span(v.span_deg) * 1e6);
  k.palette = p;
  k.home = std::string(home_country);
  {
    std::lock_guard lock(m_);
    for (auto it = lru_.begin(); it != lru_.end(); ++it) {
      if (it->k == k) {
        ++hits_;
        lru_.splice(lru_.begin(), lru_, it);
        return lru_.front().image;
      }
    }
    ++misses_;
  }
  // Drawn outside the lock: two workers asking for the same new picture at
  // once both draw it (the host keeps one worker per add-on, so in practice
  // they do not), and neither blocks the other.
  auto img = std::make_shared<map_image>();
  img->width = v.width;
  img->height = v.height;
  img->bgra.resize(static_cast<std::size_t>(v.width) * v.height * 4);
  if (!render_map(db, v, p, home_country, img->bgra, &img->marks)) return nullptr;
  std::lock_guard lock(m_);
  lru_.push_front(entry{std::move(k), img});
  while (lru_.size() > capacity_) lru_.pop_back();
  return img;
}

void map_cache::clear() {
  std::lock_guard lock(m_);
  lru_.clear();
}

std::uint64_t map_cache::hits() const {
  std::lock_guard lock(m_);
  return hits_;
}

std::uint64_t map_cache::misses() const {
  std::lock_guard lock(m_);
  return misses_;
}

std::size_t map_cache::size() const {
  std::lock_guard lock(m_);
  return lru_.size();
}

}  // namespace mv::places
