// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit/auto_tone.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace mv::edit {
namespace {

using bins = std::array<std::uint32_t, 256>;

// The fractional code below which `q` of the samples lie, taking a bin's
// samples as spread evenly across [i - 0.5, i + 0.5].
double percentile(const bins& b, std::uint64_t total, double q) noexcept {
  const double rank = q * static_cast<double>(total);
  double below = 0.0;
  for (std::size_t i = 0; i < b.size(); ++i) {
    const double n = b[i];
    if (n > 0.0 && below + n >= rank) {
      const double v = static_cast<double>(i) - 0.5 + (rank - below) / n;
      return std::clamp(v, 0.0, 255.0);
    }
    below += n;
  }
  return 255.0;
}

std::uint64_t count(const bins& b) noexcept {
  std::uint64_t n = 0;
  for (const std::uint32_t v : b) n += v;
  return n;
}

// sRGB code (0..255, fractional) to linear light.
double linear_of(double code) noexcept {
  const double v = std::clamp(code / 255.0, 0.0, 1.0);
  return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

// log2 of linear light relative to the contrast pivot: the kernel's own
// coordinate, where exposure is a shift and contrast a slope.
double stops_of(double code) noexcept {
  return std::log2(linear_of(code) / static_cast<double>(kContrastPivot));
}

}  // namespace

colour auto_tone_basis(const colour& c) noexcept {
  colour b = c;
  b.v[static_cast<std::size_t>(adjust_param::exposure)] = 0.0f;
  b.v[static_cast<std::size_t>(adjust_param::contrast)] = 0.0f;
  return b;
}

auto_tone_result auto_tone(const histogram& basis, const colour& current) noexcept {
  auto_tone_result r;
  r.value = current;

  // Black and white from the colour channels, so the white target is where
  // the brightest channel lands, not luma (a saturated highlight is not
  // pushed into clipping); the median from luma.
  const bins* channels[3] = {&basis.r, &basis.g, &basis.b};
  double black = 255.0, white = 0.0;
  bool any = false;
  for (const bins* ch : channels) {
    const std::uint64_t n = count(*ch);
    if (n == 0) continue;
    any = true;
    black = std::min(black, percentile(*ch, n, kAutoToneClip));
    white = std::max(white, percentile(*ch, n, 1.0 - kAutoToneClip));
  }
  const std::uint64_t luma_n = count(basis.luma);
  if (!any || luma_n == 0) {
    r.flat = true;
    return r;
  }
  const double median = percentile(basis.luma, luma_n, 0.5);
  r.black = static_cast<float>(black);
  r.median = static_cast<float>(median);
  r.white = static_cast<float>(white);
  if (white - black < kAutoToneMinSpan) {
    r.flat = true;
    return r;
  }

  // The levels target: black → kAutoToneBlackCode, white → kAutoToneWhiteCode,
  // never lowering a white or lifting a black that is already past its target.
  // The stretch is in linear light, where an underexposure is a gain and a
  // haze is an added veil of light: a stretch from black 0 is pure exposure.
  const double white_to = std::max(white, static_cast<double>(kAutoToneWhiteCode));
  const double black_to = std::min(black, static_cast<double>(kAutoToneBlackCode));
  const double lin_black_to = linear_of(black_to);
  const double lin_black_from = linear_of(black);
  const double lin_white = linear_of(white);
  const double lin_median = linear_of(median);
  double lin_median_to = lin_median;
  if (lin_white > lin_black_from) {
    const double gain = (linear_of(white_to) - lin_black_to) / (lin_white - lin_black_from);
    lin_median_to = lin_black_to + (lin_median - lin_black_from) * gain;
  }

  // Two points on z = s * (x + e) in stops about the pivot: (white, white_to)
  // and (median, its target). The slope is the contrast, the shift exposure.
  constexpr double kPivot = kContrastPivot;
  const double x_white = stops_of(white);
  const double z_white = stops_of(white_to);
  double slope = 1.0;
  constexpr double kMinStops = 0.05;  // median and white this close: no slope to read
  if (lin_median > 0.0 && lin_median_to > 0.0) {
    const double span = x_white - std::log2(lin_median / kPivot);
    if (span > kMinStops) slope = (z_white - std::log2(lin_median_to / kPivot)) / span;
  }
  // The black percentile may not end up above where it was, or above its
  // target when it was below it: a greyer black is not "levels". Taking white
  // to its target with black held there needs at least this slope.
  const bool has_black = linear_of(black) > 0.0;
  const double x_black = has_black ? stops_of(black) : 0.0;
  const double z_black_max =
      has_black ? stops_of(std::max(black, static_cast<double>(kAutoToneBlackCode))) : 0.0;
  if (has_black && x_white - x_black > kMinStops) {
    slope = std::max(slope, (z_white - z_black_max) / (x_white - x_black));
  }

  double contrast = slope > 1.0 ? 100.0 * std::log2(slope) / kContrastStops : 0.0;
  contrast = std::round(std::clamp(contrast, 0.0, static_cast<double>(kAutoToneMaxContrast)));
  if (contrast < kAutoToneMinContrast) contrast = 0.0;
  // Exposure from the slope actually applied, so the white target holds
  // whatever the cap and the rounding did...
  const double s = std::exp2(contrast / 100.0 * kContrastStops);
  double exposure = std::round((z_white / s - x_white) * 100.0) / 100.0;
  if (std::abs(exposure) < kAutoToneMinExposure) exposure = 0.0;
  // ...unless that would lift the black (the contrast cap bound): then white
  // falls short instead. Rounded down, so the rounding cannot lift it either.
  if (has_black) {
    exposure = std::min(exposure, std::floor((z_black_max / s - x_black) * 100.0) / 100.0);
  }
  exposure = std::clamp(exposure, -static_cast<double>(kAutoToneMaxExposure),
                        static_cast<double>(kAutoToneMaxExposure));

  r.value.v[static_cast<std::size_t>(adjust_param::exposure)] =
      clamp_param(adjust_param::exposure, static_cast<float>(exposure));
  r.value.v[static_cast<std::size_t>(adjust_param::contrast)] =
      clamp_param(adjust_param::contrast, static_cast<float>(contrast));
  r.changed = r.value != current;
  return r;
}

result<auto_tone_result> compute_auto_tone(const image::linear_image& working,
                                           const colour& current, const job_context* ctx) {
  auto h = compute_histogram(working, uniforms_of(auto_tone_basis(current)), ctx);
  if (!h) return err(h.error());
  return auto_tone(*h, current);
}

}  // namespace mv::edit
