// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/af.h"

#include <algorithm>

namespace mv::meta {
namespace {

bool bit_set(std::span<const std::int64_t> mask, std::size_t i) noexcept {
  const std::size_t word = i / 16;
  if (word >= mask.size()) return false;
  return (mask[word] >> (i % 16)) & 1;
}

float clamp01(double v) noexcept { return static_cast<float>(std::clamp(v, 0.0, 1.0)); }

// A point whose box leaves the frame is clipped to it; one entirely outside is
// dropped by the caller (w or h becomes 0).
bool push_box(std::vector<af_point>& out, double cx, double cy, double w, double h,
              std::uint32_t grid_w, std::uint32_t grid_h, bool in_focus) noexcept {
  if (grid_w == 0 || grid_h == 0) return false;
  const double x0 = (cx - w / 2) / grid_w, x1 = (cx + w / 2) / grid_w;
  const double y0 = (cy - h / 2) / grid_h, y1 = (cy + h / 2) / grid_h;
  if (x1 <= 0 || y1 <= 0 || x0 >= 1 || y0 >= 1) return false;
  af_point p;
  p.x = clamp01(x0);
  p.y = clamp01(y0);
  p.w = clamp01(x1) - p.x;
  p.h = clamp01(y1) - p.y;
  p.in_focus = in_focus;
  if (p.w <= 0 || p.h <= 0) return false;
  out.push_back(p);
  return true;
}

}  // namespace

std::vector<af_point> canon_af_points(std::uint32_t image_w, std::uint32_t image_h,
                                      std::span<const std::int64_t> widths,
                                      std::span<const std::int64_t> heights,
                                      std::span<const std::int64_t> xs,
                                      std::span<const std::int64_t> ys,
                                      std::span<const std::int64_t> in_focus_mask,
                                      std::span<const std::int64_t> selected_mask) noexcept {
  std::vector<af_point> out;
  const std::size_t n = std::min({widths.size(), heights.size(), xs.size(), ys.size()});
  for (std::size_t i = 0; i < n; ++i) {
    const bool focus = bit_set(in_focus_mask, i);
    const bool selected = bit_set(selected_mask, i);
    if (!focus && !selected) continue;
    push_box(out, image_w / 2.0 + static_cast<double>(xs[i]),
             image_h / 2.0 + static_cast<double>(ys[i]), static_cast<double>(widths[i]),
             static_cast<double>(heights[i]), image_w, image_h, focus);
  }
  return out;
}

std::vector<af_point> centre_af_point(double cx, double cy, double w, double h,
                                      std::uint32_t grid_w, std::uint32_t grid_h,
                                      bool in_focus) noexcept {
  std::vector<af_point> out;
  if (w <= 0) w = grid_w * 0.04;
  if (h <= 0) h = w;
  push_box(out, cx, cy, w, h, grid_w, grid_h, in_focus);
  return out;
}

}  // namespace mv::meta
