// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/instagram/pixels.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace mv::instagram {
namespace {

struct curves {
  std::array<float, 256> to_linear{};
  std::array<std::uint8_t, 4097> to_srgb{};
  curves() {
    for (std::size_t i = 0; i < 256; ++i) {
      const double c = static_cast<double>(i) / 255.0;
      to_linear[i] = static_cast<float>(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
    }
    for (std::size_t i = 0; i < to_srgb.size(); ++i) {
      const double l = static_cast<double>(i) / 4096.0;
      const double s = l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
      to_srgb[i] = static_cast<std::uint8_t>(std::clamp(s * 255.0 + 0.5, 0.0, 255.0));
    }
  }
};

const curves& lut() {
  static const curves c;
  return c;
}

std::uint8_t encode(float linear, const curves& c) noexcept {
  const float v = std::clamp(linear, 0.0f, 1.0f) * 4096.0f + 0.5f;
  return c.to_srgb[static_cast<std::size_t>(v)];
}

}  // namespace

void scaler::build(std::uint32_t size, double from, double len, std::uint32_t out, std::vector<tap>& taps,
                   std::vector<float>& weights) {
  const double start = from * size;
  const double span = std::max(len * size, 1e-6);
  const double step = span / out;
  const double radius = std::max(1.0, step);
  taps.resize(out);
  for (std::uint32_t i = 0; i < out; ++i) {
    const double centre = start + (i + 0.5) * step;
    const auto lo = static_cast<std::int64_t>(std::floor(centre - radius));
    const auto hi = static_cast<std::int64_t>(std::ceil(centre + radius));
    const auto a = static_cast<std::uint32_t>(std::clamp<std::int64_t>(lo, 0, size - 1));
    const auto b = static_cast<std::uint32_t>(std::clamp<std::int64_t>(hi, 0, size - 1));
    tap t;
    t.first = a;
    t.begin = static_cast<std::uint32_t>(weights.size());
    double sum = 0;
    for (std::uint32_t k = a; k <= b; ++k) {
      const double w = std::max(0.0, 1.0 - std::abs(k + 0.5 - centre) / radius);
      weights.push_back(static_cast<float>(w));
      sum += w;
    }
    t.count = b - a + 1;
    if (sum <= 0) {
      // The sample fell outside the picture: the nearest edge.
      std::fill(weights.begin() + t.begin, weights.end(), 0.0f);
      weights[t.begin] = 1.0f;
      sum = 1;
    }
    for (std::uint32_t k = 0; k < t.count; ++k) weights[t.begin + k] = static_cast<float>(weights[t.begin + k] / sum);
    taps[i] = t;
  }
}

scaler::scaler(std::uint32_t sw, std::uint32_t sh, const rect& from, std::uint32_t dw, std::uint32_t dh)
    : sw_(sw), dw_(dw), dh_(dh) {
  build(sw, from.x, from.w, dw, xt_, xw_);
  build(sh, from.y, from.h, dh, yt_, yw_);
  std::uint32_t last = 0;
  first_row_ = sh;
  for (const tap& t : yt_) {
    first_row_ = std::min(first_row_, t.first);
    last = std::max(last, t.first + t.count);
  }
  rows_ = last > first_row_ ? last - first_row_ : 0;
  mid_.resize(static_cast<std::size_t>(rows_) * dw_ * 3);
}

void scaler::run(const std::uint8_t* src, std::uint8_t* out) {
  const curves& c = lut();
  for (std::uint32_t r = 0; r < rows_; ++r) {
    const std::uint8_t* row = src + static_cast<std::size_t>(first_row_ + r) * sw_ * 3;
    float* m = mid_.data() + static_cast<std::size_t>(r) * dw_ * 3;
    for (std::uint32_t x = 0; x < dw_; ++x) {
      const tap& t = xt_[x];
      float v[3] = {0, 0, 0};
      for (std::uint32_t k = 0; k < t.count; ++k) {
        const float w = xw_[t.begin + k];
        const std::uint8_t* p = row + static_cast<std::size_t>(t.first + k) * 3;
        v[0] += w * c.to_linear[p[0]];
        v[1] += w * c.to_linear[p[1]];
        v[2] += w * c.to_linear[p[2]];
      }
      m[x * 3] = v[0];
      m[x * 3 + 1] = v[1];
      m[x * 3 + 2] = v[2];
    }
  }
  std::vector<float> line(static_cast<std::size_t>(dw_) * 3);
  for (std::uint32_t y = 0; y < dh_; ++y) {
    const tap& t = yt_[y];
    std::fill(line.begin(), line.end(), 0.0f);
    for (std::uint32_t k = 0; k < t.count; ++k) {
      const float w = yw_[t.begin + k];
      const float* m = mid_.data() + static_cast<std::size_t>(t.first + k - first_row_) * dw_ * 3;
      for (std::size_t i = 0; i < line.size(); ++i) line[i] += w * m[i];
    }
    std::uint8_t* o = out + static_cast<std::size_t>(y) * dw_ * 3;
    for (std::size_t i = 0; i < line.size(); ++i) o[i] = encode(line[i], c);
  }
}

void turned(const std::uint8_t* src, std::uint32_t w, std::uint32_t h, std::uint32_t degrees, image& out) {
  const bool swaps = degrees == 90 || degrees == 270;
  out.w = swaps ? h : w;
  out.h = swaps ? w : h;
  out.rgb.resize(static_cast<std::size_t>(w) * h * 3);
  if (degrees != 90 && degrees != 180 && degrees != 270) {
    std::memcpy(out.rgb.data(), src, out.rgb.size());
    return;
  }
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      std::uint32_t nx = 0, ny = 0;
      if (degrees == 90) {
        nx = h - 1 - y;
        ny = x;
      } else if (degrees == 180) {
        nx = w - 1 - x;
        ny = h - 1 - y;
      } else {
        nx = y;
        ny = w - 1 - x;
      }
      std::memcpy(out.rgb.data() + (static_cast<std::size_t>(ny) * out.w + nx) * 3,
                  src + (static_cast<std::size_t>(y) * w + x) * 3, 3);
    }
  }
}

std::vector<std::uint8_t> crop_rgb(const image& pic, const rect& crop, std::uint32_t w, std::uint32_t h) {
  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w) * h * 3);
  scaler(pic.w, pic.h, crop, w, h).run(pic.rgb.data(), rgb.data());
  return rgb;
}

std::vector<std::uint8_t> tile_bgra(const image& pic, const rect& crop, std::uint32_t w, std::uint32_t h,
                                    std::uint32_t ring, std::uint32_t ring_px) {
  return bgra_of(crop_rgb(pic, crop, w, h), w, h, ring, ring_px);
}

std::vector<std::uint8_t> bgra_of(const std::vector<std::uint8_t>& rgb, std::uint32_t w, std::uint32_t h,
                                  std::uint32_t ring, std::uint32_t ring_px) {
  std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4);
  const auto rr = static_cast<std::uint8_t>(ring >> 24);
  const auto rg = static_cast<std::uint8_t>(ring >> 16);
  const auto rb = static_cast<std::uint8_t>(ring >> 8);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * w + x;
      const bool edge = ring_px > 0 && (x < ring_px || y < ring_px || x + ring_px >= w || y + ring_px >= h);
      out[i * 4] = edge ? rb : rgb[i * 3 + 2];
      out[i * 4 + 1] = edge ? rg : rgb[i * 3 + 1];
      out[i * 4 + 2] = edge ? rr : rgb[i * 3];
      out[i * 4 + 3] = 255;
    }
  }
  return out;
}

}  // namespace mv::instagram
