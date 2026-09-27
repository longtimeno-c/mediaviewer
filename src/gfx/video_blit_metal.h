// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Video blit, Metal twin of gfx/video_blit.cpp's HLSL (plan/15 "Shaders":
// hand-written HLSL/MSL twins, same algorithm). NV12/P010 -> RGB with the
// stream's real matrix, range and transfer, plus HLG/PQ -> SDR tone-mapping,
// into the 8-bit sRGB drawable (D6). Takes raw MTLTexture pointers and a
// colour_desc, never a player type: gfx sits below player.
#pragma once

#include <cstdint>

#include "core/result.h"
#include "gfx/colour_desc.h"

namespace mv::gfx {

struct video_blit_params_mac {
  float pan_x = 0.0f;
  float pan_y = 0.0f;
  float zoom = 1.0f;
  float window_w = 1.0f;
  float window_h = 1.0f;
  // The VISIBLE frame; the luma texture may be larger (decoder alignment).
  float image_w = 1.0f;
  float image_h = 1.0f;
  float texture_w = 1.0f;
  float texture_h = 1.0f;
  float origin_x = 0.0f;
  float origin_y = 0.0f;
};

class video_blitter_mac {
 public:
  video_blitter_mac() = default;
  ~video_blitter_mac();

  video_blitter_mac(const video_blitter_mac&) = delete;
  video_blitter_mac& operator=(const video_blitter_mac&) = delete;

  // `mtl_device` is id<MTLDevice>; `pixel_format` the drawable's MTLPixelFormat.
  [[nodiscard]] expected create(void* mtl_device, std::uint64_t pixel_format) noexcept;
  void destroy() noexcept;

  // `encoder` is id<MTLRenderCommandEncoder>; `luma` is R8Unorm (NV12) or
  // R16Unorm (P010), `chroma` RG8Unorm / RG16Unorm (both id<MTLTexture>). P010
  // keeps its 10 bits in the HIGH bits of each 16-bit word; the shader rescales.
  void draw(void* encoder, void* luma, void* chroma, const colour_desc& colour,
            const video_blit_params_mac& p) noexcept;

 private:
  void* pipeline_ = nullptr;  // id<MTLRenderPipelineState>
  void* sampler_ = nullptr;   // id<MTLSamplerState>
};

}  // namespace mv::gfx
