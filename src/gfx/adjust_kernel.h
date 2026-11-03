// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 11 — the colour-adjust kernel, written once.
//
// docs/design/10 PR 11: "HLSL and MSL twins that disagree fail the PR." Rather than
// three hand-kept copies (HLSL, MSL, the C++ export bake) that a test can only
// sample, the kernel is one token sequence, MV_ADJUST_KERNEL, in the subset of
// syntax HLSL, the Metal Shading Language and C++ share:
//
//   * float3 / float4 constructors with every component spelled out (HLSL has
//     no one-argument float3(x) splat);
//   * component access as .x .y .z .w — no swizzles (C++ has none);
//   * max, pow, dot, and + - * / on vectors and vector * scalar.
//
// C++ compiles it against the small float3 / float4 below (mv::gfx::kernel).
// It lives in gfx/ because both blits include it and gfx/ sits below edit/;
// it is platform-neutral and names no GPU API (D9).
// gfx/blit_metal.mm pastes kAdjustKernelText — the same tokens, stringified by
// the preprocessor — into its shader source; gfx/blit.hlsl expands the macro
// itself through fxc's preprocessor when the build compiles it (issue #163). The
// twins are therefore the same text by construction; tests/test_adjust.cpp
// pins that text and checks it contains no construct outside the subset.
//
// The kernel works on straight (not premultiplied) linear Rec.709 light.
//   a0.xyz  white balance * exposure gains (edit/adjust.h uniforms_of)
//   a0.w    contrast slope: a power about the pivot in linear light, i.e. a
//           straight line of slope a0.w through the pivot in log exposure
//   a1.x    saturation: 1 = unchanged, 0 = Rec.709 luma
//   a1.y    contrast pivot, 18 % grey
//   a1.z    > 0.5 runs the tone stage (a2); a1.w is the blits' on switch
//   a2..a5  tone, vibrance, black & white, vignette, table on (adjust_kernel_tokens.h)
//   lut     the levels and curves table (adjust_table), read when a5.w > 0.5
//   pos     the pixel's offset from the output's centre, corner at distance 1
//           (vignette_map); only the vignette reads it
// Identity uniforms leave every non-negative input unchanged (to within the
// pow's rounding); negative input clamps to 0.
#pragma once

#include <cmath>
#include <cstdint>

#include "gfx/adjust_kernel_tokens.h"

namespace mv::gfx::kernel {

struct float3 {
  float x = 0, y = 0, z = 0;
  constexpr float3() = default;
  constexpr float3(float a, float b, float c) : x(a), y(b), z(c) {}
};

struct float4 {
  float x = 0, y = 0, z = 0, w = 0;
  constexpr float4() = default;
  constexpr float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
};

constexpr float3 operator+(float3 a, float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr float3 operator-(float3 a, float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr float3 operator*(float3 a, float3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
constexpr float3 operator/(float3 a, float3 b) { return {a.x / b.x, a.y / b.y, a.z / b.z}; }
constexpr float3 operator*(float3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
constexpr float4 operator+(float4 a, float4 b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
constexpr float4 operator-(float4 a, float4 b) { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
constexpr float4 operator*(float4 a, float k) { return {a.x * k, a.y * k, a.z * k, a.w * k}; }
inline float3 max(float3 a, float3 b) {
  return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z};
}
inline float3 pow(float3 a, float3 b) {
  return {std::pow(a.x, b.x), std::pow(a.y, b.y), std::pow(a.z, b.z)};
}
constexpr float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float max(float a, float b) { return a > b ? a : b; }
inline float min(float a, float b) { return a < b ? a : b; }
inline float pow(float a, float b) { return std::pow(a, b); }
inline float sqrt(float a) { return std::sqrt(a); }
inline float cos(float a) { return std::cos(a); }
inline float sin(float a) { return std::sin(a); }
inline float atan2(float y, float x) { return std::atan2(y, x); }
inline float floor(float a) { return std::floor(a); }

// MV_ADJUST_KERNEL itself is in gfx/adjust_kernel_tokens.h (included above),
// which has no C++ in it so the build-time HLSL compile can include it too.

// `inline` is C++'s, outside the shared tokens: HLSL and MSL do not need it.
// Forced: the export bake and the histogram call it once a pixel, and out of
// line its eight float4 arguments go through the stack (+12 % on a 24 MP
// bake, measured).
#if defined(_MSC_VER)
#define MV_ADJUST_INLINE __forceinline
#else
#define MV_ADJUST_INLINE inline __attribute__((always_inline))
#endif
#define MV_ADJUST_LUT_PARAM const float4* lut,
MV_ADJUST_INLINE MV_ADJUST_KERNEL
// Undefined again before the text is made, so the shaders' copy keeps the
// name and each shader defines it its own way.
#undef MV_ADJUST_LUT_PARAM

#define MV_ADJUST_STRINGIFY_(...) #__VA_ARGS__
#define MV_ADJUST_STRINGIFY(...) MV_ADJUST_STRINGIFY_(__VA_ARGS__)

// The shader twins' copy: the tokens above, one line.
inline constexpr char kAdjustKernelText[] = MV_ADJUST_STRINGIFY(MV_ADJUST_KERNEL);

}  // namespace mv::gfx::kernel

namespace mv::gfx {

// The kernel's uniforms, a0 .. a5, as both blits' constant buffers carry them
// (edit::adjust_uniforms is this type). The defaults are the identity.
inline constexpr int kAdjustVectors = 6;
inline constexpr float kVignetteInner = 0.35f;  // falloff starts here (radius 0)
inline constexpr float kVignetteWidth = 0.6f;   // and is this wide (softness 0)

// The levels and curves table (slice 2): row i is the output for
// p = i / 127, red, green and blue in x, y, z (w unused). The identity is
// the diagonal. Both blits bind it apart from the uniforms and upload it only
// when it changes.
// Slice 3 appends 64 hue rows (selective colour: hue shift in turns,
// saturation scale, luminance in stops) and 8 red-eye spots (x, y, radius in
// `pos` units; radius 0 is no spot).
inline constexpr int kAdjustTableRows = 128;
inline constexpr int kAdjustHueRows = 64;
inline constexpr int kAdjustSpotRows = 8;
inline constexpr int kAdjustHueFirst = kAdjustTableRows;
inline constexpr int kAdjustSpotFirst = kAdjustTableRows + kAdjustHueRows;
inline constexpr int kAdjustTableSize = kAdjustSpotFirst + kAdjustSpotRows;

struct adjust_table {
  float r[kAdjustTableSize][4];
  adjust_table() noexcept {
    for (int i = 0; i < kAdjustTableRows; ++i) {
      const float p = static_cast<float>(i) / static_cast<float>(kAdjustTableRows - 1);
      r[i][0] = r[i][1] = r[i][2] = p;
      r[i][3] = 0.0f;
    }
    for (int i = kAdjustHueFirst; i < kAdjustSpotFirst; ++i) {
      r[i][0] = 0.0f;
      r[i][1] = 1.0f;
      r[i][2] = r[i][3] = 0.0f;
    }
    for (int i = kAdjustSpotFirst; i < kAdjustTableSize; ++i) r[i][0] = r[i][1] = r[i][2] = r[i][3] = 0.0f;
  }
  friend constexpr bool operator==(const adjust_table&, const adjust_table&) = default;
};

struct adjust_vectors {
  float a[kAdjustVectors][4] = {
      {1.0f, 1.0f, 1.0f, 1.0f},                  // gains; contrast slope
      {1.0f, 0.18f, 0.0f, 0.0f},                 // saturation; pivot; tone on; (blit on)
      {0.0f, 0.0f, 0.0f, 0.0f},                  // highlights, shadows, whites, blacks
      {0.0f, 0.0f, 1.0f, 0.0f},                  // vibrance; mono mix; mono slope; red-eye on
      {0.2126f, 0.7152f, 0.0722f, 0.0f},         // mono weights; selective on
      {0.0f, kVignetteInner, kVignetteInner + kVignetteWidth, 0.0f},  // vignette stops, from, to; table on
  };
  friend constexpr bool operator==(const adjust_vectors&, const adjust_vectors&) = default;
};

// The kernel reads the table: levels or curves, selective colour or red-eye.
[[nodiscard]] inline bool reads_table(const adjust_vectors& u) noexcept {
  return u.a[5][3] > 0.5f || u.a[4][3] > 0.5f || u.a[3][3] > 0.5f;
}

// The kernel's `pos` over a w × h output: built once per image, then one
// multiply-add a pixel. at(x, y) takes the pixel's index; its centre is used.
struct vignette_map {
  float cx = 0.0f, cy = 0.0f, k = 0.0f;
  vignette_map(float w, float h) noexcept
      : cx(0.5f * w - 0.5f), cy(0.5f * h - 0.5f), k(2.0f / std::sqrt(w * w + h * h)) {}
  [[nodiscard]] kernel::float4 at(std::uint32_t x, std::uint32_t y) const noexcept {
    return {(static_cast<float>(x) - cx) * k, (static_cast<float>(y) - cy) * k, 0.0f, 0.0f};
  }
};

// The uniforms as the kernel's own vectors, built once per image: a loop
// that writes bytes cannot keep reloading them from `adjust_vectors` (a
// byte store may alias it), so the CPU users hold one of these in a local.
// The table rides along (by pointer, it is 2 KB); null is the identity.
struct adjust_args {
  kernel::float4 v[kAdjustVectors];
  kernel::float4 lut[kAdjustTableSize];
  explicit adjust_args(const adjust_vectors& u, const adjust_table* t = nullptr) noexcept {
    for (int i = 0; i < kAdjustVectors; ++i) v[i] = {u.a[i][0], u.a[i][1], u.a[i][2], u.a[i][3]};
    const adjust_table id;
    const adjust_table& src = t != nullptr ? *t : id;
    for (int i = 0; i < kAdjustTableSize; ++i) {
      lut[i] = {src.r[i][0], src.r[i][1], src.r[i][2], src.r[i][3]};
    }
  }
};

// The kernel on the CPU (the export bake, the histogram, tests).
[[nodiscard]] inline kernel::float3 adjust(kernel::float3 c, kernel::float4 pos,
                                           const adjust_args& a) noexcept {
  return kernel::mv_adjust(a.lut, c, pos, a.v[0], a.v[1], a.v[2], a.v[3], a.v[4], a.v[5]);
}
[[nodiscard]] inline kernel::float3 adjust(kernel::float3 c, kernel::float4 pos,
                                           const adjust_vectors& u,
                                           const adjust_table* t = nullptr) noexcept {
  return adjust(c, pos, adjust_args(u, t));
}

}  // namespace mv::gfx
