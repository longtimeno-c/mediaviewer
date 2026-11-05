// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/remove_brush.h"

#include <algorithm>
#include <cmath>

namespace mv::shell {

void remove_brush::set_frame(std::uint32_t width, std::uint32_t height) noexcept {
  const float l = static_cast<float>(std::max({width, height, 1u}));
  aspect_w_ = static_cast<float>(std::max(width, 1u)) / l;
  aspect_h_ = static_cast<float>(std::max(height, 1u)) / l;
}

bool remove_brush::add(float u, float v, float r) {
  if (dabs_.size() >= kBrushStackDabs) return false;
  dabs_.push_back({u, v, r, static_cast<std::uint8_t>(erasing_ ? 1 : 0)});
  return true;
}

bool remove_brush::begin(float u, float v, float r, bool erase) {
  if (full() || !(r > 0.0f) || !std::isfinite(r)) return false;
  redo_.clear();  // a new stroke ends the redo chain
  starts_.push_back(dabs_.size());
  done_.push_back({});
  stroking_ = true;
  erasing_ = erase;
  return add(u, v, r);
}

bool remove_brush::drag(float u, float v) {
  if (!stroking_ || dabs_.empty()) return false;
  const brush_dab last = dabs_.back();
  // Distance in the long side's units, so a stroke is as dense across as down.
  const float dx = (u - last.u) * aspect_w_, dy = (v - last.v) * aspect_h_;
  const float step = last.r * kBrushSpacing;
  const float dist = std::sqrt(dx * dx + dy * dy);
  if (!(step > 0.0f) || !std::isfinite(dist)) return false;
  const int n = static_cast<int>(std::floor(dist / step));
  for (int i = 1; i <= n; ++i) {
    const float t = static_cast<float>(i) * step / dist;
    if (!add(last.u + (u - last.u) * t, last.v + (v - last.v) * t, last.r)) return false;
  }
  return dabs_.size() < kBrushStackDabs || n == 0;
}

bool remove_brush::undo_stroke() {
  if (done_.empty()) return false;
  step s = std::move(done_.back());
  done_.pop_back();
  stroking_ = false;
  if (s.clear) {
    // The strokes the Clear took off come back (strokes after it were undone
    // first: the steps are a stack).
    dabs_ = std::move(s.dabs);
    starts_ = std::move(s.starts);
    redo_.push_back({.clear = true, .dabs = {}, .starts = {}});
  } else {
    if (starts_.empty()) return false;
    const auto first = dabs_.begin() + static_cast<std::ptrdiff_t>(starts_.back());
    redo_.push_back({.clear = false, .dabs = std::vector<brush_dab>(first, dabs_.end()), .starts = {}});
    dabs_.erase(first, dabs_.end());
    starts_.pop_back();
  }
  ++generation_;
  return true;
}

bool remove_brush::redo_stroke() {
  if (redo_.empty() || stroking_) return false;
  step& s = redo_.back();
  if (s.clear) {
    if (dabs_.empty()) {
      redo_.pop_back();
      return false;
    }
    done_.push_back({.clear = true, .dabs = std::move(dabs_), .starts = std::move(starts_)});
    dabs_.clear();
    starts_.clear();
    redo_.pop_back();
    ++generation_;
    return true;
  }
  if (s.dabs.empty() || starts_.size() >= kBrushStrokes || dabs_.size() + s.dabs.size() > kBrushStackDabs) {
    return false;
  }
  starts_.push_back(dabs_.size());
  dabs_.insert(dabs_.end(), s.dabs.begin(), s.dabs.end());
  done_.push_back({});
  redo_.pop_back();
  return true;
}

bool remove_brush::clear() {
  stroking_ = false;
  if (dabs_.empty()) return false;
  done_.push_back({.clear = true, .dabs = std::move(dabs_), .starts = std::move(starts_)});
  dabs_.clear();
  starts_.clear();
  redo_.clear();
  ++generation_;
  // Keep the newest kBrushClears Clears undoable: past that, the oldest Clear
  // and every step under it are forgotten (the stack stays consistent: the
  // steps under a Clear are the strokes it holds).
  std::size_t clears = 0;
  for (const step& s : done_) clears += s.clear ? 1 : 0;
  if (clears > kBrushClears) {
    auto oldest = std::find_if(done_.begin(), done_.end(), [](const step& s) { return s.clear; });
    done_.erase(done_.begin(), oldest + 1);
  }
  return true;
}

void remove_brush::reset() noexcept {
  dabs_.clear();
  starts_.clear();
  done_.clear();
  redo_.clear();
  stroking_ = false;
  ++generation_;
}

std::vector<float> remove_brush::handoff() const {
  std::vector<float> flat;
  flat.reserve(dabs_.size() * 3);
  // The erase dabs once (a painted dab tests only those after it), so a full
  // brushing without erasing is one pass, not 8192² on the UI thread.
  std::vector<std::size_t> erases;
  for (std::size_t k = 0; k < dabs_.size(); ++k) {
    if (dabs_[k].erase) erases.push_back(k);
  }
  for (std::size_t i = 0; i < dabs_.size(); ++i) {
    const brush_dab& d = dabs_[i];
    if (d.erase) continue;
    bool erased = false;
    for (auto it = std::upper_bound(erases.begin(), erases.end(), i); it != erases.end() && !erased; ++it) {
      const brush_dab& e = dabs_[*it];
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
