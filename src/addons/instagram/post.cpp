// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/instagram/post.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace mv::instagram {
namespace {

constexpr std::array<shape_info, 5> kShapes{{
    {shape::square, "1:1", "1:1", 1080, 1080},
    {shape::portrait, "4:5", "4:5", 1080, 1350},
    {shape::tall, "3:4", "3:4", 1080, 1440},
    {shape::widescreen, "16:9", "16:9", 1080, 608},
    {shape::wide, "1.91:1", "1.91:1", 1080, 566},
}};

char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool ends_with_any(std::string_view name, std::span<const std::string_view> exts) noexcept {
  for (const std::string_view ext : exts) {
    if (name.size() < ext.size()) continue;
    const std::string_view tail = name.substr(name.size() - ext.size());
    if (std::equal(tail.begin(), tail.end(), ext.begin(), [](char a, char b) { return lower(a) == b; })) return true;
  }
  return false;
}

constexpr std::array<std::string_view, 9> kClips{".mp4", ".mov", ".m4v", ".mkv", ".webm", ".avi", ".ts", ".mts", ".m2ts"};
constexpr std::array<std::string_view, 20> kStills{".jpg", ".jpeg", ".png", ".bmp", ".gif", ".tif", ".tiff",
                                                   ".webp", ".heic", ".heif", ".avif", ".dng", ".cr2", ".cr3",
                                                   ".nef", ".arw", ".raf", ".orf", ".rw2", ".pef"};

}  // namespace

bool adjusted(const adjustments& a) noexcept {
  return std::any_of(a.begin(), a.end(), [](float v) { return v != 0.0f; });
}

std::span<const slider_info> sliders() noexcept {
  static constexpr std::array<slider_info, 12> kSliders{{
      {0, "Exposure", -100, 100, 1, 0, 0.03f},
      {1, "Contrast", -100, 100, 1, 0, 1.0f},
      {5, "Highlights", -100, 100, 1, 0, 1.0f},
      {6, "Shadows", -100, 100, 1, 0, 1.0f},
      {7, "Whites", -100, 100, 1, 0, 1.0f},
      {8, "Blacks", -100, 100, 1, 0, 1.0f},
      {2, "Saturation", -100, 100, 1, 1, 1.0f},
      {9, "Vibrance", -100, 100, 1, 1, 1.0f},
      {3, "Temperature", -100, 100, 1, 1, 1.0f},
      {4, "Tint", -100, 100, 1, 1, 1.0f},
      {10, "Black & white", 0, 100, 1, 2, 1.0f},
      {13, "Vignette", -100, 100, 1, 2, 1.0f},
  }};
  return kSliders;
}

const shape_info& info_of(shape s) noexcept { return kShapes[static_cast<std::size_t>(s)]; }
std::span<const shape_info> shapes() noexcept { return kShapes; }

std::optional<shape> shape_from(std::string_view id) noexcept {
  for (const shape_info& i : kShapes) {
    if (i.id == id) return i.s;
  }
  return std::nullopt;
}

double aspect_of(shape s) noexcept {
  // The named ratios, not the pixel sizes: 1080 x 566 only rounds 1.91.
  switch (s) {
    case shape::square: return 1.0;
    case shape::portrait: return 4.0 / 5.0;
    case shape::tall: return 3.0 / 4.0;
    case shape::widescreen: return 16.0 / 9.0;
    case shape::wide: return 1.91;
  }
  return 1.0;
}

tile_size tile_of(shape s) noexcept {
  switch (s) {
    case shape::square: return {196, 196};
    case shape::portrait: return {157, 196};
    case shape::tall: return {147, 196};
    case shape::widescreen: return {258, 145};
    case shape::wide: return {258, 135};
  }
  return {196, 196};
}

rect crop_of(std::uint32_t w, std::uint32_t h, double aspect, const frame& f) noexcept {
  if (w == 0 || h == 0 || !(aspect > 0)) return {};
  const double picture = static_cast<double>(w) / h;
  double rw = 1, rh = 1;
  if (picture > aspect) {
    rw = aspect / picture;  // the picture is wider: full height
  } else {
    rh = picture / aspect;
  }
  const double zoom = std::clamp(f.zoom, 1.0, kMaxZoom);
  rw /= zoom;
  rh /= zoom;
  rect r;
  r.w = rw;
  r.h = rh;
  r.x = std::clamp(f.cx - rw / 2, 0.0, 1.0 - rw);
  r.y = std::clamp(f.cy - rh / 2, 0.0, 1.0 - rh);
  return r;
}

frame dragged(const frame& f, std::uint32_t w, std::uint32_t h, double aspect, double dx, double dy) noexcept {
  const rect r = crop_of(w, h, aspect, f);
  frame out = f;
  // From the crop as it shows, so a drag past an edge is not banked.
  out.cx = std::clamp(r.x + r.w / 2 - dx * r.w, 0.0, 1.0);
  out.cy = std::clamp(r.y + r.h / 2 - dy * r.h, 0.0, 1.0);
  return out;
}

frame zoomed(const frame& f, double scale) noexcept {
  frame out = f;
  out.zoom = std::clamp(f.zoom * (scale > 0 ? scale : 1.0), 1.0, kMaxZoom);
  return out;
}

void post::set_focus(std::uint64_t uid) noexcept {
  focus_ = find(uid) ? uid : 0;
}

chosen* post::find(std::uint64_t uid) noexcept {
  for (chosen& c : items_) {
    if (c.uid == uid) return &c;
  }
  return nullptr;
}

std::size_t post::index_of(std::uint64_t uid) const noexcept {
  for (std::size_t i = 0; i < items_.size(); ++i) {
    if (items_[i].uid == uid) return i;
  }
  return items_.size();
}

bool post::has(std::string_view path) const noexcept {
  return std::any_of(items_.begin(), items_.end(), [&](const chosen& c) { return c.path == path; });
}

bool post::add(std::string path, std::string name, bool video) {
  if (items_.size() >= kMaxChosen || has(path)) return false;
  chosen c;
  c.uid = next_uid_++;
  c.path = std::move(path);
  c.name = std::move(name);
  c.video = video;
  items_.push_back(std::move(c));
  return true;
}

bool post::remove(std::uint64_t uid) {
  const std::size_t i = index_of(uid);
  if (i == items_.size()) return false;
  items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
  if (focus_ == uid) {
    // The neighbour that slides into the gap, so the next press means something.
    focus_ = items_.empty() ? 0 : items_[std::min(i, items_.size() - 1)].uid;
  }
  return true;
}

bool post::replace(std::uint64_t uid, std::string path, std::string name) {
  chosen* c = find(uid);
  if (!c || has(path)) return false;
  c->path = std::move(path);
  c->name = std::move(name);
  return true;
}

bool post::move(std::uint64_t uid, int by) {
  const std::size_t i = index_of(uid);
  if (i == items_.size()) return false;
  if ((by < 0 && i == 0) || (by > 0 && i + 1 == items_.size())) return false;
  std::swap(items_[i], items_[by < 0 ? i - 1 : i + 1]);
  return true;
}

post::selection_change post::apply_selection(std::span<const described> wanted) {
  selection_change out;
  std::vector<std::uint64_t> gone;
  for (const chosen& c : items_) {
    const bool kept = std::any_of(wanted.begin(), wanted.end(), [&](const described& d) { return d.path == c.path; });
    if (!kept) gone.push_back(c.uid);
  }
  for (const std::uint64_t uid : gone) {
    if (remove(uid)) ++out.removed;
  }
  for (const described& d : wanted) {
    if (has(d.path)) continue;
    if (items_.size() >= kMaxChosen) {
      ++out.refused;
      continue;
    }
    if (add(d.path, d.name, d.video)) {
      ++out.added;
      out.last_added = items_.back().uid;
    }
  }
  return out;
}

std::vector<std::string> post::paths() const {
  std::vector<std::string> out;
  out.reserve(items_.size());
  for (const chosen& c : items_) out.push_back(c.path);
  return out;
}

bool name_matches(std::string_view name, std::string_view query) noexcept {
  if (query.empty()) return true;
  if (query.size() > name.size()) return false;
  return std::search(name.begin(), name.end(), query.begin(), query.end(),
                     [](char a, char b) { return lower(a) == lower(b); }) != name.end();
}

bool looks_like_video(std::string_view name) noexcept { return ends_with_any(name, kClips); }
bool listable(std::string_view name) noexcept { return ends_with_any(name, kClips) || ends_with_any(name, kStills); }

}  // namespace mv::instagram
