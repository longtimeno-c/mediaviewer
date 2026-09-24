// SPDX-License-Identifier: GPL-2.0-or-later
// Image blit, Metal twin of gfx/blit.cpp's HLSL (plan/15 "Shaders": hand-written
// HLSL/MSL twins, same algorithm, register binding documented in a comment).
// Single-texture path only — the tiled-pyramid path (vs_tile/ps_tile) is PR 4/7
// territory and stays D3D11-only until Milestone F reaches image tiling.
#pragma once

#include <cstdint>

#include "core/result.h"

namespace mv::gfx {

struct blit_params_mac {
  float pan_x = 0.0f;
  float pan_y = 0.0f;
  float zoom = 1.0f;
  float window_w = 1.0f;
  float window_h = 1.0f;
  float image_w = 1.0f;
  float image_h = 1.0f;
  // The bound texture's own size when smaller than the image. 0 = image size.
  float texture_w = 0.0f;
  float texture_h = 0.0f;
  float opacity = 1.0f;
  float origin_x = 0.0f;
  float origin_y = 0.0f;
  // 0 canvas, 1 gray, 2 white, 3 checkerboard — same encoding as blit_params.
  int background = 0;
  bool clipping = false;
  float time_seconds = 0.0f;
  bool pixel_grid = true;
  // PR 10 edit geometry: output uv -> source uv (edit::placement::map). With
  // a map, image_w/h are the *edited* size and texture_w/h must be the
  // texture's own. `clip_to_source` shows the background where a sample
  // falls outside the source (crop mode's straightened frame).
  float uv_map[6] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
  bool clip_to_source = false;
};

// Linear clear colour for `background` — same values as gfx::background_clear.
[[nodiscard]] constexpr float background_clear_mac(int background) noexcept {
  switch (background) {
    case 1: return 0.214f;
    case 2: return 1.0f;
    case 3: return 0.527f;
    default: return 0.018f;
  }
}

class blitter_mac {
 public:
  blitter_mac() = default;
  ~blitter_mac();

  blitter_mac(const blitter_mac&) = delete;
  blitter_mac& operator=(const blitter_mac&) = delete;

  // `mtl_device` is id<MTLDevice>, `pixel_format` is MTLPixelFormat (the
  // drawable's — MTLPixelFormatBGRA8Unorm_sRGB in the lab, matching main_mac.mm).
  [[nodiscard]] expected create(void* mtl_device, std::uint64_t pixel_format) noexcept;
  void destroy() noexcept;

  // `encoder` is id<MTLRenderCommandEncoder>, `texture` is id<MTLTexture>.
  void draw(void* encoder, void* texture, const blit_params_mac& p) noexcept;

 private:
  void* pipeline_opaque_ = nullptr;   // id<MTLRenderPipelineState>
  void* pipeline_blend_ = nullptr;    // id<MTLRenderPipelineState>
  void* sampler_aniso_ = nullptr;     // id<MTLSamplerState>
  void* sampler_point_ = nullptr;     // id<MTLSamplerState>
};

}  // namespace mv::gfx
