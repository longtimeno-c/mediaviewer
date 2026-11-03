// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 11 — colour adjusts, the first editing set (docs/design/07 scope line:
// exposure, contrast, saturation, temperature / tint; histogram + clipping),
// and the full Adjust pane's per-pixel half (docs/plans/edit-adjust.md slice 1:
// highlights, shadows, whites, blacks, vibrance, black & white, vignette).
//
// Order is docs/design/07's fixed pipeline: geometry → white balance (temperature,
// tint) → exposure → highlights / shadows / whites / blacks → contrast →
// saturation and vibrance → black & white → vignette → display encode. White
// balance and exposure are one per-channel gain, so the whole chain is one
// per-pixel kernel (gfx/adjust_kernel.h) with six float4 uniforms. On the
// canvas it runs in the blit's pixel shader over the FP16 working texture
// (image/linear.h): a slider drag re-uploads 96 bytes of uniforms and never
// decodes. On export the same kernel runs on the CPU over the full-resolution
// working image (edit/bake.h).
//
// Everything here is pure and platform-neutral (D9).
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "gfx/adjust_kernel.h"

namespace mv::edit {

struct op;
struct placement;

// Appended only: the values are wire ids (the Windows island's commands, the
// Mac bridge's `param`) and index colour::v.
enum class adjust_param : std::uint8_t {
  exposure = 0,  // EV stops, ±5
  contrast,      // −100 .. +100
  saturation,    // −100 (greyscale) .. +100
  temperature,   // −100 (cooler) .. +100 (warmer)
  tint,          // −100 (greener) .. +100 (more magenta)
  // docs/plans/edit-adjust.md slice 1: the rest of Light, Colour, Black &
  // White and Vignette.
  highlights,        // −100 (recover) .. +100
  shadows,           // −100 .. +100 (lift)
  whites,            // −100 .. +100: the top of the range
  blacks,            // −100 (crush) .. +100 (lift the black point)
  vibrance,          // −100 .. +100: saturation that spares colours already strong
  mono,              // 0 (colour) .. 100 (black & white)
  mono_filter,       // −100 (blue filter) .. +100 (red filter)
  mono_tone,         // −100 .. +100: contrast of the grey
  vignette,          // −100 (lighten) .. +100 (darken the edges)
  vignette_radius,   // −100 .. +100 about the default
  vignette_softness, // −100 .. +100 about the default
  // docs/plans/edit-adjust.md slice 2: Levels, for the image (master) and
  // then red, green and blue, in display-encoded light (p = linear^(1/2.2)).
  levels_black,        // 0 .. 100: the input black point, up to p = 0.5
  levels_mid,          // −100 (darker) .. +100 (lighter): the mid-tone gamma
  levels_white,        // −100 .. 0: the input white point, down to p = 0.5
  levels_red_black,
  levels_red_mid,
  levels_red_white,
  levels_green_black,
  levels_green_mid,
  levels_green_white,
  levels_blue_black,
  levels_blue_mid,
  levels_blue_white,
  // Slice 3: Selective Colour, hue (±30°), saturation and luminance for eight
  // bands (kSelectiveBands), each −100 .. +100.
  sel_red_hue,
  sel_red_sat,
  sel_red_lum,
  sel_orange_hue,
  sel_orange_sat,
  sel_orange_lum,
  sel_yellow_hue,
  sel_yellow_sat,
  sel_yellow_lum,
  sel_green_hue,
  sel_green_sat,
  sel_green_lum,
  sel_aqua_hue,
  sel_aqua_sat,
  sel_aqua_lum,
  sel_blue_hue,
  sel_blue_sat,
  sel_blue_lum,
  sel_purple_hue,
  sel_purple_sat,
  sel_purple_lum,
  sel_magenta_hue,
  sel_magenta_sat,
  sel_magenta_lum,
  count
};

inline constexpr int kAdjustParamCount = static_cast<int>(adjust_param::count);

// The Adjust pane's sections, each with its own Auto and reset. Wire values
// too (the Windows island's adjust_auto / adjust_reset_section argument).
enum class adjust_section : std::uint8_t {
  light = 0,  // exposure, highlights, shadows, whites, blacks, contrast
  colour,     // saturation, vibrance, temperature, tint
  mono,       // black & white
  vignette,
  levels,     // slice 2: the twelve levels parameters
  curves,     // slice 2: the four curves (colour::curves), no parameter
  selective,  // slice 3: the 24 selective colour parameters
  red_eye,    // slice 3: the red-eye spots (colour::spots), no parameter
  count
};

// Slice 3: Selective Colour's bands, by their hue in turns on the opponent
// colour wheel the kernel measures (red 0, yellow 1/6, green 1/3, aqua 1/2,
// blue 2/3, magenta 5/6).
inline constexpr int kSelectiveBands = 8;
inline constexpr float kSelectiveCentres[kSelectiveBands] = {
    0.0f, 1.0f / 12.0f, 1.0f / 6.0f, 1.0f / 3.0f, 0.5f, 2.0f / 3.0f, 0.75f, 5.0f / 6.0f};
inline constexpr float kSelectiveHueTurns = 1.0f / 12.0f;  // ±100 rotates ±30°
inline constexpr float kSelectiveLumStops = 1.0f;          // ±100 at full saturation

// Slice 3: a red-eye spot, in the source's uv with its radius a fraction of
// the source's diagonal. At most kRedEyeSpots; one click is one op.
inline constexpr int kRedEyeSpots = 8;
struct red_eye_spot {
  float x = 0.0f, y = 0.0f, r = 0.0f;
  friend constexpr bool operator==(const red_eye_spot&, const red_eye_spot&) = default;
};
inline constexpr float kRedEyeDefaultRadius = 0.012f;

inline constexpr int kAdjustSectionCount = static_cast<int>(adjust_section::count);

[[nodiscard]] adjust_section section_of(adjust_param p) noexcept;

struct adjust_range {
  float min = 0.0f;
  float max = 0.0f;
  float step = 0.0f;  // one arrow-key press on the pane's slider
  const char* name = "";
};

[[nodiscard]] adjust_range range_of(adjust_param p) noexcept;

// A tone curve (slice 2): up to kCurvePoints points in display-encoded light
// (0..1 both ways), x strictly increasing, evaluated as a monotone cubic
// (Fritsch–Carlson) and held flat past the end points. n == 0 is the
// identity, as is any curve whose points all sit on the diagonal.
inline constexpr int kCurvePoints = 8;

enum class curve_channel : std::uint8_t { master = 0, red, green, blue, count };
inline constexpr int kCurveChannels = static_cast<int>(curve_channel::count);

struct curve {
  std::uint8_t n = 0;
  float x[kCurvePoints] = {};
  float y[kCurvePoints] = {};

  [[nodiscard]] bool identity() const noexcept {
    for (int i = 0; i < n; ++i) {
      if (x[i] != y[i]) return false;
    }
    return true;
  }
  friend constexpr bool operator==(const curve&, const curve&) = default;
};

// Points sorted, clamped to 0..1, x at least 1/64 apart (a later duplicate
// dropped), at most kCurvePoints; a curve of one point or fewer is n == 0.
[[nodiscard]] curve sanitise(const curve& c) noexcept;
// The curve at p (0..1).
[[nodiscard]] float evaluate(const curve& c, float p) noexcept;

// The folded colour state: the last value set for each parameter, and the
// four curves.
struct colour {
  std::array<float, kAdjustParamCount> v{};  // 0 = unchanged, for every parameter
  std::array<curve, kCurveChannels> curves{};
  std::array<red_eye_spot, kRedEyeSpots> spots{};
  std::uint8_t spot_count = 0;

  [[nodiscard]] float get(adjust_param p) const noexcept { return v[static_cast<std::size_t>(p)]; }
  [[nodiscard]] const curve& curve_of(curve_channel ch) const noexcept {
    return curves[static_cast<std::size_t>(ch)];
  }
  // No parameter moved and every curve the identity: the kernel is skipped.
  [[nodiscard]] bool identity() const noexcept {
    for (const float x : v) {
      if (x != 0.0f) return false;
    }
    return !has_table() && spot_count == 0;
  }
  // Slice 3: a selective colour band moved.
  [[nodiscard]] bool has_selective() const noexcept;
  // Levels or a curve is set: the kernel reads the table (table_of).
  [[nodiscard]] bool has_table() const noexcept;
  friend constexpr bool operator==(const colour&, const colour&) = default;
};

// Clamped to the parameter's range; NaN becomes 0.
[[nodiscard]] float clamp_param(adjust_param p, float value) noexcept;

// `c` with every parameter of section `s` at 0.
[[nodiscard]] colour without_section(const colour& c, adjust_section s) noexcept;

[[nodiscard]] colour fold_colour(std::span<const op> ops) noexcept;

// The ops that set colour (adjust, adjust_all, curve): kept when a lossless
// turn is written under them.
[[nodiscard]] bool is_colour_op(const op& o) noexcept;

// The kernel's table (gfx::adjust_table): rows 0..127 levels then the curve,
// per channel, after the master channel's levels and curve (row i is
// p = i / 127); rows 128..191 Selective Colour by hue (row 128 + k is hue
// k / 64: x hue shift in turns, y saturation scale, z luminance in stops).
// The red-eye rows are left empty: they need the geometry (place_spots).
[[nodiscard]] gfx::adjust_table table_of(const colour& c) noexcept;

// Slice 3: the red-eye spots into rows 192..199 as the kernel's `pos` sees
// them on the output `p` describes: centre and radius, corner at distance 1.
void place_spots(gfx::adjust_table& t, const colour& c, const placement& p) noexcept;

// White balance (temperature, tint) that makes linear RGB `rgb` neutral: the
// white-balance picker. Exact, within the sliders' ±100.
[[nodiscard]] std::array<float, 2> neutral_balance(const std::array<double, 3>& rgb) noexcept;

// The kernel's uniforms, a0 .. a5 (gfx/adjust_kernel.h). Both shaders take
// them as six float4s in their constant buffers.
using adjust_uniforms = gfx::adjust_vectors;

[[nodiscard]] adjust_uniforms uniforms_of(const colour& c) noexcept;

// The scales behind uniforms_of, public for the tests' written-out chain.
inline constexpr float kWhitesReach = 0.25f;  // ±100 moves the top of p by this
inline constexpr float kBlacksReach = 0.15f;  // ±100 moves the bottom of p by this
inline constexpr float kVignetteStops = 1.5f; // ±100 at the corner
// Black & white filter weights at ±100 (each sums to 1, as Rec.709 does, so a
// neutral grey keeps its value whatever the filter).
inline constexpr std::array<float, 3> kMonoRed{0.60f, 0.35f, 0.05f};
inline constexpr std::array<float, 3> kMonoBlue{0.10f, 0.35f, 0.55f};
[[nodiscard]] std::array<float, 3> mono_weights(float filter) noexcept;

// The contrast slider's scale: ±100 is a slope of 2^(±kContrastStops) in log
// exposure, a power about kContrastPivot in linear light (the kernel's a0.w
// and a1.y). Public so auto tone (edit/auto_tone.h) can invert it.
inline constexpr float kContrastStops = 0.585f;
inline constexpr float kContrastPivot = 0.18f;

// White-balance gains for a temperature / tint pair, normalised so a neutral
// grey keeps its Rec.709 luminance (white balance never brightens). (0, 0)
// is exactly {1, 1, 1}.
[[nodiscard]] std::array<float, 3> white_balance_gains(float temperature, float tint) noexcept;

// Display-referred clipping thresholds, in linear light — the same numbers the
// viewer's `C` blinkies test in both blit shaders, so the pane's percentages
// and the blinking pixels agree. A channel at sRGB 254 or above is a clipped
// highlight; every channel at sRGB 1 or below is a crushed shadow.
inline constexpr float kClipHighLinear = 0.9911f;
inline constexpr float kClipLowLinear = 0.0003f;

}  // namespace mv::edit
