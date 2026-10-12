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
// Identity uniforms leave every non-negative input unchanged (to within the
// pow's rounding); negative input clamps to 0.
#pragma once

#include <cmath>

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
inline float3 max(float3 a, float3 b) {
  return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z};
}
inline float3 pow(float3 a, float3 b) {
  return {std::pow(a.x, b.x), std::pow(a.y, b.y), std::pow(a.z, b.z)};
}
constexpr float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// MV_ADJUST_KERNEL itself is in gfx/adjust_kernel_tokens.h (included above),
// which has no C++ in it so the build-time HLSL compile can include it too.

// `inline` is C++'s, outside the shared tokens: HLSL and MSL do not need it.
inline MV_ADJUST_KERNEL

#define MV_ADJUST_STRINGIFY_(...) #__VA_ARGS__
#define MV_ADJUST_STRINGIFY(...) MV_ADJUST_STRINGIFY_(__VA_ARGS__)

// The shader twins' copy: the tokens above, one line.
inline constexpr char kAdjustKernelText[] = MV_ADJUST_STRINGIFY(MV_ADJUST_KERNEL);

}  // namespace mv::gfx::kernel
