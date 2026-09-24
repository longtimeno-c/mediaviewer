// SPDX-License-Identifier: GPL-2.0-or-later
// PR 11 — the full-resolution export bake (plan/07: "run the full-res chain
// once on export"): geometry, then the colour kernel, over the FP16 working
// image, encoded to 8-bit sRGB.
//
// The canvas runs the same chain on the GPU: the blit samples the working
// texture through `placement::map` and runs MV_ADJUST_KERNEL on the sample
// (gfx/adjust_kernel.h), and the _SRGB render target encodes. Here the
// geometry is PR 10's CPU resampler (edit/geometry.h, now reading linear FP16)
// and the kernel is the same tokens compiled as C++, so for the same pixels
// the bake and the preview differ by 8-bit rounding only.
//
// Worker thread only.
#pragma once

#include "codec/raster.h"
#include "core/job_system.h"
#include "core/result.h"
#include "edit/adjust.h"
#include "edit/edit_stack.h"
#include "image/linear.h"

namespace mv::edit {

// `src` is the image `p` was placed against. The result is `p.output` sized,
// sRGB-encoded RGBA8 with no ICC profile (the working space is Rec.709, so
// what is written is sRGB; tagged_srgb is set), `display_referred`.
[[nodiscard]] result<codec::raster> bake(const image::linear_image& src, const placement& p,
                                         const adjust_uniforms& u,
                                         const job_context* ctx = nullptr);

// One pixel of the chain, for tests and tools: linear in, the 8-bit sRGB
// codes the bake writes out.
void bake_pixel(const float rgb_linear[3], const adjust_uniforms& u, std::uint8_t out[3]) noexcept;

}  // namespace mv::edit
