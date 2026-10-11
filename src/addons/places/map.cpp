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
// soft edge, into an alpha buffer (max, so joints do not darken). An edge
// with water on either side is a coast, not a border, and is skipped: the
// land/water edge is already crisp, and a coast line would double it.
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
    const int z = std::clamp(v.zoom, 0, k_zoom_steps - 1);
    cx = v.width / 2.0;
    cy = v.height / 2.0;
    lon0 = wrap180(v.lon);
    m0 = merc(lat0);
    // The map's height covers the step's span in latitude at its centre:
    // d(merc)/d(lat) = 1 / cos(lat).
    k = v.height * std::cos(lat0 * kDeg) / (k_zoom_span_deg[z] * kDeg);
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

// Land coverage at a point, 0 outside the picture (treated as unknown: a
// border running off the edge is still a border).
float land_at(const std::vector<float>& cov, std::uint32_t w, std::uint32_t h, double x, double y,
              bool& outside) noexcept {
  const int xi = static_cast<int>(std::floor(x));
  const int yi = static_cast<int>(std::floor(y));
  if (xi < 0 || yi < 0 || xi >= static_cast<int>(w) || yi >= static_cast<int>(h)) {
    outside = true;
    return 1.0f;
  }
  return cov[static_cast<std::size_t>(yi) * w + xi];
}

// Strokes the edges of `layer`'s polygons that have land on both sides.
void stroke_borders(const geo::database& db, geo::layer layer, const std::vector<std::uint32_t>& polys,
                    const projection& pr, const std::vector<float>& land, stroker& s, double half,
                    float alpha) {
  std::vector<double> xy;
  std::vector<std::uint8_t> seam;
  const double probe = half + 1.75;
  const double w = s.w;
  const double h = s.h;
  for (std::uint32_t p : polys) {
    const std::size_t rings = db.ring_count(layer, p);
    for (std::size_t r = 0; r < rings; ++r) {
      project_ring(pr, db.ring(layer, p, r), xy, &seam);
      const std::size_t n = xy.size() / 2;
      for (std::size_t i = 0; i + 1 < n; ++i) {
        if (seam[i] && seam[i + 1]) continue;
        const double ax = xy[2 * i], ay = xy[2 * i + 1];
        const double bx = xy[2 * i + 2], by = xy[2 * i + 3];
        // Off the picture: nothing to draw.
        if ((ax < -2 && bx < -2) || (ax > w + 2 && bx > w + 2) || (ay < -2 && by < -2) ||
            (ay > h + 2 && by > h + 2)) {
          continue;
        }
        const double vx = bx - ax;
        const double vy = by - ay;
        const double len = std::sqrt(vx * vx + vy * vy);
        if (len < 1e-6) continue;
        const double nx = -vy / len * probe;
        const double ny = vx / len * probe;
        const double mx = (ax + bx) / 2;
        const double my = (ay + by) / 2;
        bool outside = false;
        const float left = land_at(land, s.w, s.h, mx + nx, my + ny, outside);
        const float right = land_at(land, s.w, s.h, mx - nx, my - ny, outside);
        if (left < 0.5f || right < 0.5f) continue;  // a coast
        s.segment(ax, ay, bx, by, half, alpha);
      }
    }
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

map_palette palette_for(const theme_tokens& t) noexcept {
  const std::uint32_t surface = t.surface >> 8;
  const std::uint32_t canvas = t.canvas >> 8;
  const std::uint32_t body = t.body >> 8;
  const std::uint32_t disabled = t.disabled >> 8;
  map_palette p;
  if (t.dark) {
    p.land_home = mix(surface, 0xffffff, 0.11f);
    p.land = mix(surface, 0xffffff, 0.05f);
    p.water = mix(mix(canvas, surface, 0.5f), 0x123a66, 0.45f);
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
  for (int z = k_zoom_steps - 1; z >= 0; --z) {
    if (k_zoom_span_deg[z] >= need) return z;
  }
  return 0;
}

bool render_map(const geo::database& db, const map_view& v, const map_palette& p, std::string_view home_country,
                std::span<std::uint8_t> out) {
  const std::uint32_t w = v.width;
  const std::uint32_t h = v.height;
  if (w == 0 || h == 0 || w > 4096 || h > 4096 || out.size() < static_cast<std::size_t>(w) * h * 4) return false;
  const float scale = v.scale > 0 ? v.scale : 1.0f;
  const projection pr(v);
  const geo::box b = pr.bounds(v);
  const int zoom = std::clamp(v.zoom, 0, k_zoom_steps - 1);

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
  if (zoom >= k_region_borders_from && db.valid()) {
    const std::vector<std::uint32_t> regions = db.polygons_in(geo::layer::regions, b);
    stroke_borders(db, geo::layer::regions, regions, pr, land, region, 0.35 * scale, 1.0f);
  }
  stroke_borders(db, geo::layer::countries, countries, pr, land, border, 0.6 * scale, 1.0f);

  const rgb water = unpack(p.water);
  const rgb land_c = unpack(p.land);
  const rgb home_c = unpack(p.land_home);
  const rgb region_c = unpack(p.region);
  const rgb border_c = unpack(p.border);
  const rgb frame_c = unpack(p.frame);
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
  k.zoom = std::clamp(v.zoom, 0, k_zoom_steps - 1);
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
  if (!render_map(db, v, p, home_country, img->bgra)) return nullptr;
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
