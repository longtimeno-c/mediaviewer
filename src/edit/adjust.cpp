// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit/adjust.h"

#include <algorithm>
#include <cmath>

#include "edit/edit_stack.h"

namespace mv::edit {
namespace {

// Contrast ±100 maps to a slope of 2^(±0.585) ≈ ×1.5 / ÷1.5 in log exposure.
constexpr float kContrastStops = 0.585f;
// Temperature ±100 moves the assumed illuminant ±100 mireds from D65
// (6504 K ≈ 153.8 mired): about 3940 K to 18 600 K.
constexpr double kMiredPerUnit = 1.0;
constexpr double kD65Mired = 1.0e6 / 6504.0;
// Tint ±100 scales green by 2^(∓0.5) before the luminance normalisation.
constexpr float kTintStops = 0.5f;

// CIE daylight locus (CIE 15), valid 4000–25 000 K; the slider's cool end
// (3940 K) is a hair outside and still smooth.
void daylight_xy(double t, double& x, double& y) noexcept {
  const double t2 = t * t, t3 = t2 * t;
  if (t <= 7000.0) {
    x = -4.6070e9 / t3 + 2.9678e6 / t2 + 0.09911e3 / t + 0.244063;
  } else {
    x = -2.0064e9 / t3 + 1.9018e6 / t2 + 0.24748e3 / t + 0.237040;
  }
  y = -3.000 * x * x + 2.870 * x - 0.275;
}

// Linear Rec.709 RGB of the white with chromaticity (x, y) at Y = 1.
void white_rgb(double x, double y, double rgb[3]) noexcept {
  const double X = x / y, Y = 1.0, Z = (1.0 - x - y) / y;
  rgb[0] = 3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z;
  rgb[1] = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z;
  rgb[2] = 0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z;
}

}  // namespace

adjust_range range_of(adjust_param p) noexcept {
  switch (p) {
    case adjust_param::exposure: return {-5.0f, 5.0f, 0.1f, "Exposure"};
    case adjust_param::contrast: return {-100.0f, 100.0f, 5.0f, "Contrast"};
    case adjust_param::saturation: return {-100.0f, 100.0f, 5.0f, "Saturation"};
    case adjust_param::temperature: return {-100.0f, 100.0f, 5.0f, "Temperature"};
    case adjust_param::tint: return {-100.0f, 100.0f, 5.0f, "Tint"};
    default: return {};
  }
}

float clamp_param(adjust_param p, float value) noexcept {
  if (!std::isfinite(value)) return 0.0f;
  const adjust_range r = range_of(p);
  value = std::clamp(value, r.min, r.max);
  return value == -0.0f ? 0.0f : value;
}

colour fold_colour(std::span<const op> ops) noexcept {
  colour c;
  for (const op& o : ops) {
    if (o.kind != op_kind::adjust) continue;
    if (o.param == adjust_param::count) {  // reset every colour parameter
      c = colour{};
      continue;
    }
    const auto i = static_cast<std::size_t>(o.param);
    if (i >= c.v.size()) continue;
    c.v[i] = clamp_param(o.param, o.value);
  }
  return c;
}

std::array<float, 3> white_balance_gains(float temperature, float tint) noexcept {
  std::array<float, 3> g{1.0f, 1.0f, 1.0f};
  if (temperature == 0.0f && tint == 0.0f) return g;
  if (temperature != 0.0f) {
    // Warmer = the scene is taken to have been lit bluer than D65, so the
    // correction divides by a bluer white: red up, blue down.
    const double mired = kD65Mired - temperature * kMiredPerUnit;
    double ref[3], lit[3], x = 0, y = 0;
    daylight_xy(1.0e6 / kD65Mired, x, y);
    white_rgb(x, y, ref);
    daylight_xy(1.0e6 / mired, x, y);
    white_rgb(x, y, lit);
    for (int i = 0; i < 3; ++i) g[static_cast<std::size_t>(i)] = static_cast<float>(ref[i] / lit[i]);
  }
  g[1] *= std::exp2(-tint / 100.0f * kTintStops);
  const float luma = 0.2126f * g[0] + 0.7152f * g[1] + 0.0722f * g[2];
  if (luma > 0.0f) {
    for (float& v : g) v /= luma;
  }
  return g;
}

adjust_uniforms uniforms_of(const colour& c) noexcept {
  adjust_uniforms u;
  const std::array<float, 3> wb =
      white_balance_gains(c.get(adjust_param::temperature), c.get(adjust_param::tint));
  const float ev = c.get(adjust_param::exposure);
  const float gain = ev == 0.0f ? 1.0f : std::exp2(ev);
  u.a0[0] = wb[0] * gain;
  u.a0[1] = wb[1] * gain;
  u.a0[2] = wb[2] * gain;
  const float contrast = c.get(adjust_param::contrast);
  u.a0[3] = contrast == 0.0f ? 1.0f : std::exp2(contrast / 100.0f * kContrastStops);
  u.a1[0] = 1.0f + c.get(adjust_param::saturation) / 100.0f;
  u.a1[1] = 0.18f;
  return u;
}

}  // namespace mv::edit
