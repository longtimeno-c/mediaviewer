// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit/auto_adjust.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "edit/auto_tone.h"
#include "image/half.h"

namespace mv::edit {
namespace {

namespace kernel = gfx::kernel;

// A sampling step over `img` that reads at most kAutoSampleBudget pixels.
std::uint32_t step_of(const image::linear_image& img) noexcept {
  const std::uint64_t pixels = static_cast<std::uint64_t>(img.width) * img.height;
  std::uint32_t step = 1;
  while (pixels / (static_cast<std::uint64_t>(step) * step) > kAutoSampleBudget) ++step;
  return step;
}

// Every sampled, visible pixel of `img` as linear RGB, through `f`.
template <class F>
bool for_samples(const image::linear_image& img, const job_context* ctx, F&& f) {
  const std::uint32_t step = step_of(img);
  for (std::uint32_t y = step / 2; y < img.height; y += step) {
    if (ctx && ctx->cancelled()) return false;
    const std::uint16_t* row = img.rgba.data() + static_cast<std::size_t>(y) * img.width * 4;
    for (std::uint32_t x = step / 2; x < img.width; x += step) {
      const std::uint16_t* p = row + static_cast<std::size_t>(x) * 4;
      if (!(image::half_to_float(p[3]) > 0.0f)) continue;
      f(image::half_to_float(p[0]), image::half_to_float(p[1]), image::half_to_float(p[2]));
    }
  }
  return true;
}

double wb_error(const std::array<double, 3>& mean, float temperature, float tint) noexcept {
  const std::array<float, 3> g = white_balance_gains(temperature, tint);
  const double r = std::log(g[0] * mean[0]), gr = std::log(g[1] * mean[1]),
               b = std::log(g[2] * mean[2]);
  return (r - gr) * (r - gr) + (b - gr) * (b - gr);
}

void set(colour& c, adjust_param p, float v) noexcept {
  c.v[static_cast<std::size_t>(p)] = clamp_param(p, v);
}

}  // namespace

float neutral_confidence(float mean_saturation) noexcept {
  return std::clamp((kAutoNeutralNone - mean_saturation) / (kAutoNeutralNone - kAutoNeutralSure), 0.0f,
                    1.0f);
}

std::optional<std::array<double, 3>> sample_mean(const image::linear_image& working, float u, float v,
                                                 int half) noexcept {
  if (!working.valid() || !(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f)) return std::nullopt;
  const int w = static_cast<int>(working.width), h = static_cast<int>(working.height);
  const int cx = std::clamp(static_cast<int>(u * static_cast<float>(w)), 0, w - 1);
  const int cy = std::clamp(static_cast<int>(v * static_cast<float>(h)), 0, h - 1);
  std::array<double, 3> sum{};
  int n = 0;
  for (int y = std::max(0, cy - half); y <= std::min(h - 1, cy + half); ++y) {
    for (int x = std::max(0, cx - half); x <= std::min(w - 1, cx + half); ++x) {
      const std::uint16_t* p = working.rgba.data() + (static_cast<std::size_t>(y) * working.width + static_cast<std::size_t>(x)) * 4;
      for (std::size_t k = 0; k < 3; ++k) sum[k] += image::half_to_float(p[k]);
      ++n;
    }
  }
  if (n == 0) return std::nullopt;
  return std::array<double, 3>{sum[0] / n, sum[1] / n, sum[2] / n};
}

bool has_auto(adjust_section s) noexcept {
  return s == adjust_section::light || s == adjust_section::colour || s == adjust_section::mono ||
         s == adjust_section::levels;
}

std::array<float, 2> grey_world(const std::array<double, 3>& mean, float strength) noexcept {
  if (!(mean[0] > 0.0 && mean[1] > 0.0 && mean[2] > 0.0)) return {0.0f, 0.0f};
  // A coarse grid, then a fine one about its best point: ~1 700 calls of
  // white_balance_gains, microseconds.
  float bt = 0.0f, bn = 0.0f;
  double best = wb_error(mean, 0.0f, 0.0f);
  auto search = [&](float t0, float n0, int reach, float step) {
    for (int i = -reach; i <= reach; ++i) {
      for (int j = -reach; j <= reach; ++j) {
        const float t = t0 + step * static_cast<float>(i), n = n0 + step * static_cast<float>(j);
        const double e = wb_error(mean, t, n);
        if (e < best) {
          best = e;
          bt = t;
          bn = n;
        }
      }
    }
  };
  search(0.0f, 0.0f, 20, 5.0f);  // −100 .. +100 by 5
  search(bt, bn, 5, 1.0f);       // ±5 by 1 about the best
  auto finish = [strength](float v) {
    v = std::clamp(std::round(v * kAutoColourDamping * strength), -kAutoColourMax, kAutoColourMax);
    return std::abs(v) < kAutoColourMin ? 0.0f : v;
  };
  return {finish(bt), finish(bn)};
}

float best_mono_filter(const std::vector<std::array<float, 3>>& samples) noexcept {
  if (samples.size() < 2) return 0.0f;
  constexpr int kSteps = 9;  // −100 .. +100 by 25
  std::array<double, kSteps> var{};
  for (int i = 0; i < kSteps; ++i) {
    const std::array<float, 3> w = mono_weights(-100.0f + 25.0f * static_cast<float>(i));
    double sum = 0.0, sum2 = 0.0;
    for (const auto& c : samples) {
      const double g = std::clamp(w[0] * c[0] + w[1] * c[1] + w[2] * c[2], 0.0f, 1.0f);
      const double p = std::pow(g, 1.0 / 2.2);
      sum += p;
      sum2 += p * p;
    }
    const double n = static_cast<double>(samples.size());
    var[static_cast<std::size_t>(i)] = sum2 / n - (sum / n) * (sum / n);
  }
  const double top = *std::max_element(var.begin(), var.end());
  // Within 2 % of the best is a tie; the filter nearest 0 wins it, so a
  // photo that gains little keeps the plain Rec.709 mix.
  float pick = 0.0f;
  float pick_abs = 1000.0f;
  for (int i = 0; i < kSteps; ++i) {
    const float f = -100.0f + 25.0f * static_cast<float>(i);
    if (var[static_cast<std::size_t>(i)] >= top * 0.98 && std::abs(f) < pick_abs) {
      pick = f;
      pick_abs = std::abs(f);
    }
  }
  return pick;
}

result<auto_result> compute_auto(const image::linear_image& working, const colour& current,
                                 adjust_section section, const job_context* ctx) {
  if (!working.valid()) return err(status::invalid_arg);
  auto_result r;
  r.value = current;
  switch (section) {
    case adjust_section::light: {
      MV_TRY(auto_tone_result t, compute_auto_tone(working, current, ctx));
      r.value = t.value;
      break;
    }
    case adjust_section::colour: {
      // Neither clipped (a blown sky says nothing about the light) nor near
      // black (noise); of those, the least saturated quarter, so a frame
      // full of sea or leaves reads its light from the clouds, sand and
      // stone in it rather than taking the sea for a blue cast.
      std::vector<std::array<float, 4>> pick;  // r, g, b, saturation
      pick.reserve(static_cast<std::size_t>(kAutoSampleBudget));
      const bool done = for_samples(working, ctx, [&](float red, float green, float blue) {
        const float hi = std::max(red, std::max(green, blue));
        const float lo = std::min(red, std::min(green, blue));
        const float y = 0.2126f * red + 0.7152f * green + 0.0722f * blue;
        if (hi >= 0.9f || y <= 0.01f) return;
        pick.push_back({red, green, blue, (hi - lo) / hi});
      });
      if (!done) return err(status::cancelled);
      if (pick.size() < 64) break;  // too little to judge: no change
      const std::size_t keep = std::max<std::size_t>(16, pick.size() / 4);
      std::nth_element(pick.begin(), pick.begin() + static_cast<std::ptrdiff_t>(keep - 1), pick.end(),
                       [](const auto& a, const auto& b) { return a[3] < b[3]; });
      std::array<double, 4> sum{};
      for (std::size_t i = 0; i < keep; ++i) {
        for (std::size_t k = 0; k < 4; ++k) sum[k] += pick[i][k];
      }
      const auto count = static_cast<double>(keep);
      const std::array<double, 3> mean{sum[0] / count, sum[1] / count, sum[2] / count};
      const float confidence = neutral_confidence(static_cast<float>(sum[3] / count));
      const std::array<float, 2> wb = grey_world(mean, confidence);
      set(r.value, adjust_param::temperature, wb[0]);
      set(r.value, adjust_param::tint, wb[1]);
      break;
    }
    case adjust_section::mono: {
      // The colour as it reaches black & white: the current edit with black
      // & white and the vignette off.
      const colour before = without_section(without_section(current, adjust_section::mono),
                                            adjust_section::vignette);
      const gfx::adjust_table table = table_of(before);
      const gfx::adjust_args args(uniforms_of(before), &table);
      std::vector<std::array<float, 3>> samples;
      samples.reserve(static_cast<std::size_t>(kAutoSampleBudget));
      const bool done = for_samples(working, ctx, [&](float red, float green, float blue) {
        const kernel::float3 c = gfx::adjust({red, green, blue}, kernel::float4{}, args);
        samples.push_back({c.x, c.y, c.z});
      });
      if (!done) return err(status::cancelled);
      const float mono = current.get(adjust_param::mono);
      set(r.value, adjust_param::mono, mono > 0.0f ? mono : 100.0f);
      set(r.value, adjust_param::mono_filter, best_mono_filter(samples));
      break;
    }
    case adjust_section::levels: {
      // p of each channel as it reaches the table: the edit with levels,
      // curves, black & white and the vignette off.
      colour before = current;
      for (adjust_section s : {adjust_section::levels, adjust_section::curves, adjust_section::mono,
                               adjust_section::vignette}) {
        before = without_section(before, s);
      }
      const gfx::adjust_args args(uniforms_of(before));
      std::array<std::vector<float>, 3> ps;
      for (auto& v : ps) v.reserve(static_cast<std::size_t>(kAutoSampleBudget));
      const bool done = for_samples(working, ctx, [&](float red, float green, float blue) {
        const kernel::float3 c = gfx::adjust({red, green, blue}, kernel::float4{}, args);
        ps[0].push_back(std::pow(std::max(c.x, 0.0f), 1.0f / 2.2f));
        ps[1].push_back(std::pow(std::max(c.y, 0.0f), 1.0f / 2.2f));
        ps[2].push_back(std::pow(std::max(c.z, 0.0f), 1.0f / 2.2f));
      });
      if (!done) return err(status::cancelled);
      if (ps[0].size() < 64) break;
      r.value = without_section(current, adjust_section::levels);
      constexpr adjust_param firsts[3] = {adjust_param::levels_red_black, adjust_param::levels_green_black,
                                          adjust_param::levels_blue_black};
      for (std::size_t ch = 0; ch < 3; ++ch) {
        std::vector<float>& v = ps[ch];
        const auto at = [&v](double q) {
          const auto k = static_cast<std::size_t>(q * static_cast<double>(v.size() - 1));
          std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
          return v[k];
        };
        const float lo = at(kAutoLevelsClip), hi = at(1.0 - kAutoLevelsClip);
        if (hi - lo < 0.05f) continue;  // flat: nothing to stretch
        // Black p = black/100 * 0.5; white p = 1 + white/100 * 0.5 (adjust.cpp).
        const float black = std::clamp(std::round(lo / 0.5f * 100.0f), 0.0f, kAutoLevelsReach);
        const float white = std::clamp(std::round((std::min(hi, 1.0f) - 1.0f) / 0.5f * 100.0f),
                                       -kAutoLevelsReach, 0.0f);
        set(r.value, firsts[ch], black < 1.0f ? 0.0f : black);
        set(r.value, static_cast<adjust_param>(static_cast<int>(firsts[ch]) + 2), white > -1.0f ? 0.0f : white);
      }
      break;
    }
    default: break;
  }
  r.changed = r.value != current;
  return r;
}

}  // namespace mv::edit
