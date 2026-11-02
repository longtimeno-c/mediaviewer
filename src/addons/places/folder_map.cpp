// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The folder map's dot layer (folder_map.h). See docs/design/27 "The folder map".
#include "addons/places/folder_map.h"

#include <algorithm>
#include <cmath>

namespace mv::places {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr double kMaxMercLat = 85.0;  // as map.cpp's projection

double merc(double lat_deg) noexcept {
  const double phi = std::clamp(lat_deg, -kMaxMercLat, kMaxMercLat) * kDeg;
  return std::log(std::tan(kPi / 4 + phi / 2));
}

double wrap180(double d) noexcept {
  if (d >= -180 && d < 180) return d;
  if (!std::isfinite(d)) return 0;
  d = std::fmod(d + 180, 360);
  if (d < 0) d += 360;
  return d - 180;
}

// map_pixel's projection, built once per call rather than once per item. The
// tests check it lands where map_pixel does.
struct projection {
  double cx = 0, cy = 0, k = 1, lon0 = 0, m0 = 0;
  explicit projection(const map_view& v) noexcept {
    const double lat0 = std::clamp(v.lat, -kMaxMercLat, kMaxMercLat);
    cx = v.width / 2.0;
    cy = v.height / 2.0;
    lon0 = wrap180(v.lon);
    m0 = merc(lat0);
    k = v.height * std::cos(lat0 * kDeg) / (clamp_span(v.span_deg) * kDeg);
  }
  void pixel(double lat, double lon, double& x, double& y) const noexcept {
    x = cx + wrap180(lon - lon0) * kDeg * k;
    y = cy - (merc(lat) - m0) * k;
  }
};

float backing(const map_view& v) noexcept { return v.scale > 0 ? std::min(v.scale, 8.0f) : 1.0f; }

double radius_of(std::uint32_t count, double scale) noexcept {
  const double pt = k_folder_dot_pt + 1.5 * std::log2(static_cast<double>(std::max<std::uint32_t>(count, 1)));
  return std::min(pt, k_folder_dot_max_pt) * scale;
}

struct rgb {
  float r, g, b;
};

rgb unpack(std::uint32_t c) noexcept {
  return {static_cast<float>((c >> 16) & 0xff), static_cast<float>((c >> 8) & 0xff), static_cast<float>(c & 0xff)};
}

std::uint32_t mix(std::uint32_t a, std::uint32_t b, float t) noexcept {
  const rgb x = unpack(a);
  const rgb y = unpack(b);
  const auto q = [](float v) { return static_cast<std::uint32_t>(std::clamp(v + 0.5f, 0.0f, 255.0f)); };
  return (q(x.r + (y.r - x.r) * t) << 16) | (q(x.g + (y.g - x.g) * t) << 8) | q(x.b + (y.b - x.b) * t);
}

// A disc of radius `r` at (x, y) in colour `c`, over premultiplied BGRA.
void disc(const map_view& v, std::span<std::uint8_t> bgra, double x, double y, double r, rgb c) noexcept {
  const int w = static_cast<int>(v.width);
  const int h = static_cast<int>(v.height);
  const int y0 = std::max(0, static_cast<int>(std::floor(y - r - 1)));
  const int y1 = std::min(h - 1, static_cast<int>(std::ceil(y + r + 1)));
  const int x0 = std::max(0, static_cast<int>(std::floor(x - r - 1)));
  const int x1 = std::min(w - 1, static_cast<int>(std::ceil(x + r + 1)));
  for (int py = y0; py <= y1; ++py) {
    std::uint8_t* row = bgra.data() + static_cast<std::size_t>(py) * v.width * 4;
    const double dy = py + 0.5 - y;
    for (int px = x0; px <= x1; ++px) {
      const double dx = px + 0.5 - x;
      const float cov = static_cast<float>(std::clamp(r + 0.5 - std::sqrt(dx * dx + dy * dy), 0.0, 1.0));
      if (cov <= 0) continue;
      std::uint8_t* p = row + 4 * px;
      // The picture's own alpha (its rounded corners) scales the dot too.
      const float a = p[3] / 255.0f;
      const auto over = [cov, a](std::uint8_t d, float s) {
        return static_cast<std::uint8_t>(std::clamp(d + (s * a - d) * cov + 0.5f, 0.0f, 255.0f));
      };
      p[0] = over(p[0], c.b);
      p[1] = over(p[1], c.g);
      p[2] = over(p[2], c.r);
    }
  }
}

}  // namespace

dot_palette dot_palette_for(const theme_tokens& t) noexcept {
  const map_palette m = palette_for(t);
  dot_palette p;
  p.fill = mix(t.accent >> 8, t.title >> 8, t.dark ? 0.30f : 0.40f);
  p.ring = m.land_home;
  return p;
}

void cluster_dots(const map_view& v, std::span<const folder_item> items, std::size_t skip, std::vector<map_dot>& out) {
  out.clear();
  if (v.width == 0 || v.height == 0 || v.width > 4096 || v.height > 4096 || !std::isfinite(v.lat) ||
      !std::isfinite(v.lon)) {
    return;
  }
  const double scale = backing(v);
  const projection pr(v);
  const double cell = k_folder_cell_pt * scale;
  const double margin = k_folder_dot_max_pt * scale;
  // The grid is anchored to (0, 0) degrees, so a pan moves the cells with the
  // land and the clusters keep their members.
  const double ox = pr.cx - pr.lon0 * kDeg * pr.k;
  const double oy = pr.cy + pr.m0 * pr.k;
  const double c0 = std::floor((-margin - ox) / cell);
  const double r0 = std::floor((-margin - oy) / cell);
  const auto cols = static_cast<std::size_t>(std::floor((v.width + margin - ox) / cell) - c0 + 1);
  const auto rows = static_cast<std::size_t>(std::floor((v.height + margin - oy) / cell) - r0 + 1);
  struct acc {
    double sx = 0, sy = 0;
    std::uint32_t count = 0;
    std::uint32_t first = 0;
  };
  std::vector<acc> grid(cols * rows);
  const std::size_t n = std::min(items.size(), k_max_folder_items);
  for (std::size_t i = 0; i < n; ++i) {
    if (i == skip) continue;
    const folder_item& it = items[i];
    if (!std::isfinite(it.lat) || !std::isfinite(it.lon)) continue;
    double x = 0, y = 0;
    pr.pixel(it.lat, it.lon, x, y);
    if (x < -margin || y < -margin || x >= v.width + margin || y >= v.height + margin) continue;
    const double gc = std::floor((x - ox) / cell) - c0;
    const double gr = std::floor((y - oy) / cell) - r0;
    if (gc < 0 || gr < 0 || gc >= static_cast<double>(cols) || gr >= static_cast<double>(rows)) continue;
    acc& a = grid[static_cast<std::size_t>(gr) * cols + static_cast<std::size_t>(gc)];
    if (a.count == 0) a.first = static_cast<std::uint32_t>(i);  // items arrive in folder order
    a.sx += x;
    a.sy += y;
    ++a.count;
  }
  for (const acc& a : grid) {
    if (a.count == 0) continue;
    map_dot d;
    d.x = a.sx / a.count;
    d.y = a.sy / a.count;
    d.count = a.count;
    d.first = a.first;
    d.r = radius_of(a.count, scale);
    out.push_back(d);
  }
  // Neighbouring cells can put two dots on top of each other: merge them,
  // the bigger taking the smaller, once.
  std::sort(out.begin(), out.end(), [](const map_dot& a, const map_dot& b) {
    return a.count != b.count ? a.count > b.count : a.first < b.first;
  });
  const double gap = 1.5 * scale;
  std::vector<std::uint8_t> gone(out.size(), 0);
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (gone[i]) continue;
    for (std::size_t j = i + 1; j < out.size(); ++j) {
      if (gone[j]) continue;
      map_dot& a = out[i];
      const map_dot& b = out[j];
      const double reach = a.r + b.r + gap;
      const double dx = a.x - b.x, dy = a.y - b.y;
      if (dx * dx + dy * dy >= reach * reach) continue;
      const double total = static_cast<double>(a.count) + b.count;
      a.x = (a.x * a.count + b.x * b.count) / total;
      a.y = (a.y * a.count + b.y * b.count) / total;
      a.count += b.count;
      a.first = std::min(a.first, b.first);
      a.r = radius_of(a.count, scale);
      gone[j] = 1;
    }
  }
  std::size_t kept = 0;
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (!gone[i]) out[kept++] = out[i];
  }
  out.resize(kept);
  std::stable_sort(out.begin(), out.end(), [](const map_dot& a, const map_dot& b) { return a.count > b.count; });
}

bool draw_dots(const map_view& v, const dot_palette& p, std::span<const map_dot> dots, std::span<std::uint8_t> bgra) {
  if (v.width == 0 || v.height == 0 || v.width > 4096 || v.height > 4096 ||
      bgra.size() < static_cast<std::size_t>(v.width) * v.height * 4) {
    return false;
  }
  const double scale = backing(v);
  const rgb fill = unpack(p.fill);
  const rgb ring = unpack(p.ring);
  // Smallest first, so a big cluster sits on top of the lone items near it.
  for (std::size_t i = dots.size(); i-- > 0;) {
    const map_dot& d = dots[i];
    if (!std::isfinite(d.x) || !std::isfinite(d.y) || !(d.r > 0) || d.r > 64 * scale) continue;
    disc(v, bgra, d.x, d.y, d.r + 1.2 * scale, ring);
    disc(v, bgra, d.x, d.y, d.r, fill);
    // A cluster has a light centre: one item reads as a dot, many as a ring.
    if (d.count > 1) disc(v, bgra, d.x, d.y, d.r * 0.36, ring);
  }
  return true;
}

const map_dot* dot_at(std::span<const map_dot> dots, const map_view& v, double x, double y) noexcept {
  if (!std::isfinite(x) || !std::isfinite(y)) return nullptr;
  const double slop = k_folder_hit_slop_pt * backing(v);
  const map_dot* best = nullptr;
  double best_d = 0;
  for (const map_dot& d : dots) {
    const double dx = d.x - x, dy = d.y - y;
    const double dist = std::sqrt(dx * dx + dy * dy);
    if (dist > d.r + slop) continue;
    // Nearest edge wins (inside a dot is negative), so a lone dot beside a
    // big cluster can still be hit.
    const double edge = dist - d.r;
    if (!best || edge < best_d) {
      best = &d;
      best_d = edge;
    }
  }
  return best;
}

std::string_view folder_of(std::string_view path) noexcept {
  const auto cut = path.find_last_of("/\\");
  // A file at the root of a volume has no folder layer (the host's open_item
  // takes the same view).
  return cut == std::string_view::npos ? std::string_view{} : path.substr(0, cut);
}

bool same_path(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const char x = a[i] == '\\' ? '/' : a[i];
    const char y = b[i] == '\\' ? '/' : b[i];
    if (x != y) return false;
  }
  return true;
}

}  // namespace mv::places
