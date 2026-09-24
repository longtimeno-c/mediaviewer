// SPDX-License-Identifier: GPL-2.0-or-later
// PR 11 — colour adjusts, the first editing set (plan/07 scope line:
// exposure, contrast, saturation, temperature / tint; histogram + clipping).
//
// Order is plan/07's fixed pipeline: geometry → white balance (temperature,
// tint) → exposure → contrast → saturation → display encode. White balance
// and exposure are one per-channel gain, so the whole chain is one small
// per-pixel kernel (gfx/adjust_kernel.h) with eight uniforms. On the canvas
// it runs in the blit's pixel shader over the FP16 working texture
// (image/linear.h): a slider drag re-uploads 32 bytes of uniforms and never
// decodes. On export the same kernel runs on the CPU over the full-resolution
// working image (edit/bake.h).
//
// Everything here is pure and platform-neutral (D9).
#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace mv::edit {

struct op;

enum class adjust_param : std::uint8_t {
  exposure = 0,  // EV stops, ±5
  contrast,      // −100 .. +100
  saturation,    // −100 (greyscale) .. +100
  temperature,   // −100 (cooler) .. +100 (warmer)
  tint,          // −100 (greener) .. +100 (more magenta)
  count
};

inline constexpr int kAdjustParamCount = static_cast<int>(adjust_param::count);

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

[[nodiscard]] colour fold_colour(std::span<const op> ops) noexcept;

// The kernel's eight uniforms, laid out as its a0 / a1 (adjust_kernel.h).
// Both shaders take them as two float4s in their constant buffers.
struct adjust_uniforms {
  float a0[4] = {1.0f, 1.0f, 1.0f, 1.0f};   // gains r, g, b; contrast slope
  float a1[4] = {1.0f, 0.18f, 0.0f, 0.0f};  // saturation; pivot; unused, unused
  friend constexpr bool operator==(const adjust_uniforms&, const adjust_uniforms&) = default;
};

[[nodiscard]] adjust_uniforms uniforms_of(const colour& c) noexcept;

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
