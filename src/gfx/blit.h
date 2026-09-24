// SPDX-License-Identifier: GPL-2.0-or-later
// Image blit: linear sample of an 8-bit sRGB texture into the 8-bit sRGB
// swapchain. Filter follows plan/03: anisotropic/trilinear when zoomed out,
// Catmull-Rom between 100 % and 400 %, nearest above 400 %.
#pragma once

#include <span>

#include "core/result.h"
#include "gfx/device.h"

namespace mv::gfx {

struct blit_params {
  float pan_x = 0.0f;
  float pan_y = 0.0f;
  float zoom = 1.0f;
  float window_w = 1.0f;
  float window_h = 1.0f;
  // The image the camera is looking at, in full-resolution pixels.
  float image_w = 1.0f;
  float image_h = 1.0f;
  // The bound texture's own size when it is smaller than the image (a preview
  // or a tiled image's overview stretched over the same rect). 0 = image size.
  float texture_w = 0.0f;
  float texture_h = 0.0f;
  // < 1 blends over what is already in the target (preview → full fade).
  float opacity = 1.0f;
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
  // PR 10 edit geometry, twin of blit_params_mac: output uv -> source uv
  // (edit::placement::map). Single-texture draws only; tiles ignore it.
  float uv_map[6] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
  bool clip_to_source = false;
};

// One tile of a tiled pyramid (image/tiles.h), as the render thread draws it.
// `srv` is a 260x260 two-mip texture whose content starts 2 texels in.
struct tile_quad {
  ID3D11ShaderResourceView* srv = nullptr;
  float origin_x = 0.0f;  // content top-left, in pixels of its level
  float origin_y = 0.0f;
  float scale_x = 1.0f;   // full-resolution pixels per level pixel
  float scale_y = 1.0f;
  float content_w = 256.0f;  // level pixels of real content (edge tiles are smaller)
  float content_h = 256.0f;
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

  // Tiles over whatever `draw` put down, in order (coarse first). Only the
  // tiles' own rects are rasterised. `p` is the same camera as the base draw.
  void draw_tiles(ID3D11DeviceContext* ctx, std::span<const tile_quad> tiles,
                  const blit_params& p) noexcept;

 private:
  void bind_camera(ID3D11DeviceContext* ctx, const blit_params& p, float texture_w,
                   float texture_h) noexcept;

  com_ptr<ID3D11VertexShader> vs_;
  com_ptr<ID3D11PixelShader> ps_;
  com_ptr<ID3D11VertexShader> tile_vs_;
  com_ptr<ID3D11PixelShader> tile_ps_;
  com_ptr<ID3D11Buffer> cb_;
  com_ptr<ID3D11Buffer> tile_cb_;
  com_ptr<ID3D11SamplerState> aniso_;
  com_ptr<ID3D11SamplerState> point_;
  com_ptr<ID3D11BlendState> blend_;
  com_ptr<ID3D11BlendState> blend_alpha_;
  com_ptr<ID3D11RasterizerState> raster_;
};

}  // namespace mv::gfx
