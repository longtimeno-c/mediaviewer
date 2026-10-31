// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #289 — Auto tone (Preview's "Auto Levels"): one click sets exposure and
// contrast from the histogram (docs/design/07, "Auto tone").
//
// There is no levels op. The colour chain has two tone controls, a gain
// (exposure) and a power about 18 % grey (contrast), so auto tone is a
// levels stretch fitted to those two and no new GPU op:
//
//   1. Percentiles of the histogram (fractional sRGB codes, interpolated
//      inside the bin): black and white at kAutoToneClip from each end, the
//      darkest and brightest of R, G and B (so a saturated highlight is not
//      pushed into clipping by a luma that sits lower), and the luma median.
//   2. The levels target is the stretch, in linear light, that takes black to
//      kAutoToneBlackCode and white to kAutoToneWhiteCode. In linear light an
//      underexposure is a gain and a haze an added veil, so a stretch from a
//      black at 0 is pure exposure. White is never lowered (a white already
//      above the target, a clipped sky, stays where it is) and a black already
//      below its target is left there, so a full-range photo is its own
//      target: no change.
//   3. Exposure and contrast are the curve that puts the white percentile on
//      the white target and the median where the stretch puts it, solved in
//      the kernel's own log-exposure terms about the pivot (two points, two
//      unknowns), with the slope raised if needed so the black percentile
//      does not end up above where it was (or above its target).
//   4. Caps, so a camera JPEG's own tone curve is only nudged: contrast
//      0 .. +kAutoToneMaxContrast (auto tone never flattens), exposure
//      ±kAutoToneMaxExposure. When the contrast cap binds, the black is held
//      and white falls short of its target: a power about 18 % grey cannot
//      subtract an offset, so a hazy black comes down only as far as +40
//      allows. That is the limit of the existing controls, stated, not hidden;
//      a levels or curve op would be a new op.
//
// The values are absolute, computed from the histogram of the image with
// exposure and contrast at 0 (auto_tone_basis): temperature, tint and
// saturation stay as the user set them, and a second click gives the same
// values as the first (idempotent by construction, not by convergence).
//
// Pure, deterministic and histogram-sized (a few walks over 256 bins, ~2 µs);
// compute_auto_tone adds the histogram reduction, which is a worker job
// (rule 1), like the pane's own histogram.
#pragma once

#include "core/job_system.h"
#include "core/result.h"
#include "edit/adjust.h"
#include "edit/histogram.h"
#include "image/linear.h"

namespace mv::edit {

inline constexpr float kAutoToneClip = 0.001f;        // 0.1 % at each end (Photoshop's default)
inline constexpr float kAutoToneBlackCode = 8.0f;     // sRGB code the black percentile aims at
inline constexpr float kAutoToneWhiteCode = 250.0f;   // under the blinkies' 254 (adjust.h)
inline constexpr float kAutoToneMinSpan = 8.0f;       // black..white codes; less is a flat frame
inline constexpr float kAutoToneMaxExposure = 2.0f;   // EV, either way
inline constexpr float kAutoToneMaxContrast = 40.0f;  // slider units, slope ×1.18
// Results closer to 0 than these snap to 0, so a balanced photo is exactly
// untouched rather than nudged by a rounding.
inline constexpr float kAutoToneMinExposure = 0.05f;
inline constexpr float kAutoToneMinContrast = 2.0f;

// `c` with exposure and contrast at 0: the colour the histogram auto_tone
// reads must be computed with.
[[nodiscard]] colour auto_tone_basis(const colour& c) noexcept;

struct auto_tone_result {
  colour value;          // `current` with exposure and contrast replaced
  bool changed = false;  // value != current
  // What was read, in fractional sRGB codes: black and white of R, G, B,
  // the median of luma. For tests and the log.
  float black = 0.0f, median = 0.0f, white = 0.0f;
  bool flat = false;     // empty, single-level or too narrow to stretch: no-op
};

// `basis` is a histogram computed with uniforms_of(auto_tone_basis(current)).
[[nodiscard]] auto_tone_result auto_tone(const histogram& basis, const colour& current) noexcept;

// Worker only: the basis histogram of `working` (~1 M samples), then
// auto_tone. Cancelled when `ctx` is.
[[nodiscard]] result<auto_tone_result> compute_auto_tone(const image::linear_image& working,
                                                         const colour& current,
                                                         const job_context* ctx = nullptr);

}  // namespace mv::edit
