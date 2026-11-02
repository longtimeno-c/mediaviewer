// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit/adjust.h"

#include <algorithm>
#include <cmath>

#include "edit/edit_stack.h"

namespace mv::edit {
namespace {

// Contrast ±100 maps to a slope of 2^(±kContrastStops) ≈ ×1.5 / ÷1.5 in log
// exposure (adjust.h).
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
    case adjust_param::highlights: return {-100.0f, 100.0f, 5.0f, "Highlights"};
    case adjust_param::shadows: return {-100.0f, 100.0f, 5.0f, "Shadows"};
    case adjust_param::whites: return {-100.0f, 100.0f, 5.0f, "Whites"};
    case adjust_param::blacks: return {-100.0f, 100.0f, 5.0f, "Blacks"};
    case adjust_param::vibrance: return {-100.0f, 100.0f, 5.0f, "Vibrance"};
    case adjust_param::mono: return {0.0f, 100.0f, 5.0f, "Intensity"};
    case adjust_param::mono_filter: return {-100.0f, 100.0f, 5.0f, "Filter"};
    case adjust_param::mono_tone: return {-100.0f, 100.0f, 5.0f, "Tone"};
    case adjust_param::vignette: return {-100.0f, 100.0f, 5.0f, "Strength"};
    case adjust_param::vignette_radius: return {-100.0f, 100.0f, 5.0f, "Radius"};
    case adjust_param::vignette_softness: return {-100.0f, 100.0f, 5.0f, "Softness"};
    case adjust_param::levels_black:
    case adjust_param::levels_red_black:
    case adjust_param::levels_green_black:
    case adjust_param::levels_blue_black: return {0.0f, 100.0f, 1.0f, "Black"};
    case adjust_param::levels_mid:
    case adjust_param::levels_red_mid:
    case adjust_param::levels_green_mid:
    case adjust_param::levels_blue_mid: return {-100.0f, 100.0f, 2.0f, "Mid-tones"};
    case adjust_param::levels_white:
    case adjust_param::levels_red_white:
    case adjust_param::levels_green_white:
    case adjust_param::levels_blue_white: return {-100.0f, 0.0f, 1.0f, "White"};
    default: break;
  }
  if (p >= adjust_param::sel_red_hue && p < adjust_param::count) {
    constexpr const char* names[3] = {"Hue", "Saturation", "Luminance"};
    const int k = (static_cast<int>(p) - static_cast<int>(adjust_param::sel_red_hue)) % 3;
    return {-100.0f, 100.0f, 5.0f, names[k]};
  }
  switch (p) {
    default: return {};
  }
}

adjust_section section_of(adjust_param p) noexcept {
  switch (p) {
    case adjust_param::exposure:
    case adjust_param::contrast:
    case adjust_param::highlights:
    case adjust_param::shadows:
    case adjust_param::whites:
    case adjust_param::blacks: return adjust_section::light;
    case adjust_param::saturation:
    case adjust_param::temperature:
    case adjust_param::tint:
    case adjust_param::vibrance: return adjust_section::colour;
    case adjust_param::mono:
    case adjust_param::mono_filter:
    case adjust_param::mono_tone: return adjust_section::mono;
    case adjust_param::vignette:
    case adjust_param::vignette_radius:
    case adjust_param::vignette_softness: return adjust_section::vignette;
    case adjust_param::levels_black:
    case adjust_param::levels_mid:
    case adjust_param::levels_white:
    case adjust_param::levels_red_black:
    case adjust_param::levels_red_mid:
    case adjust_param::levels_red_white:
    case adjust_param::levels_green_black:
    case adjust_param::levels_green_mid:
    case adjust_param::levels_green_white:
    case adjust_param::levels_blue_black:
    case adjust_param::levels_blue_mid:
    case adjust_param::levels_blue_white: return adjust_section::levels;
    default: break;
  }
  if (p >= adjust_param::sel_red_hue && p < adjust_param::count) return adjust_section::selective;
  switch (p) {
    default: return adjust_section::count;
  }
}

colour without_section(const colour& c, adjust_section s) noexcept {
  colour out = c;
  for (int i = 0; i < kAdjustParamCount; ++i) {
    if (section_of(static_cast<adjust_param>(i)) == s) out.v[static_cast<std::size_t>(i)] = 0.0f;
  }
  if (s == adjust_section::curves) out.curves = {};
  if (s == adjust_section::red_eye) {
    out.spots = {};
    out.spot_count = 0;
  }
  return out;
}

bool colour::has_selective() const noexcept {
  for (int i = static_cast<int>(adjust_param::sel_red_hue); i < kAdjustParamCount; ++i) {
    if (v[static_cast<std::size_t>(i)] != 0.0f) return true;
  }
  return false;
}

std::array<float, 2> neutral_balance(const std::array<double, 3>& rgb) noexcept {
  if (!(rgb[0] > 0.0 && rgb[1] > 0.0 && rgb[2] > 0.0)) return {0.0f, 0.0f};
  auto err = [&](float t, float n) {
    const std::array<float, 3> g = white_balance_gains(t, n);
    const double r = std::log(g[0] * rgb[0]), gr = std::log(g[1] * rgb[1]), b = std::log(g[2] * rgb[2]);
    return (r - gr) * (r - gr) + (b - gr) * (b - gr);
  };
  float bt = 0.0f, bn = 0.0f;
  double best = err(0.0f, 0.0f);
  auto search = [&](float t0, float n0, int reach, float step) {
    for (int i = -reach; i <= reach; ++i) {
      for (int j = -reach; j <= reach; ++j) {
        const float t = std::clamp(t0 + step * static_cast<float>(i), -100.0f, 100.0f);
        const float n = std::clamp(n0 + step * static_cast<float>(j), -100.0f, 100.0f);
        const double e = err(t, n);
        if (e < best) {
          best = e;
          bt = t;
          bn = n;
        }
      }
    }
  };
  search(0.0f, 0.0f, 20, 5.0f);
  search(bt, bn, 5, 1.0f);
  search(bt, bn, 5, 0.2f);
  return {std::round(bt), std::round(bn)};
}

curve sanitise(const curve& c) noexcept {
  struct pt {
    float x, y;
  };
  pt p[kCurvePoints];
  int n = 0;
  for (int i = 0; i < std::min<int>(c.n, kCurvePoints); ++i) {
    if (!std::isfinite(c.x[i]) || !std::isfinite(c.y[i])) continue;
    p[n++] = {std::clamp(c.x[i], 0.0f, 1.0f), std::clamp(c.y[i], 0.0f, 1.0f)};
  }
  std::stable_sort(p, p + n, [](const pt& a, const pt& b) { return a.x < b.x; });
  curve out;
  for (int i = 0; i < n; ++i) {
    if (out.n > 0 && p[i].x - out.x[out.n - 1] < 1.0f / 64.0f) continue;
    out.x[out.n] = p[i].x == -0.0f ? 0.0f : p[i].x;
    out.y[out.n] = p[i].y == -0.0f ? 0.0f : p[i].y;
    ++out.n;
  }
  if (out.n < 2) return curve{};
  return out;
}

float evaluate(const curve& c, float p) noexcept {
  if (c.n < 2) return p;
  const int n = c.n;
  if (p <= c.x[0]) return c.y[0];
  if (p >= c.x[n - 1]) return c.y[n - 1];
  // Fritsch–Carlson: secants, then tangents limited so each segment stays
  // monotone where its ends are (no overshoot between points).
  float d[kCurvePoints], m[kCurvePoints];
  for (int i = 0; i + 1 < n; ++i) d[i] = (c.y[i + 1] - c.y[i]) / (c.x[i + 1] - c.x[i]);
  m[0] = d[0];
  m[n - 1] = d[n - 2];
  for (int i = 1; i + 1 < n; ++i) m[i] = d[i - 1] * d[i] <= 0.0f ? 0.0f : (d[i - 1] + d[i]) / 2.0f;
  for (int i = 0; i + 1 < n; ++i) {
    if (d[i] == 0.0f) {
      m[i] = m[i + 1] = 0.0f;
      continue;
    }
    const float a = m[i] / d[i], b = m[i + 1] / d[i];
    const float h = a * a + b * b;
    if (h > 9.0f) {
      const float t = 3.0f / std::sqrt(h);
      m[i] = t * a * d[i];
      m[i + 1] = t * b * d[i];
    }
  }
  int k = 0;
  while (k + 2 < n && p > c.x[k + 1]) ++k;
  const float h = c.x[k + 1] - c.x[k];
  const float t = (p - c.x[k]) / h;
  const float t2 = t * t, t3 = t2 * t;
  return (2 * t3 - 3 * t2 + 1) * c.y[k] + (t3 - 2 * t2 + t) * h * m[k] + (-2 * t3 + 3 * t2) * c.y[k + 1] +
         (t3 - t2) * h * m[k + 1];
}

bool colour::has_table() const noexcept {
  for (int i = 0; i < kAdjustParamCount; ++i) {
    if (section_of(static_cast<adjust_param>(i)) == adjust_section::levels &&
        v[static_cast<std::size_t>(i)] != 0.0f) {
      return true;
    }
  }
  for (const curve& k : curves) {
    if (!k.identity()) return true;
  }
  return false;
}

namespace {

// Levels for one channel: `first` is its black parameter (mid and white follow).
float levels(const colour& c, adjust_param first, float p) noexcept {
  const auto at = [&](int k) { return c.v[static_cast<std::size_t>(first) + static_cast<std::size_t>(k)]; };
  const float black = at(0) / 100.0f * 0.5f;
  const float white = 1.0f + at(2) / 100.0f * 0.5f;
  const float gamma = std::exp2(at(1) / 100.0f);  // +100: twice as light in the mid-tones
  float t = (p - black) / std::max(white - black, 1e-3f);
  t = std::clamp(t, 0.0f, 1.0f);
  return gamma == 1.0f ? t : std::pow(t, 1.0f / gamma);
}

}  // namespace

gfx::adjust_table table_of(const colour& c) noexcept {
  gfx::adjust_table t;
  if (c.has_selective()) {
    // Each band's three values at its centre, linear between neighbouring
    // centres around the wheel.
    for (int k = 0; k < gfx::kAdjustHueRows; ++k) {
      const float h = static_cast<float>(k) / static_cast<float>(gfx::kAdjustHueRows);
      int a = kSelectiveBands - 1;
      for (int b = 0; b < kSelectiveBands; ++b) {
        if (kSelectiveCentres[b] <= h) a = b;
      }
      const int b = (a + 1) % kSelectiveBands;
      float span = kSelectiveCentres[b] - kSelectiveCentres[a];
      if (span <= 0.0f) span += 1.0f;
      float off = h - kSelectiveCentres[a];
      if (off < 0.0f) off += 1.0f;
      const float w = off / span;
      auto band = [&](int n, int field) {
        return c.v[static_cast<std::size_t>(adjust_param::sel_red_hue) + static_cast<std::size_t>(n * 3 + field)] /
               100.0f;
      };
      float* row = t.r[gfx::kAdjustHueFirst + k];
      const auto mix = [&](int field) { return band(a, field) + (band(b, field) - band(a, field)) * w; };
      row[0] = mix(0) * kSelectiveHueTurns;
      row[1] = 1.0f + mix(1);
      row[2] = mix(2) * kSelectiveLumStops;
      row[3] = 0.0f;
    }
  }
  if (!c.has_table()) return t;
  constexpr adjust_param firsts[3] = {adjust_param::levels_red_black, adjust_param::levels_green_black,
                                      adjust_param::levels_blue_black};
  for (int i = 0; i < gfx::kAdjustTableRows; ++i) {
    const float p = static_cast<float>(i) / static_cast<float>(gfx::kAdjustTableRows - 1);
    const float m = evaluate(c.curve_of(curve_channel::master), levels(c, adjust_param::levels_black, p));
    for (int ch = 0; ch < 3; ++ch) {
      const curve& k = c.curves[static_cast<std::size_t>(ch) + 1];
      t.r[i][ch] = evaluate(k, levels(c, firsts[ch], m));
    }
  }
  return t;
}

bool is_colour_op(const op& o) noexcept {
  return o.kind == op_kind::adjust || o.kind == op_kind::adjust_all || o.kind == op_kind::curve ||
         o.kind == op_kind::red_eye;
}

void place_spots(gfx::adjust_table& t, const colour& c, const placement& p) noexcept {
  for (int i = 0; i < gfx::kAdjustSpotRows; ++i) {
    for (float& x : t.r[gfx::kAdjustSpotFirst + i]) x = 0.0f;
  }
  if (c.spot_count == 0 || p.cropped.w == 0 || p.cropped.h == 0) return;
  const affine inv = invert(p.map);
  // pos units: 2 / the output's diagonal per output pixel, and an oriented
  // source pixel is output.w / cropped.w output pixels.
  const double ow = p.output.w, oh = p.output.h;
  const double out_diag = std::sqrt(ow * ow + oh * oh);
  const double sw = p.oriented.w, sh = p.oriented.h;
  const double src_diag = std::sqrt(sw * sw + sh * sh);
  const double scale = ow / static_cast<double>(p.cropped.w);  // output px per source px
  for (int i = 0; i < std::min<int>(c.spot_count, gfx::kAdjustSpotRows); ++i) {
    const red_eye_spot& s = c.spots[static_cast<std::size_t>(i)];
    const double u = inv.m[0] * s.x + inv.m[1] * s.y + inv.m[2];
    const double v = inv.m[3] * s.x + inv.m[4] * s.y + inv.m[5];
    float* row = t.r[gfx::kAdjustSpotFirst + i];
    row[0] = static_cast<float>((u - 0.5) * ow * 2.0 / out_diag);
    row[1] = static_cast<float>((v - 0.5) * oh * 2.0 / out_diag);
    row[2] = static_cast<float>(s.r * src_diag * scale * 2.0 / out_diag);
  }
}

std::array<float, 3> mono_weights(float filter) noexcept {
  constexpr std::array<float, 3> k709{0.2126f, 0.7152f, 0.0722f};
  if (filter == 0.0f) return k709;
  const std::array<float, 3>& end = filter > 0.0f ? kMonoRed : kMonoBlue;
  const float t = std::min(std::abs(filter) / 100.0f, 1.0f);
  std::array<float, 3> w{};
  for (std::size_t i = 0; i < 3; ++i) w[i] = k709[i] + (end[i] - k709[i]) * t;
  return w;
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
    if (o.kind == op_kind::adjust_all) {
      for (int i = 0; i < kAdjustParamCount; ++i) {
        const auto p = static_cast<adjust_param>(i);
        c.v[static_cast<std::size_t>(i)] = clamp_param(p, o.values.get(p));
      }
      for (std::size_t k = 0; k < c.curves.size(); ++k) c.curves[k] = sanitise(o.values.curves[k]);
      c.spots = o.values.spots;
      c.spot_count = std::min<std::uint8_t>(o.values.spot_count, kRedEyeSpots);
      continue;
    }
    if (o.kind == op_kind::red_eye) {
      if (o.spot.r <= 0.0f) {
        c.spots = {};
        c.spot_count = 0;
      } else if (c.spot_count < kRedEyeSpots && std::isfinite(o.spot.x) && std::isfinite(o.spot.y)) {
        c.spots[c.spot_count++] = {std::clamp(o.spot.x, 0.0f, 1.0f), std::clamp(o.spot.y, 0.0f, 1.0f),
                                   std::clamp(o.spot.r, 0.001f, 0.1f)};
      }
      continue;
    }
    if (o.kind == op_kind::curve) {
      const auto k = static_cast<std::size_t>(o.channel);
      if (k < c.curves.size()) c.curves[k] = sanitise(o.values.curves[k]);
      continue;
    }
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
  auto& a = u.a;
  const std::array<float, 3> wb =
      white_balance_gains(c.get(adjust_param::temperature), c.get(adjust_param::tint));
  const float ev = c.get(adjust_param::exposure);
  const float gain = ev == 0.0f ? 1.0f : std::exp2(ev);
  a[0][0] = wb[0] * gain;
  a[0][1] = wb[1] * gain;
  a[0][2] = wb[2] * gain;
  const float contrast = c.get(adjust_param::contrast);
  a[0][3] = contrast == 0.0f ? 1.0f : std::exp2(contrast / 100.0f * kContrastStops);
  a[1][0] = 1.0f + c.get(adjust_param::saturation) / 100.0f;
  a[1][1] = kContrastPivot;

  // Tone: the curve's terms on p = luma^(1/2.2) (adjust_kernel_tokens.h).
  // Highlights (p³(1−p)) and shadows (p(1−p)³) peak at 0.105 of p, at
  // p = 3/4 and 1/4, leave the other end nearly alone, and stay monotone at
  // ±100; whites (p³) and blacks ((1−p)³) move the ends.
  a[2][0] = c.get(adjust_param::highlights) / 100.0f;
  a[2][1] = c.get(adjust_param::shadows) / 100.0f;
  a[2][2] = c.get(adjust_param::whites) / 100.0f * kWhitesReach;
  a[2][3] = c.get(adjust_param::blacks) / 100.0f * kBlacksReach;
  const bool tone = a[2][0] != 0.0f || a[2][1] != 0.0f || a[2][2] != 0.0f || a[2][3] != 0.0f;
  a[1][2] = tone ? 1.0f : 0.0f;

  a[3][0] = c.get(adjust_param::vibrance) / 100.0f;
  a[3][1] = c.get(adjust_param::mono) / 100.0f;
  const float mono_tone = c.get(adjust_param::mono_tone);
  a[3][2] = mono_tone == 0.0f ? 1.0f : std::exp2(mono_tone / 100.0f * kContrastStops);
  const std::array<float, 3> mw = mono_weights(c.get(adjust_param::mono_filter));
  a[4][0] = mw[0];
  a[4][1] = mw[1];
  a[4][2] = mw[2];

  // Vignette: from `inner` to `inner + width` of the way to the corner.
  // Radius moves the start (0.05 .. 0.65), softness the width (0.2 .. 1.0).
  a[5][0] = c.get(adjust_param::vignette) / 100.0f * kVignetteStops;
  const float radius = c.get(adjust_param::vignette_radius) / 100.0f;
  const float softness = c.get(adjust_param::vignette_softness) / 100.0f;
  const float inner = radius == 0.0f ? gfx::kVignetteInner : gfx::kVignetteInner + 0.3f * radius;
  const float width = softness == 0.0f ? gfx::kVignetteWidth : gfx::kVignetteWidth + 0.4f * softness;
  a[5][1] = inner;
  a[5][2] = inner + width;
  a[5][3] = c.has_table() ? 1.0f : 0.0f;
  a[3][3] = c.spot_count > 0 ? 1.0f : 0.0f;
  a[4][3] = c.has_selective() ? 1.0f : 0.0f;
  return u;
}

}  // namespace mv::edit
