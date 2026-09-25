// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/video_blit_metal.h"

#import <Metal/Metal.h>

#include "core/trace.h"

namespace mv::gfx {
namespace {

// Enum values reach the shader as uints and MUST match colour_desc.h.
static_assert(static_cast<int>(colour_matrix::bt709) == 1);
static_assert(static_cast<int>(colour_matrix::bt601) == 2);
static_assert(static_cast<int>(colour_matrix::bt2020_ncl) == 3);
static_assert(static_cast<int>(colour_matrix::smpte240m) == 4);
static_assert(static_cast<int>(colour_transfer::bt709) == 1);
static_assert(static_cast<int>(colour_transfer::srgb) == 2);
static_assert(static_cast<int>(colour_transfer::smpte2084) == 3);
static_assert(static_cast<int>(colour_transfer::arib_std_b67) == 4);
static_assert(static_cast<int>(colour_range::limited) == 1);
static_assert(static_cast<int>(colour_range::full) == 2);
static_assert(static_cast<int>(colour_primaries::bt601_525) == 2);
static_assert(static_cast<int>(colour_primaries::bt601_625) == 3);
static_assert(static_cast<int>(colour_primaries::bt2020) == 4);

// Line-for-line twin of gfx/video_blit.cpp's kHlsl. Binding: buffer(0) is the
// cbuffer register(b0); texture(0)/texture(1) are t0/t1; sampler(0) is s0.
// Output is LINEAR light: the drawable is _sRGB, so the hardware encodes on
// write (writing an encoded value would double-encode it).
constexpr const char kMsl[] = R"(
#include <metal_stdlib>
using namespace metal;

struct VideoCamera {
  float2 pan;
  float  zoom;
  float  _pad0;
  float2 window_size;
  float2 image_size;    // the VISIBLE frame, in pixels
  float2 texture_size;  // the allocation, possibly padded
  float2 origin;
  uint4  colour_a;      // x matrix, y transfer, z range, w bit_depth
  uint4  colour_b;      // x primaries
};

struct VSOut { float4 pos [[position]]; };

vertex VSOut vs_video(uint id [[vertex_id]]) {
  VSOut o;
  float2 uv = float2(float((id << 1) & 2), float(id & 2));
  o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return o;
}

// BT.1886, not sRGB: video is display-referred to gamma 2.4.
static float3 eotf_bt1886(float3 v) { return pow(max(v, 0.0), 2.4); }

static float3 eotf_srgb(float3 v) {
  return select(pow((v + 0.055) / 1.055, 2.4), v / 12.92, v <= 0.04045);
}

// SMPTE ST 2084 (PQ). 0..1 where 1.0 is 10000 nits.
static float3 eotf_pq(float3 e) {
  const float m1 = 0.1593017578125, m2 = 78.84375;
  const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
  float3 p = pow(max(e, 0.0), 1.0 / m2);
  float3 num = max(p - c1, 0.0);
  float3 den = c2 - c3 * p;
  return pow(num / max(den, 1e-6), 1.0 / m1);
}

// ARIB STD-B67 (HLG) inverse OETF: signal -> SCENE linear, 0..1.
static float3 inverse_oetf_hlg(float3 e) {
  const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
  float3 lo = (e * e) / 3.0;
  float3 hi = (exp((e - c) / a) + b) / 12.0;
  return select(hi, lo, e <= 0.5);
}

// We output BT.709 primaries: the drawable is 8-bit sRGB (D6).
static float3 to_bt709(float3 rgb, uint primaries) {
  if (primaries == 4) {  // BT.2020
    return float3(dot(rgb, float3( 1.6605, -0.5876, -0.0728)),
                  dot(rgb, float3(-0.1246,  1.1329, -0.0083)),
                  dot(rgb, float3(-0.0182, -0.1006,  1.1187)));
  }
  if (primaries == 2) {  // SMPTE-C
    return float3(dot(rgb, float3( 0.9395,  0.0502,  0.0103)),
                  dot(rgb, float3( 0.0178,  0.9658,  0.0164)),
                  dot(rgb, float3(-0.0016, -0.0044,  1.0060)));
  }
  if (primaries == 3) {  // EBU
    return float3(dot(rgb, float3( 1.0440, -0.0440,  0.0000)),
                  dot(rgb, float3( 0.0000,  1.0000,  0.0000)),
                  dot(rgb, float3( 0.0000,  0.0118,  0.9882)));
  }
  return rgb;
}

// Reinhard-extended on luminance with the ratio applied to RGB (hue preserved).
// Reference white 203 nits (ITU-R BT.2408): diffuse white lands at SDR white.
static float3 tone_map(float3 linear_scene, float peak_over_white) {
  float l = max(dot(linear_scene, float3(0.2126, 0.7152, 0.0722)), 1e-6);
  float w2 = peak_over_white * peak_over_white;
  float mapped = l * (1.0 + l / w2) / (1.0 + l);
  return linear_scene * (mapped / l);
}

fragment float4 ps_video(VSOut vin [[stage_in]],
                         constant VideoCamera& cam [[buffer(0)]],
                         texture2d<float> luma_tex [[texture(0)]],
                         texture2d<float> chroma_tex [[texture(1)]],
                         sampler samp [[sampler(0)]]) {
  uint matrix_id = cam.colour_a.x;
  uint transfer  = cam.colour_a.y;
  uint range_id  = cam.colour_a.z;
  uint depth     = cam.colour_a.w;
  uint primaries = cam.colour_b.x;

  float2 image_px = cam.pan + (vin.pos.xy - (cam.origin + cam.window_size * 0.5)) / cam.zoom;
  float2 uv = image_px / cam.image_size;
  if (any(uv < 0.0) || any(uv > 1.0)) {
    return float4(0.016, 0.018, 0.024, 1.0);
  }
  // Sample the VISIBLE rect, not the allocation.
  float2 uv_tex = uv * (cam.image_size / cam.texture_size);

  float  y  = luma_tex.sample(samp, uv_tex, level(0)).r;
  float2 cc = chroma_tex.sample(samp, uv_tex, level(0)).rg;

  // P010: 10 bits in the HIGH bits of a 16-bit word; an R16Unorm read returns
  // code*64/65535, not code/1023.
  if (depth > 8) {
    const float p010_scale = 65535.0 / 65472.0;
    y  *= p010_scale;
    cc *= p010_scale;
  }

  // Range. An explicit full-range tag wins; limited is the default, never assumed.
  float y_min, y_span, c_mid, c_span;
  if (range_id == 2) {
    y_min = 0.0; y_span = 1.0; c_mid = 0.5; c_span = 1.0;
  } else if (depth > 8) {
    y_min = 64.0 / 1023.0; y_span = 876.0 / 1023.0;
    c_mid = 512.0 / 1023.0; c_span = 896.0 / 1023.0;
  } else {
    y_min = 16.0 / 255.0; y_span = 219.0 / 255.0;
    c_mid = 128.0 / 255.0; c_span = 224.0 / 255.0;
  }
  float yy = (y - y_min) / y_span;
  float cb = (cc.x - c_mid) / c_span;
  float cr = (cc.y - c_mid) / c_span;

  float kr, kb;
  if (matrix_id == 2)      { kr = 0.299;  kb = 0.114;  }  // BT.601
  else if (matrix_id == 3) { kr = 0.2627; kb = 0.0593; }  // BT.2020 NCL
  else if (matrix_id == 4) { kr = 0.212;  kb = 0.087;  }  // SMPTE 240M
  else                     { kr = 0.2126; kb = 0.0722; }  // BT.709
  float kg = 1.0 - kr - kb;

  float3 rgb;
  rgb.r = yy + 2.0 * (1.0 - kr) * cr;
  rgb.b = yy + 2.0 * (1.0 - kb) * cb;
  rgb.g = yy - (2.0 * (kr * (1.0 - kr) * cr + kb * (1.0 - kb) * cb)) / kg;

  float3 linear_rgb;
  if (transfer == 3) {
    // PQ: normalise so 203 nits reads as 1.0, then tone-map.
    linear_rgb = eotf_pq(saturate(rgb)) * (10000.0 / 203.0);
    linear_rgb = to_bt709(linear_rgb, primaries);
    linear_rgb = tone_map(linear_rgb, 10000.0 / 203.0);
  } else if (transfer == 4) {
    // HLG: inverse OETF gives scene light; the OOTF (gamma 1.2 for a 1000-nit
    // reference) makes it display light. Skipping it is what makes an iPhone
    // HLG clip look flat and washed out.
    float3 scene = inverse_oetf_hlg(saturate(rgb));
    float ys = max(dot(scene, float3(0.2627, 0.6780, 0.0593)), 1e-6);
    linear_rgb = scene * pow(ys, 0.2) * (1000.0 / 203.0);
    linear_rgb = to_bt709(linear_rgb, primaries);
    linear_rgb = tone_map(linear_rgb, 1000.0 / 203.0);
  } else if (transfer == 2) {
    linear_rgb = to_bt709(eotf_srgb(saturate(rgb)), primaries);
  } else {
    // SDR video: display-referred, NOT tone-mapped (like a camera JPEG, D6).
    linear_rgb = to_bt709(eotf_bt1886(saturate(rgb)), primaries);
  }
  return float4(saturate(linear_rgb), 1.0);
}
)";

struct alignas(16) video_camera_cb {
  float pan_x, pan_y;
  float zoom;
  float pad0;
  float window_w, window_h;
  float image_w, image_h;
  float texture_w, texture_h;
  float origin_x, origin_y;
  std::uint32_t matrix, transfer, range, bit_depth;
  std::uint32_t primaries, pad1, pad2, pad3;
};
static_assert(sizeof(video_camera_cb) == 80, "keep in sync with the MSL VideoCamera struct");

}  // namespace

video_blitter_mac::~video_blitter_mac() { destroy(); }

expected video_blitter_mac::create(void* mtl_device, std::uint64_t pixel_format) noexcept {
  destroy();
  if (!mtl_device) return err(status::invalid_arg);
  id<MTLDevice> device = (__bridge id<MTLDevice>)mtl_device;

  NSError* error = nil;
  id<MTLLibrary> library = [device newLibraryWithSource:@(kMsl) options:nil error:&error];
  if (!library) {
    MV_LOG_ERROR("video_blitter_mac: MSL compile failed: %s",
                 error ? error.localizedDescription.UTF8String : "unknown");
    return err(status::internal);
  }
  id<MTLFunction> vs = [library newFunctionWithName:@"vs_video"];
  id<MTLFunction> ps = [library newFunctionWithName:@"ps_video"];
  if (!vs || !ps) return err(status::internal);

  MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
  desc.vertexFunction = vs;
  desc.fragmentFunction = ps;
  desc.colorAttachments[0].pixelFormat = static_cast<MTLPixelFormat>(pixel_format);
  error = nil;
  id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:desc error:&error];
  if (!pso) {
    MV_LOG_ERROR("video_blitter_mac: PSO failed: %s",
                 error ? error.localizedDescription.UTF8String : "unknown");
    return err(status::internal);
  }

  MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
  sd.minFilter = MTLSamplerMinMagFilterLinear;
  sd.magFilter = MTLSamplerMinMagFilterLinear;
  sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
  sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
  id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sd];
  if (!sampler) return err(status::internal);

  pipeline_ = (__bridge_retained void*)pso;
  sampler_ = (__bridge_retained void*)sampler;
  return {};
}

void video_blitter_mac::destroy() noexcept {
  if (sampler_) { (void)(__bridge_transfer id)sampler_; sampler_ = nullptr; }
  if (pipeline_) { (void)(__bridge_transfer id)pipeline_; pipeline_ = nullptr; }
}

void video_blitter_mac::draw(void* encoder_ptr, void* luma, void* chroma,
                             const colour_desc& colour, const video_blit_params_mac& p) noexcept {
  if (!encoder_ptr || !luma || !chroma || !pipeline_) return;
  id<MTLRenderCommandEncoder> encoder = (__bridge id<MTLRenderCommandEncoder>)encoder_ptr;

  video_camera_cb cb{};
  cb.pan_x = p.pan_x;
  cb.pan_y = p.pan_y;
  cb.zoom = p.zoom > 0.0f ? p.zoom : 1.0f;
  cb.window_w = p.window_w;
  cb.window_h = p.window_h;
  cb.image_w = p.image_w;
  cb.image_h = p.image_h;
  cb.texture_w = p.texture_w > 0.0f ? p.texture_w : p.image_w;
  cb.texture_h = p.texture_h > 0.0f ? p.texture_h : p.image_h;
  cb.origin_x = p.origin_x;
  cb.origin_y = p.origin_y;
  cb.matrix = static_cast<std::uint32_t>(colour.matrix);
  cb.transfer = static_cast<std::uint32_t>(colour.transfer);
  cb.range = static_cast<std::uint32_t>(colour.range);
  cb.bit_depth = colour.bit_depth;
  cb.primaries = static_cast<std::uint32_t>(colour.primaries);

  [encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)pipeline_];
  [encoder setFragmentBytes:&cb length:sizeof(cb) atIndex:0];
  [encoder setFragmentTexture:(__bridge id<MTLTexture>)luma atIndex:0];
  [encoder setFragmentTexture:(__bridge id<MTLTexture>)chroma atIndex:1];
  [encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)sampler_ atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

}  // namespace mv::gfx
