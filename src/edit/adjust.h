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
  count
};

inline constexpr int kAdjustSectionCount = static_cast<int>(adjust_section::count);

[[nodiscard]] adjust_section section_of(adjust_param p) noexcept;

struct adjust_range {
  float min = 0.0f;
  float max = 0.0f;
  float step = 0.0f;  // one arrow-key press on the pane's slider
  const char* name = "";
};

[[nodiscard]] adjust_range range_of(adjust_param p) noexcept;

// The folded colour state: the last value set for each parameter.
struct colour {
  std::array<float, kAdjustParamCount> v{};  // 0 = unchanged, for every parameter

  [[nodiscard]] float get(adjust_param p) const noexcept { return v[static_cast<std::size_t>(p)]; }
  [[nodiscard]] bool identity() const noexcept {
    for (const float x : v) {
      if (x != 0.0f) return false;
    }
    return true;
  }
  friend constexpr bool operator==(const colour&, const colour&) = default;
};

// Clamped to the parameter's range; NaN becomes 0.
[[nodiscard]] float clamp_param(adjust_param p, float value) noexcept;

// `c` with every parameter of section `s` at 0.
[[nodiscard]] colour without_section(const colour& c, adjust_section s) noexcept;

[[nodiscard]] colour fold_colour(std::span<const op> ops) noexcept;

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
