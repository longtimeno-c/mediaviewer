// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/brighten.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace mv::enhance {
namespace {

// The scene brightness each amount lifts to, and the most it lifts.
struct amount_params {
  double target_key;
  double max_gain;
};
amount_params params_of(brighten_amount a) noexcept {
  switch (a) {
    case brighten_amount::some: return {0.09, 3.0};
    case brighten_amount::more: return {0.15, 6.0};
    case brighten_amount::off: break;
  }
  return {0.0, 1.0};
}

constexpr double kTauS = 0.5;       // the gain's easing time constant
constexpr float kCutDistance = 0.5f;  // half the histogram moved: a new scene
constexpr std::uint32_t kStep = 4;    // every fourth pixel each way

double to_linear(double v) noexcept {
  return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}
double to_srgb(double v) noexcept {
  return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

const std::array<float, 256>& linear_table() {
  static const std::array<float, 256> t = [] {
    std::array<float, 256> a{};
    for (int i = 0; i < 256; ++i) a[static_cast<std::size_t>(i)] = static_cast<float>(to_linear(i / 255.0));
    return a;
  }();
  return t;
}

}  // namespace

brighten_amount brighten_of(std::string_view id) noexcept {
  if (id == "some") return brighten_amount::some;
  if (id == "more") return brighten_amount::more;
  return brighten_amount::off;
}

double brighten_curve(double x, double gain) noexcept {
  x = std::clamp(x, 0.0, 1.0);
  return gain * x / (1.0 + (gain - 1.0) * x);
}

brightener::brightener(brighten_amount amount) noexcept : amount_(amount) {}

brighten_fit brightener::apply(std::span<const std::uint8_t> rgb, std::uint32_t width, std::uint32_t height,
                               std::int64_t pts_us, std::vector<std::uint8_t>& out) {
  brighten_fit fit;
  out.assign(rgb.begin(), rgb.end());
  if (!active() || width == 0 || height == 0 || rgb.size() < std::size_t{width} * height * 3) return fit;
  const auto& lin = linear_table();

  // The key and a 32-bin histogram of luma, from a sparse grid.
  double log_sum = 0;
  std::size_t n = 0;
  std::array<float, 32> hist{};
  for (std::uint32_t y = kStep / 2; y < height; y += kStep) {
    const std::uint8_t* row = rgb.data() + std::size_t{y} * width * 3;
    for (std::uint32_t x = kStep / 2; x < width; x += kStep) {
      const std::uint8_t* p = row + std::size_t{x} * 3;
      const double l = 0.2126 * lin[p[0]] + 0.7152 * lin[p[1]] + 0.0722 * lin[p[2]];
      log_sum += std::log(l + 1e-4);
      const int luma8 = (54 * p[0] + 183 * p[1] + 19 * p[2]) >> 8;
      hist[static_cast<std::size_t>(luma8 >> 3)] += 1.0f;
      ++n;
    }
  }
  if (n == 0) {  // smaller than the grid: every pixel once
    for (std::size_t i = 0; i + 2 < rgb.size(); i += 3) {
      const double l = 0.2126 * lin[rgb[i]] + 0.7152 * lin[rgb[i + 1]] + 0.0722 * lin[rgb[i + 2]];
      log_sum += std::log(l + 1e-4);
      hist[static_cast<std::size_t>(((54 * rgb[i] + 183 * rgb[i + 1] + 19 * rgb[i + 2]) >> 8) >> 3)] += 1.0f;
      ++n;
    }
  }
  for (float& h : hist) h /= static_cast<float>(n);
  fit.key = std::max(0.0, std::exp(log_sum / static_cast<double>(n)) - 1e-4);

  const amount_params prm = params_of(amount_);
  fit.target = std::clamp(prm.target_key / std::max(fit.key, 1e-4), 1.0, prm.max_gain);

  float distance = 0;
  for (std::size_t i = 0; i < hist.size(); ++i) distance += std::fabs(hist[i] - last_hist_[i]);
  fit.cut = !started_ || distance * 0.5f > kCutDistance;
  const double want = std::log(fit.target);
  if (fit.cut) {
    log_gain_ = want;
  } else {
    const double dt = std::max<double>(0, static_cast<double>(pts_us - last_pts_)) / 1e6;
    log_gain_ += (want - log_gain_) * (1.0 - std::exp(-dt / kTauS));
  }
  started_ = true;
  last_pts_ = pts_us;
  last_hist_ = hist;
  fit.gain = std::exp(log_gain_);
  if (fit.gain < 1.0 + 1e-3) return fit;  // nothing to lift: the frame as it was

  std::array<std::uint8_t, 256> lut{};
  for (int i = 0; i < 256; ++i) {
    const double v = to_srgb(brighten_curve(lin[static_cast<std::size_t>(i)], fit.gain));
    lut[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
  }
  for (std::uint8_t& v : out) v = lut[v];
  return fit;
}

}  // namespace mv::enhance
