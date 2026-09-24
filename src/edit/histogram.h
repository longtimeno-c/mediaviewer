// SPDX-License-Identifier: GPL-2.0-or-later
// PR 11 — the adjust pane's histogram and clipping readout (plan/07:
// "Histogram + clipping warnings", plan/16: "11, on the adjust pane").
//
// A reduction over the preview working image (image/linear.h, ~3072 px on the
// long edge) *after* the colour kernel and the display encode, so it shows
// what the canvas shows. It runs on a worker when the sliders settle — never
// on the UI or render thread (rule 1) — and samples at most ~1 M pixels.
//
// Clipping uses the viewer blinkies' thresholds (adjust.h kClipHighLinear /
// kClipLowLinear), so the pane's percentages count the pixels `C` blinks.
#pragma once

#include <array>
#include <cstdint>

#include "core/job_system.h"
#include "core/result.h"
#include "edit/adjust.h"
#include "image/linear.h"

namespace mv::edit {

struct histogram {
  // Bins are sRGB codes 0..255 of what is displayed: R, G, B and Rec.709 luma.
  std::array<std::uint32_t, 256> r{}, g{}, b{}, luma{};
  std::uint64_t samples = 0;
  std::uint64_t clipped_high = 0;  // any channel at or above kClipHighLinear
  std::uint64_t clipped_low = 0;   // every channel at or below kClipLowLinear

  [[nodiscard]] float high_fraction() const noexcept {
    return samples ? static_cast<float>(clipped_high) / static_cast<float>(samples) : 0.0f;
  }
  [[nodiscard]] float low_fraction() const noexcept {
    return samples ? static_cast<float>(clipped_low) / static_cast<float>(samples) : 0.0f;
  }
};

inline constexpr std::uint64_t kHistogramSampleBudget = 1u << 20;

[[nodiscard]] result<histogram> compute_histogram(const image::linear_image& working,
                                                  const adjust_uniforms& u,
                                                  const job_context* ctx = nullptr);

// What both chromes draw: kHistogramBins bins per channel (R, G, B, luma, in
// that order), each 0..1000 of the tallest bin that is not an end bin (a
// clipped spike at 0 or 255 would flatten everything else; it is shown by
// the clipping readout instead). Pure, so the two panes cannot disagree.
inline constexpr int kHistogramBins = 64;
[[nodiscard]] std::array<std::uint16_t, 4 * kHistogramBins> pack_histogram(const histogram& h) noexcept;

}  // namespace mv::edit
