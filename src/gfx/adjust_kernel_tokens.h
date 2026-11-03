// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 11: the colour-adjust kernel's tokens, and nothing else.
//
// Split out of gfx/adjust_kernel.h (which explains the shared-subset rules) so
// that a shader compiler's preprocessor can include it as well as C++: the
// Windows build compiles gfx/blit.hlsl with fxc at build time, and that file
// expands MV_ADJUST_KERNEL from here (issue #163). Keep this file free of C++:
// no namespaces, no #pragma once, no includes.
//
// The full Adjust pane (docs/plans/edit-adjust.md slice 1) adds a2 .. a5 and
// `pos`; each new stage sits behind an `if` on its own uniforms, so a photo
// with only exposure set runs what PR 11 ran, and the export bake pays for a
// stage only when it is set. The stages, in order:
//   gains           a0.xyz (white balance * exposure)
//   tone            a1.z > 0.5: highlights a2.x, shadows a2.y, whites a2.z,
//                   blacks a2.w — a curve on p = luma^(1/2.2), applied as a
//                   ratio so hue is kept, the rest added as grey at black
//   contrast        a0.w about the pivot a1.y
//   saturation      a1.x, times vibrance (1 + a3.x * (1 - s)), s = the
//                   pixel's own saturation, so muted colours move most
//   black & white   a3.y > 0: a mix of grey dot(c, a4.xyz) at a3.y, with a
//                   contrast slope a3.z on that grey about the pivot
//   vignette        a5.x != 0: a5.x stops (+ darkens) from a5.y to a5.z of
//                   the distance to the corner; pos.xy is the pixel's offset
//                   from the centre, scaled so a corner is at distance 1
#ifndef MV_GFX_ADJUST_KERNEL_TOKENS_H
#define MV_GFX_ADJUST_KERNEL_TOKENS_H

// clang-format off
#define MV_ADJUST_KERNEL                                                        \
  float3 mv_adjust(float3 c, float4 pos, float4 a0, float4 a1, float4 a2,       \
                   float4 a3, float4 a4, float4 a5) {                           \
    float3 zero = float3(0.0f, 0.0f, 0.0f);                                     \
    float3 w = float3(0.2126f, 0.7152f, 0.0722f);                               \
    c = max(c * float3(a0.x, a0.y, a0.z), zero);                                \
    if (a1.z > 0.5f) {                                                          \
      float l = max(dot(c, w), 0.0f);                                           \
      float p = pow(l, 0.45454545f);                                            \
      float q = min(p, 1.0f);                                                   \
      float r = 1.0f - q;                                                       \
      float t = p + a2.x * q * q * q * r + a2.y * q * r * r * r                 \
                + a2.z * q * q * q + a2.w * r * r * r;                          \
      float lt = pow(max(t, 0.0f), 2.2f);                                       \
      float k = lt / max(l, 0.0001f);                                           \
      float rest = lt - l * k;                                                  \
      c = max(c * k + float3(rest, rest, rest), zero);                          \
    }                                                                           \
    float3 pivot = float3(a1.y, a1.y, a1.y);                                    \
    c = pivot * pow(c / pivot, float3(a0.w, a0.w, a0.w));                       \
    float luma = dot(c, w);                                                     \
    float3 grey = float3(luma, luma, luma);                                     \
    float sat = a1.x;                                                           \
    if (a3.x != 0.0f) {                                                         \
      float hi = max(c.x, max(c.y, c.z));                                       \
      float lo = min(c.x, min(c.y, c.z));                                       \
      float s = (hi - lo) / max(hi, 0.0001f);                                   \
      sat = sat * max(1.0f + a3.x * (1.0f - s), 0.0f);                          \
    }                                                                           \
    c = max(grey + (c - grey) * sat, zero);                                     \
    if (a3.y > 0.0f) {                                                          \
      float m = max(dot(c, float3(a4.x, a4.y, a4.z)), 0.0f);                    \
      m = a1.y * pow(m / a1.y, a3.z);                                           \
      c = c + (float3(m, m, m) - c) * a3.y;                                     \
    }                                                                           \
    if (a5.x != 0.0f) {                                                         \
      float d = sqrt(pos.x * pos.x + pos.y * pos.y);                            \
      float f = max(min((d - a5.y) / (a5.z - a5.y), 1.0f), 0.0f);               \
      f = f * f * (3.0f - 2.0f * f);                                            \
      c = c * pow(2.0f, 0.0f - a5.x * f);                                       \
    }                                                                           \
    return c;                                                                   \
  }
// clang-format on

#endif  // MV_GFX_ADJUST_KERNEL_TOKENS_H
