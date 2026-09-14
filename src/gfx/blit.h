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
  // plan/16 view overlays, all in the same draw (no extra pass):
  // 0 canvas, 1 gray, 2 white, 3 checkerboard (the alpha case).
  int background = 0;
  // Display-referred clipping blinkies; they animate on `time_seconds`.
  bool clipping = false;
  float time_seconds = 0.0f;
  // One-pixel grid, drawn only at >= 400 % whatever this says.
  bool pixel_grid = true;
};

// Linear colour the canvas clears to for `background`, so the gutters around
// the image match what the shader draws beside it.
[[nodiscard]] constexpr float background_clear(int background) noexcept {
  switch (background) {
    case 1: return 0.214f;  // sRGB 128
    case 2: return 1.0f;
    case 3: return 0.527f;  // checkerboard mid tone
    default: return 0.018f;
  }
}

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
