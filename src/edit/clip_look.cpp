// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit/clip_look.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mv::edit::clip {

namespace {

struct ratio_row {
  crop_ratio r;
  const char* name;
  std::uint32_t w, h;
};
constexpr ratio_row kRatios[] = {
    {crop_ratio::original, "original", 0, 0}, {crop_ratio::r16_9, "16:9", 16, 9}, {crop_ratio::r9_16, "9:16", 9, 16},
    {crop_ratio::r1_1, "1:1", 1, 1},           {crop_ratio::r4_3, "4:3", 4, 3},   {crop_ratio::r4_5, "4:5", 4, 5},
};

}  // namespace

look sanitised(look l) noexcept {
  l.turns &= 3u;
  if (l.crop >= crop_ratio::count) l.crop = crop_ratio::original;
  // edit::range_of's limits, spelled here so the clip library needs no edit stack.
  for (int i = 0; i < kAdjustParamCount; ++i) {
    float& v = l.light.v[static_cast<std::size_t>(i)];
    const float lim = static_cast<adjust_param>(i) == adjust_param::exposure ? 5.0f : 100.0f;
    v = std::isfinite(v) ? std::clamp(v, -lim, lim) : 0.0f;
  }
  l.volume = std::isfinite(l.volume) ? std::clamp(l.volume, 0.0f, 2.0f) : 1.0f;
  l.fade_in_ms = std::clamp(l.fade_in_ms, 0, kMaxFadeMs);
  l.fade_out_ms = std::clamp(l.fade_out_ms, 0, kMaxFadeMs);
  l.speed = !std::isfinite(l.speed) ? 1.0f : l.speed < 0.75f ? 0.5f : l.speed < 1.5f ? 1.0f : 2.0f;
  return l;
}

const char* crop_name(crop_ratio r) noexcept {
  for (const ratio_row& row : kRatios) {
    if (row.r == r) return row.name;
  }
  return "original";
}

bool crop_from_name(const char* name, crop_ratio& out) noexcept {
  if (name == nullptr) return false;
  for (const ratio_row& row : kRatios) {
    if (std::strcmp(row.name, name) == 0) {
      out = row.r;
      return true;
    }
  }
  return false;
}

frame_geometry geometry_of(std::uint32_t coded_w, std::uint32_t coded_h, int source_rotation,
                           const look& l) noexcept {
  frame_geometry g;
  const int src_turns = ((source_rotation % 360 + 360) % 360) / 90;
  g.turns = static_cast<std::uint32_t>(src_turns + l.turns) & 3u;
  g.flip_h = l.flip_h;
  g.flip_v = l.flip_v;
  const bool sideways = (g.turns & 1u) != 0;
  g.shown_w = sideways ? coded_h : coded_w;
  g.shown_h = sideways ? coded_w : coded_h;
  std::uint32_t w = g.shown_w;
  std::uint32_t h = g.shown_h;
  for (const ratio_row& row : kRatios) {
    if (row.r != l.crop || row.w == 0) continue;
    // The largest centred rectangle of that ratio.
    if (static_cast<std::uint64_t>(g.shown_w) * row.h > static_cast<std::uint64_t>(g.shown_h) * row.w) {
      w = static_cast<std::uint32_t>(std::lround(static_cast<double>(g.shown_h) * row.w / row.h));
    } else {
      h = static_cast<std::uint32_t>(std::lround(static_cast<double>(g.shown_w) * row.h / row.w));
    }
  }
  // Even, for 4:2:0 encoders; never larger than the picture.
  w = std::max<std::uint32_t>(2, std::min(w, g.shown_w) & ~1u);
  h = std::max<std::uint32_t>(2, std::min(h, g.shown_h) & ~1u);
  g.out_w = std::min(w, g.shown_w);
  g.out_h = std::min(h, g.shown_h);
  g.crop_x = (g.shown_w - g.out_w) / 2;
  g.crop_y = (g.shown_h - g.out_h) / 2;
  return g;
}

void apply_geometry(const frame_geometry& g, std::span<const std::uint8_t> src, std::uint32_t coded_w,
                    std::uint32_t coded_h, std::uint32_t channels, std::span<std::uint8_t> dst) noexcept {
  const std::size_t need_src = static_cast<std::size_t>(coded_w) * coded_h * channels;
  const std::size_t need_dst = static_cast<std::size_t>(g.out_w) * g.out_h * channels;
  if (src.size() < need_src || dst.size() < need_dst || g.out_w > g.shown_w || g.out_h > g.shown_h) return;
  const std::size_t row = static_cast<std::size_t>(coded_w) * channels;
  // No turn and no mirror: a crop is a row copy.
  if (g.turns == 0 && !g.flip_h) {
    for (std::uint32_t y = 0; y < g.out_h; ++y) {
      const std::uint32_t sy = g.flip_v ? g.shown_h - 1 - (g.crop_y + y) : g.crop_y + y;
      std::memcpy(dst.data() + static_cast<std::size_t>(y) * g.out_w * channels,
                  src.data() + sy * row + static_cast<std::size_t>(g.crop_x) * channels,
                  static_cast<std::size_t>(g.out_w) * channels);
    }
    return;
  }
  std::uint8_t* out = dst.data();
  for (std::uint32_t y = 0; y < g.out_h; ++y) {
    for (std::uint32_t x = 0; x < g.out_w; ++x) {
      std::uint32_t cx = 0, cy = 0;
      coded_of(g, coded_w, coded_h, x, y, cx, cy);
      const std::uint8_t* p = src.data() + cy * row + static_cast<std::size_t>(cx) * channels;
      for (std::uint32_t c = 0; c < channels; ++c) *out++ = p[c];
    }
  }
}

colour_table::colour_table(const colour& c) : identity_(c.identity()) {
  if (identity_) return;
  constexpr double kGamma = 2.4;  // BT.1886, as the canvas decodes SDR video
  // PR 11's five sliders only (kLookLightParams): their stages are a0
  // (gains, contrast slope) and a1 (saturation, pivot); the kernel's later
  // stages stay the identity on a clip.
  const adjust_uniforms u = uniforms_of(c);
  saturation_ = u.a[1][0];
  const double pivot = u.a[1][1];
  for (int ch = 0; ch < 3; ++ch) {
    for (int code = 0; code < 256; ++code) {
      const double x = std::pow(code / 255.0, kGamma) * u.a[0][ch];
      const double t = x <= 0.0 ? 0.0 : pivot * std::pow(x / pivot, static_cast<double>(u.a[0][3]));
      linear_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(code)] = static_cast<float>(t);
    }
  }
  encode_.resize(kEncodeSteps + 1);
  for (int i = 0; i <= kEncodeSteps; ++i) {
    const double v = std::pow(static_cast<double>(i) / kEncodeSteps, 1.0 / kGamma);
    encode_[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
  }
}

void colour_table::apply(std::span<std::uint8_t> px, std::uint32_t channels) const noexcept {
  if (identity_ || channels < 3) return;
  const std::size_t n = px.size() / channels;
  std::uint8_t* p = px.data();
  for (std::size_t i = 0; i < n; ++i, p += channels) {
    const float r = linear_[0][p[0]];
    const float g = linear_[1][p[1]];
    const float b = linear_[2][p[2]];
    const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    const float out[3] = {luma + (r - luma) * saturation_, luma + (g - luma) * saturation_,
                          luma + (b - luma) * saturation_};
    for (int c = 0; c < 3; ++c) {
      const float v = std::clamp(out[c], 0.0f, 1.0f);
      p[c] = encode_[static_cast<std::size_t>(v * static_cast<float>(kEncodeSteps) + 0.5f)];
    }
  }
}

uv_crop uv_crop_of(const frame_geometry& g) noexcept {
  uv_crop u;
  if (g.shown_w == 0 || g.shown_h == 0) return u;
  u.size[0] = static_cast<float>(g.out_w) / static_cast<float>(g.shown_w);
  u.size[1] = static_cast<float>(g.out_h) / static_cast<float>(g.shown_h);
  u.origin[0] = static_cast<float>(g.crop_x) / static_cast<float>(g.shown_w);
  u.origin[1] = static_cast<float>(g.crop_y) / static_cast<float>(g.shown_h);
  return u;
}

}  // namespace mv::edit::clip
