// SPDX-License-Identifier: GPL-2.0-or-later
// Image blit: linear sample of an 8-bit sRGB texture into the 8-bit sRGB
// swapchain. Filter follows plan/03: anisotropic/trilinear when zoomed out,
// Catmull-Rom between 100 % and 400 %, nearest above 400 %.
#pragma once

#include "core/result.h"
#include "gfx/device.h"

namespace mv::gfx {

struct blit_params {
  float pan_x = 0.0f;
  float pan_y = 0.0f;
  float zoom = 1.0f;
  float window_w = 1.0f;
  float window_h = 1.0f;
  float image_w = 1.0f;
  float image_h = 1.0f;
  // Top-left of the usable canvas in swapchain pixels. The command-bar strip
  // is above this; fit/pan are in this rect, not the full client.
  float origin_x = 0.0f;
  float origin_y = 0.0f;
};

class blitter {
 public:
  blitter() = default;
  ~blitter();

  blitter(const blitter&) = delete;
  blitter& operator=(const blitter&) = delete;

  [[nodiscard]] expected create(ID3D11Device* device);
  void destroy() noexcept;

  void draw(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* image, const blit_params& p) noexcept;

 private:
  com_ptr<ID3D11VertexShader> vs_;
  com_ptr<ID3D11PixelShader> ps_;
  com_ptr<ID3D11Buffer> cb_;
  com_ptr<ID3D11SamplerState> aniso_;
  com_ptr<ID3D11SamplerState> point_;
  com_ptr<ID3D11BlendState> blend_;
  com_ptr<ID3D11RasterizerState> raster_;
};

}  // namespace mv::gfx
