// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Adjust pane's Auto buttons, one per section (docs/plans/edit-adjust.md
// slice 1; docs/design/07 "Auto"). Each returns absolute values for its own
// section from the working image, so a second click gives what the first
// gave, and lands as one adjust_all op (one undo step).
//
//   light   #289's auto tone (edit/auto_tone.h): exposure and contrast from
//           the histogram, highlights / shadows / whites / blacks to 0.
//   colour  temperature and tint from the grey world over the least
//           saturated quarter of the pixels that are neither clipped nor
//           near black (so a frame of sea or leaves is not read as a cast),
//           moved most of the way to neutral (kAutoColourDamping), so a
//           sunset stays warm, and less the more colourful those "neutral"
//           pixels still are (neutral_confidence). Saturation and vibrance
//           are kept.
//   mono    black & white at full intensity (kept if already set) with the
//           filter that spreads the grey the most: the variance of
//           grey^(1/2.2) over the filters −100 .. +100 in steps of 25, a
//           near-tie going to the filter nearest 0. Tone is kept.
//   vignette has no Auto.
//
// Worker only (rule 1): each reads at most kAutoSampleBudget pixels.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "core/job_system.h"
#include "core/result.h"
#include "edit/adjust.h"
#include "image/linear.h"

namespace mv::edit {

inline constexpr std::uint64_t kAutoSampleBudget = 1u << 16;
inline constexpr float kAutoColourDamping = 0.7f;  // of the way to neutral
inline constexpr float kAutoColourMax = 60.0f;     // temperature / tint, either way
inline constexpr float kAutoColourMin = 3.0f;      // closer to 0 snaps to 0

struct auto_result {
  colour value;          // `current` with the section replaced
  bool changed = false;  // value != current
};

// The pure halves, for tests: white balance from the working image's mean
// linear RGB, and the filter from samples of the colour before black & white.
// `strength` (0..1) scales the move on top of kAutoColourDamping.
[[nodiscard]] std::array<float, 2> grey_world(const std::array<double, 3>& mean,
                                              float strength = 1.0f) noexcept;
// How far to trust the near-neutral pixels, from their mean saturation:
// 1 at kAutoNeutralSure or less, 0 at kAutoNeutralNone or more (a frame with
// nothing grey in it, all sea and sky, is left nearly alone).
inline constexpr float kAutoNeutralSure = 0.15f;
inline constexpr float kAutoNeutralNone = 0.35f;
[[nodiscard]] float neutral_confidence(float mean_saturation) noexcept;
[[nodiscard]] float best_mono_filter(const std::vector<std::array<float, 3>>& samples) noexcept;

[[nodiscard]] result<auto_result> compute_auto(const image::linear_image& working,
                                               const colour& current, adjust_section section,
                                               const job_context* ctx = nullptr);

}  // namespace mv::edit
