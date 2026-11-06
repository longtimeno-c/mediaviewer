// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit/clip_auto_light.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "edit/auto_tone.h"
#include "edit/clip_look.h"

namespace mv::edit::clip {

namespace {

// A BT.1886-encoded video code -> the sRGB code the canvas shows for it.
std::array<std::uint8_t, 256> shown_codes() {
  std::array<std::uint8_t, 256> t{};
  for (int i = 0; i < 256; ++i) {
    const double lin = std::pow(i / 255.0, 2.4);
    const double s = lin <= 0.0031308 ? 12.92 * lin : 1.055 * std::pow(lin, 1.0 / 2.4) - 0.055;
    t[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(std::lround(std::clamp(s, 0.0, 1.0) * 255.0));
  }
  return t;
}

}  // namespace

histogram displayed_histogram(std::span<const std::uint8_t> rgb, const colour& basis) {
  histogram h;
  const std::size_t n = rgb.size() / 3;
  if (n == 0) return h;
  // At most kHistogramSampleBudget pixels, evenly spread.
  const std::size_t step = std::max<std::size_t>(1, n / kHistogramSampleBudget);
  std::vector<std::uint8_t> px;
  px.reserve((n / step + 1) * 3);
  for (std::size_t i = 0; i < n; i += step) px.insert(px.end(), rgb.begin() + static_cast<std::ptrdiff_t>(i * 3),
                                                      rgb.begin() + static_cast<std::ptrdiff_t>(i * 3 + 3));
  colour_table(basis).apply(px, 3);
  static const std::array<std::uint8_t, 256> shown = shown_codes();
  for (std::size_t i = 0; i + 2 < px.size(); i += 3) {
    const std::uint8_t r = shown[px[i]], g = shown[px[i + 1]], b = shown[px[i + 2]];
    ++h.r[r];
    ++h.g[g];
    ++h.b[b];
    const double lin = 0.2126 * std::pow(px[i] / 255.0, 2.4) + 0.7152 * std::pow(px[i + 1] / 255.0, 2.4) +
                       0.0722 * std::pow(px[i + 2] / 255.0, 2.4);
    const double s = lin <= 0.0031308 ? 12.92 * lin : 1.055 * std::pow(lin, 1.0 / 2.4) - 0.055;
    ++h.luma[static_cast<std::size_t>(std::lround(std::clamp(s, 0.0, 1.0) * 255.0))];
    ++h.samples;
  }
  return h;
}

colour auto_light(std::span<const std::uint8_t> rgb, const colour& current, bool& changed) {
  const auto r = auto_tone(displayed_histogram(rgb, auto_tone_basis(current)), current);
  changed = r.changed;
  return r.value;
}

}  // namespace mv::edit::clip
