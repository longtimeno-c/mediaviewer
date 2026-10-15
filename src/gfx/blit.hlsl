// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The photo blit (gfx/blit.cpp). Compiled at build time by fxc into byte-array
// headers, one per entry point (CMakeLists.txt, mv_fxc; issue #163), so a device
// create never runs D3DCompile. MSL twin: gfx/blit_metal.mm.
//
// The PR 11 colour kernel comes from gfx/adjust_kernel_tokens.h through fxc's
// preprocessor: the same tokens the MSL twin and the CPU export bake compile.
cbuffer Camera : register(b0) {
  float2 pan;
  float zoom;
  float opacity;
  float2 window_size;
  float2 image_size;
  float2 origin;
  float2 texture_size;
  float unused0;  // the retired background mode (issue #408)
  float clipping;
  float time;
  float grid;
  // PR 10 geometry, twin of blit_metal.mm: output uv -> source uv. map0.w >
  // 0.5 shows the background outside the source. Identity = unchanged.
  float4 map0;
  float4 map1;
  // The inverse map (source uv -> output uv) and the source's full-resolution
  // size, for the tile path: tiles live in the source's pixels.
  float4 inv0;
  float4 inv1;
  float4 source_size;  // xy
  // PR 11 colour adjust (edit::adjust_uniforms a0 / a1). adj1.w > 0.5 runs
  // mv_adjust on every sample; the kernel itself never reads adj1.w.
  float4 adj0;
  float4 adj1;
  float4 theme_background;  // the theme's canvas, linear RGB
};

#include "adjust_kernel_tokens.h"
MV_ADJUST_KERNEL
Texture2D img : register(t0);
SamplerState samp_aniso : register(s0);
SamplerState samp_point : register(s1);

struct VSOut { float4 pos : SV_Position; };

VSOut vs_main(uint id : SV_VertexID) {
  VSOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return o;
}

// One tile's content rect, clipped to the image. Adjacent tiles compute a
// shared edge from the same expression, so the rasteriser's fill rule leaves
// neither a crack nor a double-blended seam.
// PR 10: the tile's corners are source pixels; they reach the screen through
// the inverse edit map, so an edited (turned, cropped, straightened) tiled
// image still draws its tiles. The map is affine, so the shared-edge rule
// above survives it: both tiles map the same edge through the same matrix.
// Issue #164: a tile's record is per-instance vertex data, every tile's record
// in one buffer written once per frame; instance i of DrawInstanced(6, 1, 0, i)
// reads record i. rec0 = content origin (level px) and full-resolution px per
// level px; rec1 = content size (level px), border and texture size (texels).
struct TileOut {
  float4 pos : SV_Position;
  nointerpolation float4 rec0 : TEXCOORD0;
  nointerpolation float2 rec1 : TEXCOORD1;  // border, texture size
};

TileOut vs_tile(uint id : SV_VertexID, float4 rec0 : TILE0, float4 rec1 : TILE1) {
  static const float2 corners[6] = {
    float2(0, 0), float2(1, 0), float2(0, 1),
    float2(0, 1), float2(1, 0), float2(1, 1)
  };
  TileOut o;
  o.rec0 = rec0;
  o.rec1 = rec1.zw;
  float2 tile_origin = rec0.xy;
  float2 tile_scale = rec0.zw;
  float2 tile_content = rec1.xy;
  float2 source_px = min((tile_origin + corners[id] * tile_content) * tile_scale, source_size.xy);
  float3 s = float3(source_px / source_size.xy, 1.0);
  float2 image_px = float2(dot(inv0.xyz, s), dot(inv1.xyz, s)) * image_size;
  float2 rel = window_size * 0.5 + (image_px - pan) * zoom;
  float2 ndc = rel / window_size * 2.0 - 1.0;
  o.pos = float4(ndc.x, -ndc.y, 0.0, 1.0);
  return o;
}

float4 catmull_rom_1d(float4 a, float4 b, float4 c, float4 d, float t) {
  float t2 = t * t;
  float t3 = t2 * t;
  return 0.5 * ((2.0 * b) +
                (-a + c) * t +
                (2.0 * a - 5.0 * b + 4.0 * c - d) * t2 +
                (-a + 3.0 * b - 3.0 * c + d) * t3);
}

// Point-sample at texel centres through the _SRGB view. Conversion is on the
// view, not the sampler; Load was returning 0 outside the image (dark halo).
float4 texel(Texture2D tex, int2 p, float2 tex_size) {
  return tex.SampleLevel(samp_point, (float2(p) + 0.5) / tex_size, 0);
}

float4 sample_catmull(Texture2D tex, float2 uv, float2 tex_size) {
  float2 pos = uv * tex_size - 0.5;
  float2 f = frac(pos);
  int2 i = int2(floor(pos));
  float4 rows[4];
  [unroll] for (int y = -1; y <= 2; ++y) {
    float4 c0 = texel(tex, i + int2(-1, y), tex_size);
    float4 c1 = texel(tex, i + int2( 0, y), tex_size);
    float4 c2 = texel(tex, i + int2( 1, y), tex_size);
    float4 c3 = texel(tex, i + int2( 2, y), tex_size);
    rows[y + 1] = catmull_rom_1d(c0, c1, c2, c3, f.x);
  }
  // Catmull-Rom rings. Unclamped overshoot encodes per-channel on the sRGB
  // RTV and reads as magenta / cyan fringes along edges ("pink shapes").
  return saturate(catmull_rom_1d(rows[0], rows[1], rows[2], rows[3], f.y));
}

// Linear values: the render target view is _SRGB and encodes on write.
// Issue #408: around the picture and through its alpha is the theme's canvas,
// the colour the chrome paints; there is no canvas background of its own.
float3 background_at(float2 screen) {
  return theme_background.rgb;
}

float4 sample_filtered(float2 uv, float2 tex_size) {
  if (zoom >= 4.0) return img.SampleLevel(samp_point, uv, 0);
  if (zoom >= 1.0) return sample_catmull(img, uv, tex_size);
  return img.Sample(samp_aniso, uv);
}

float2 image_px_at(float2 screen) {
  return pan + (screen - (origin + window_size * 0.5)) / zoom;
}

float4 finish(float4 c, float2 screen, float2 image_px) {
  // PR 11: the edit's colour, in linear light, before anything is composited.
  if (adj1.w > 0.5) c.rgb = mv_adjust(c.rgb, adj0, adj1);
  float3 bg = background_at(screen);
  float3 rgb = lerp(bg, c.rgb, saturate(c.a));

  // Display-referred blinkies (docs/design/16 `C`): a channel at sRGB 254+ is a
  // clipped highlight, every channel at sRGB 1 or below is a crushed shadow.
  // Same thresholds as edit::kClipHighLinear / kClipLowLinear (the adjust
  // pane's readout). They test the adjusted colour, so on an edited RAW they
  // read LibRaw's linear develop through the working texture (PR 11).
  if (clipping > 0.5 && frac(time * 2.0) < 0.5) {
    float hi = max(c.r, max(c.g, c.b));
    if (hi >= 0.9911) rgb = float3(1.0, 0.0, 0.0);
    else if (hi <= 0.0003) rgb = float3(0.0, 0.25, 1.0);
  }

  // Pixel grid at >= 400 %: darken the first screen pixel of each image pixel.
  if (grid > 0.5 && zoom >= 4.0) {
    float2 f = frac(image_px);
    float edge = 1.0 / zoom;
    if (f.x < edge || f.y < edge) rgb *= 0.6;
  }
  return float4(rgb, opacity);
}

float4 ps_main(VSOut vin) : SV_Target {
  float2 image_px = image_px_at(vin.pos.xy);
  float2 uv = image_px / image_size;
  if (any(uv < 0.0) || any(uv > 1.0)) {
    return float4(background_at(vin.pos.xy), opacity);
  }
  float3 h = float3(uv, 1.0);
  float2 src = float2(dot(map0.xyz, h), dot(map1.xyz, h));
  if (map0.w > 0.5 && (any(src < 0.0) || any(src > 1.0))) {
    return float4(background_at(vin.pos.xy), opacity);
  }
  return finish(sample_filtered(src, texture_size), vin.pos.xy, image_px);
}

// Issue #164: the background alone, for a frame whose ready tiles at the drawn
// level cover every image pixel in view. The overview under them would be
// sampled (Catmull-Rom above 100 %) and then wholly overdrawn; outside the
// image (and outside an edit's source) the background is all it shows anyway.
float4 ps_background(VSOut vin) : SV_Target {
  return float4(background_at(vin.pos.xy), opacity);
}

float4 ps_tile(TileOut vin) : SV_Target {
  float2 tile_origin = vin.rec0.xy;
  float2 tile_scale = vin.rec0.zw;
  float tile_border = vin.rec1.x;
  float tile_tex = vin.rec1.y;
  float2 image_px = image_px_at(vin.pos.xy);
  // Output pixel -> source pixel. A tile can reach past the edited output (the
  // cropped-away part of the source): those pixels are not the picture.
  float2 out_uv = image_px / image_size;
  if (any(out_uv < 0.0) || any(out_uv > 1.0)) discard;
  float3 h = float3(out_uv, 1.0);
  float2 source_px = float2(dot(map0.xyz, h), dot(map1.xyz, h)) * source_size.xy;
  float2 level_px = source_px / tile_scale;
  float2 uv = (level_px - tile_origin + tile_border) / tile_tex;
  return finish(sample_filtered(uv, float2(tile_tex, tile_tex)), vin.pos.xy, image_px);
}
