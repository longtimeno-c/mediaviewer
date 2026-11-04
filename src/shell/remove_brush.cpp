// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/remove_brush.h"

#include <algorithm>
#include <cmath>

namespace mv::shell {

namespace {
// Dabs this far apart (in radii) draw a smooth stroke.
constexpr float kSpacing = 0.35f;
}  // namespace

void remove_brush::set_frame(std::uint32_t width, std::uint32_t height) noexcept {
  const float l = static_cast<float>(std::max({width, height, 1u}));
  aspect_w_ = static_cast<float>(std::max(width, 1u)) / l;
  aspect_h_ = static_cast<float>(std::max(height, 1u)) / l;
}

bool remove_brush::add(float u, float v, float r) {
  if (full()) return false;
  dabs_.push_back({u, v, r, static_cast<std::uint8_t>(erasing_ ? 1 : 0)});
  return true;
}

bool remove_brush::begin(float u, float v, float r, bool erase) {
  if (full() || !(r > 0.0f)) return false;
  undone_.clear();  // a new stroke ends the redo chain
  starts_.push_back(dabs_.size());
  stroking_ = true;
  erasing_ = erase;
  return add(u, v, r);
}

bool remove_brush::drag(float u, float v) {
  if (!stroking_ || dabs_.empty()) return false;
  const brush_dab last = dabs_.back();
  // Distance in the long side's units, so a stroke is as dense across as down.
  const float dx = (u - last.u) * aspect_w_, dy = (v - last.v) * aspect_h_;
  const float step = last.r * kSpacing;
  const int n = static_cast<int>(std::floor(std::sqrt(dx * dx + dy * dy) / step));
  for (int i = 1; i <= n; ++i) {
    const float t = static_cast<float>(i) * step / std::sqrt(dx * dx + dy * dy);
    if (!add(last.u + (u - last.u) * t, last.v + (v - last.v) * t, last.r)) return false;
  }
  return true;
}

bool remove_brush::undo_stroke() {
  if (starts_.empty()) return false;
  const auto first = dabs_.begin() + static_cast<std::ptrdiff_t>(starts_.back());
  undone_.emplace_back(first, dabs_.end());
  dabs_.erase(first, dabs_.end());
  starts_.pop_back();
  stroking_ = false;
  return true;
}

bool remove_brush::redo_stroke() {
  if (undone_.empty() || stroking_) return false;
  std::vector<brush_dab> back = std::move(undone_.back());
  undone_.pop_back();
  if (back.empty() || dabs_.size() + back.size() > kBrushDabs) return false;
  starts_.push_back(dabs_.size());
  dabs_.insert(dabs_.end(), back.begin(), back.end());
  return true;
}

void remove_brush::clear() noexcept {
  dabs_.clear();
  starts_.clear();
  undone_.clear();
  stroking_ = false;
}

std::vector<float> remove_brush::handoff() const {
  std::vector<float> flat;
  flat.reserve(dabs_.size() * 3);
  for (std::size_t i = 0; i < dabs_.size(); ++i) {
    const brush_dab& d = dabs_[i];
    if (d.erase) continue;
    bool erased = false;
    for (std::size_t k = i + 1; k < dabs_.size() && !erased; ++k) {
      const brush_dab& e = dabs_[k];
      if (!e.erase) continue;
      const float dx = (d.u - e.u) * aspect_w_, dy = (d.v - e.v) * aspect_h_;
      erased = dx * dx + dy * dy <= e.r * e.r;
    }
    if (!erased) flat.insert(flat.end(), {d.u, d.v, d.r});
  }
  return flat;
}

std::vector<std::uint8_t> remove_brush::mask(std::uint32_t width, std::uint32_t height) const {
  std::vector<std::uint8_t> out(std::size_t{width} * height, 0);
  if (width == 0 || height == 0) return out;
  const float l = static_cast<float>(std::max(width, height));
  for (const brush_dab& d : dabs_) {
    const float cx = d.u * static_cast<float>(width), cy = d.v * static_cast<float>(height), r = d.r * l;
    const auto lo = [](float v) { return static_cast<std::int64_t>(std::floor(v)); };
    const std::int64_t x0 = std::max<std::int64_t>(0, lo(cx - r)), x1 = std::min<std::int64_t>(width - 1, lo(cx + r));
    const std::int64_t y0 = std::max<std::int64_t>(0, lo(cy - r)), y1 = std::min<std::int64_t>(height - 1, lo(cy + r));
    for (std::int64_t y = y0; y <= y1; ++y) {
      for (std::int64_t x = x0; x <= x1; ++x) {
        const float px = static_cast<float>(x) + 0.5f - cx, py = static_cast<float>(y) + 0.5f - cy;
        if (px * px + py * py <= r * r) {
          out[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)] = d.erase ? 0 : 255;
        }
      }
    }
  }
  return out;
}

}  // namespace mv::shell
