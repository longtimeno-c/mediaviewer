// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 11: the colour-adjust kernel's tokens, and nothing else.
//
// Split out of gfx/adjust_kernel.h (which explains the shared-subset rules) so
// that a shader compiler's preprocessor can include it as well as C++: the
// Windows build compiles gfx/blit.hlsl with fxc at build time, and that file
// expands MV_ADJUST_KERNEL from here (issue #163). Keep this file free of C++:
// no namespaces, no #pragma once, no includes.
#ifndef MV_GFX_ADJUST_KERNEL_TOKENS_H
#define MV_GFX_ADJUST_KERNEL_TOKENS_H

// clang-format off
#define MV_ADJUST_KERNEL                                                        \
  float3 mv_adjust(float3 c, float4 a0, float4 a1) {                            \
    float3 zero = float3(0.0f, 0.0f, 0.0f);                                     \
    c = max(c * float3(a0.x, a0.y, a0.z), zero);                                \
    float3 pivot = float3(a1.y, a1.y, a1.y);                                    \
    c = pivot * pow(c / pivot, float3(a0.w, a0.w, a0.w));                       \
    float luma = dot(c, float3(0.2126f, 0.7152f, 0.0722f));                     \
    float3 grey = float3(luma, luma, luma);                                     \
    return max(grey + (c - grey) * a1.x, zero);                                 \
  }
// clang-format on

#endif  // MV_GFX_ADJUST_KERNEL_TOKENS_H
