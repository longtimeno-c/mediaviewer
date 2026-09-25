// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit/edit_stack.h"

#include <algorithm>
#include <cmath>

namespace mv::edit {
namespace {

constexpr float kMinCrop = 1e-4f;
constexpr double kPi = 3.14159265358979323846;

codec::d4 op_d4(op_kind k) noexcept {
  switch (k) {
    case op_kind::rotate_cw: return codec::kRotateCw;
    case op_kind::rotate_ccw: return codec::kRotateCcw;
    case op_kind::flip_h: return codec::kFlipH;
    case op_kind::flip_v: return codec::kFlipV;
    default: return codec::d4{};
  }
}

bool is_d4(op_kind k) noexcept {
  return k == op_kind::rotate_cw || k == op_kind::rotate_ccw || k == op_kind::flip_h ||
         k == op_kind::flip_v;
}

rect clamp_rect(rect r) noexcept {
  if (!(r.w > kMinCrop)) r.w = kMinCrop;  // also catches NaN
  if (!(r.h > kMinCrop)) r.h = kMinCrop;
  r.w = std::min(r.w, 1.0f);
  r.h = std::min(r.h, 1.0f);
  if (!(r.x >= 0.0f)) r.x = 0.0f;
  if (!(r.y >= 0.0f)) r.y = 0.0f;
  r.x = std::min(r.x, 1.0f - r.w);
  r.y = std::min(r.y, 1.0f - r.h);
  return r;
}

// The crop rect, seen after `h` is applied to the frame it lives in.
rect map_rect(rect r, codec::d4 h) noexcept {
  const codec::d4 inv = codec::inverse(h);
  float min_x = 2, min_y = 2, max_x = -2, max_y = -2;
  const float xs[2] = {r.x, r.x + r.w};
  const float ys[2] = {r.y, r.y + r.h};
  for (float px : xs) {
    for (float py : ys) {
      const float cx = 2 * px - 1, cy = 2 * py - 1;
      const float nx = inv.a * cx + inv.b * cy;
      const float ny = inv.c * cx + inv.d * cy;
      min_x = std::min(min_x, nx);
      max_x = std::max(max_x, nx);
      min_y = std::min(min_y, ny);
      max_y = std::max(max_y, ny);
    }
  }
  return rect{(min_x + 1) / 2, (min_y + 1) / 2, (max_x - min_x) / 2, (max_y - min_y) / 2};
}

struct rotation {
  double c = 1, s = 0;
  explicit rotation(float degrees) noexcept
      : c(std::cos(degrees * kPi / 180.0)), s(std::sin(degrees * kPi / 180.0)) {}
};

// Is the frame point (normalised, 0..1) inside the source once the content
// is rotated by the straighten angle?
bool inside(const rotation& rot, double fw, double fh, double u, double v) noexcept {
  const double px = (u - 0.5) * fw, py = (v - 0.5) * fh;
  const double qx = rot.c * px + rot.s * py;
  const double qy = -rot.s * px + rot.c * py;
  constexpr double kSlack = 1e-6;
  return std::abs(qx) <= fw * (0.5 + kSlack) && std::abs(qy) <= fh * (0.5 + kSlack);
}

bool corners_inside(const rotation& rot, double fw, double fh, const rect& r) noexcept {
  return inside(rot, fw, fh, r.x, r.y) && inside(rot, fw, fh, r.x + r.w, r.y) &&
         inside(rot, fw, fh, r.x, r.y + r.h) && inside(rot, fw, fh, r.x + r.w, r.y + r.h);
}

std::uint32_t round_px(double v) noexcept {
  if (!(v >= 1.0)) return 1;
  if (v > 4294967295.0) return 0xFFFFFFFFu;
  return static_cast<std::uint32_t>(std::lround(v));
}

}  // namespace

geometry fold(std::span<const op> ops) noexcept {
  geometry g;
  for (const op& o : ops) {
    if (is_d4(o.kind)) {
      const codec::d4 h = op_d4(o.kind);
      g.orient = codec::compose(g.orient, h);
      g.crop = map_rect(g.crop, h);
      // A mirror reverses the sense of rotation; a quarter turn does not.
      if (h.a * h.d - h.b * h.c < 0) g.straighten = -g.straighten;
      continue;
    }
    switch (o.kind) {
      case op_kind::crop:
        g.crop = clamp_rect(o.crop);
        break;
      case op_kind::straighten:
        g.straighten = std::isfinite(o.degrees)
                           ? std::clamp(o.degrees, -kMaxStraighten, kMaxStraighten)
                           : 0.0f;
        break;
      case op_kind::resize:
        g.resize = o.resize;
        break;
      default:
        break;
    }
  }
  if (g.straighten == -0.0f) g.straighten = 0.0f;
  return g;
}

rect constrain_crop(rect r, float straighten, size2 frame) noexcept {
  r = clamp_rect(r);
  if (straighten == 0.0f || frame.w == 0 || frame.h == 0) return r;
  const rotation rot(straighten);
  const double fw = frame.w, fh = frame.h;
  if (corners_inside(rot, fw, fh, r)) return r;

  const float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
  const bool centre_inside = inside(rot, fw, fh, cx, cy);
  auto at = [&](double s) {
    const double ox = centre_inside ? cx : 0.5 + (cx - 0.5) * s;
    const double oy = centre_inside ? cy : 0.5 + (cy - 0.5) * s;
    const double w = r.w * s, h = r.h * s;
    return rect{static_cast<float>(ox - w / 2), static_cast<float>(oy - h / 2),
                static_cast<float>(w), static_cast<float>(h)};
  };
  double lo = 0.0, hi = 1.0;
  for (int i = 0; i < 40; ++i) {
    const double mid = (lo + hi) / 2;
    if (corners_inside(rot, fw, fh, at(mid))) lo = mid; else hi = mid;
  }
  rect out = at(lo);
  if (out.w < kMinCrop || out.h < kMinCrop) out = clamp_rect(out);
  return out;
}

affine invert(const affine& a) noexcept {
  const double m0 = a.m[0], m1 = a.m[1], m2 = a.m[2], m3 = a.m[3], m4 = a.m[4], m5 = a.m[5];
  const double det = m0 * m4 - m1 * m3;
  if (!(std::abs(det) > 1e-12)) return affine{};
  affine r;
  const double i0 = m4 / det, i1 = -m1 / det, i3 = -m3 / det, i4 = m0 / det;
  r.m[0] = static_cast<float>(i0);
  r.m[1] = static_cast<float>(i1);
  r.m[2] = static_cast<float>(-(i0 * m2 + i1 * m5));
  r.m[3] = static_cast<float>(i3);
  r.m[4] = static_cast<float>(i4);
  r.m[5] = static_cast<float>(-(i3 * m2 + i4 * m5));
  return r;
}

rect auto_crop(float straighten, size2 frame) noexcept {
  return constrain_crop(rect{}, straighten, frame);
}

placement place(const geometry& g, size2 source, codec::d4 base, bool keep_frame) noexcept {
  placement p;
  p.total = codec::compose(base, g.orient);
  p.oriented = p.total.transposes() ? size2{source.h, source.w} : source;
  const double ow = p.oriented.w, oh = p.oriented.h;

  const rect crop = keep_frame ? clamp_rect(g.crop) : constrain_crop(g.crop, g.straighten, p.oriented);
  p.cropped = size2{round_px(crop.w * ow), round_px(crop.h * oh)};
  p.cropped.w = std::min(p.cropped.w, std::max(p.oriented.w, 1u));
  p.cropped.h = std::min(p.cropped.h, std::max(p.oriented.h, 1u));
  p.crop_x = static_cast<std::uint32_t>(std::lround(std::max(0.0, crop.x * ow)));
  p.crop_y = static_cast<std::uint32_t>(std::lround(std::max(0.0, crop.y * oh)));

  const double cw = p.cropped.w, ch = p.cropped.h;
  switch (g.resize.mode) {
    case resize_mode::none:
      p.output = p.cropped;
      break;
    case resize_mode::long_edge: {
      if (g.resize.a == 0) {
        p.output = p.cropped;
        break;
      }
      const double scale = g.resize.a / std::max(cw, ch);
      p.output = size2{round_px(cw * scale), round_px(ch * scale)};
      break;
    }
    case resize_mode::exact:
      p.output = (g.resize.a == 0 || g.resize.b == 0) ? p.cropped
                                                       : size2{g.resize.a, g.resize.b};
      break;
    case resize_mode::percent: {
      const double pct = std::isfinite(g.resize.percent)
                             ? std::clamp(static_cast<double>(g.resize.percent), 1.0, 1000.0)
                             : 100.0;
      p.output = size2{round_px(cw * pct / 100.0), round_px(ch * pct / 100.0)};
      break;
    }
  }

  p.exact_copy = g.straighten == 0.0f && p.output == p.cropped;
  if (p.exact_copy) {
    p.crop_x = std::min(p.crop_x, p.oriented.w - p.cropped.w);
    p.crop_y = std::min(p.crop_y, p.oriented.h - p.cropped.h);
  }

  // Output uv → source uv, evaluated at three points: it is affine.
  const rotation rot(g.straighten);
  const codec::d4 t = p.total;
  auto map = [&](double u, double v, double& su, double& sv) {
    const double px = (crop.x + u * crop.w - 0.5) * ow;
    const double py = (crop.y + v * crop.h - 0.5) * oh;
    const double qx = rot.c * px + rot.s * py;
    const double qy = -rot.s * px + rot.c * py;
    const double nx = ow > 0 ? 2 * qx / ow : 0, ny = oh > 0 ? 2 * qy / oh : 0;
    su = (t.a * nx + t.b * ny + 1) / 2;
    sv = (t.c * nx + t.d * ny + 1) / 2;
  };
  double u0, v0, ux, vx, uy, vy;
  map(0, 0, u0, v0);
  map(1, 0, ux, vx);
  map(0, 1, uy, vy);
  auto clean = [](double v) {
    // Keep exact 0 / ±1 exact so an unedited map compares as the identity.
    const double r = std::round(v);
    return static_cast<float>(std::abs(v - r) < 1e-9 ? r : v);
  };
  p.map.m[0] = clean(ux - u0);
  p.map.m[1] = clean(uy - u0);
  p.map.m[2] = clean(u0);
  p.map.m[3] = clean(vx - v0);
  p.map.m[4] = clean(vy - v0);
  p.map.m[5] = clean(v0);
  return p;
}

}  // namespace mv::edit
