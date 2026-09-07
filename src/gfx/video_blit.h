// SPDX-License-Identifier: GPL-2.0-or-later
// Video blit: NV12/P010 -> RGB using the stream's real colour matrix, plus
// HLG/PQ -> SDR tone-mapping, into the 8-bit sRGB swapchain (D6).
//
// Deliberately a sibling of blit.h rather than a mode of it. The still path
// samples one sRGB texture; this samples two planes, has to honour a matrix,
// primaries, transfer and range that arrive per-clip, and has to tone-map. One
// class doing both would branch on every field of both.
//
// It takes raw SRV pointers and a colour_desc, never a player type: gfx sits
// below player in the module graph and may not include it.
#pragma once

#include "core/result.h"
#include "gfx/colour_desc.h"
#include "gfx/device.h"

namespace mv::gfx {

struct video_blit_params {
  float pan_x = 0.0f;
  float pan_y = 0.0f;
  float zoom = 1.0f;
  float window_w = 1.0f;
  float window_h = 1.0f;
  // The decoded frame's dimensions. Not the same as the texture's: a D3D11VA
  // surface is padded up to the decoder's alignment (commonly a multiple of 16
  // or 32), so sampling the full texture shows garbage down the right and
  // bottom edges. Sample the visible rect, not the allocation.
  float image_w = 1.0f;
  float image_h = 1.0f;
  float texture_w = 1.0f;
  float texture_h = 1.0f;
  // Top-left of the usable canvas in swapchain pixels, as blit_params.
  float origin_x = 0.0f;
  float origin_y = 0.0f;
};

class video_blitter {
 public:
  video_blitter() = default;
  ~video_blitter();

  video_blitter(const video_blitter&) = delete;
  video_blitter& operator=(const video_blitter&) = delete;

  [[nodiscard]] expected create(ID3D11Device* device);
  void destroy() noexcept;

  // `luma` is R8_UNORM (NV12) or R16_UNORM (P010); `chroma` is R8G8_UNORM or
  // R16G16_UNORM. P010 keeps its 10 bits in the HIGH bits of each 16-bit word,
  // so the shader shifts down — without that every value is 64x too large.
  void draw(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* luma,
            ID3D11ShaderResourceView* chroma, const colour_desc& colour,
            const video_blit_params& p) noexcept;

 private:
  com_ptr<ID3D11VertexShader> vs_;
  com_ptr<ID3D11PixelShader> ps_;
  com_ptr<ID3D11Buffer> cb_;
  com_ptr<ID3D11SamplerState> linear_;
  com_ptr<ID3D11SamplerState> point_;
  com_ptr<ID3D11BlendState> blend_;
  com_ptr<ID3D11RasterizerState> raster_;
};

}  // namespace mv::gfx
