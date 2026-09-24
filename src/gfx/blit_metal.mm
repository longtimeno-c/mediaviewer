// SPDX-License-Identifier: GPL-2.0-or-later
#include "gfx/blit_metal.h"

#import <Metal/Metal.h>

#include "core/trace.h"

namespace mv::gfx {
namespace {

// Line-for-line twin of gfx/blit.cpp's kHlsl vs_main/catmull_rom_1d/texel/
// sample_catmull/background_at/sample_filtered/image_px_at/finish/ps_main.
// Buffer(0) here is the Camera cbuffer's register(b0) twin (plan/15 binding
// note); no Tile buffer — the tiled path is not ported in PR 17.
constexpr const char kMsl[] = R"(
#include <metal_stdlib>
using namespace metal;

struct Camera {
  float2 pan;
  float zoom;
  float opacity;
  float2 window_size;
  float2 image_size;
  float2 origin;
  float2 texture_size;
  float background;
  float clipping;
  float time;
  float grid;
  // PR 10 geometry (rotate / flip / straighten / crop): output uv -> source
  // uv, affine. xyz of each row are the matrix; map0.w > 0.5 means samples
  // that land outside the source show the background (a straightened frame's
  // corners in crop mode). The identity leaves every pixel as before.
  float4 map0;
  float4 map1;
};

struct VSOut { float4 pos [[position]]; };

vertex VSOut vs_main(uint id [[vertex_id]]) {
  VSOut o;
  float2 uv = float2(float((id << 1) & 2), float(id & 2));
  o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return o;
}

static float4 catmull_rom_1d(float4 a, float4 b, float4 c, float4 d, float t) {
  float t2 = t * t;
  float t3 = t2 * t;
  return 0.5 * ((2.0 * b) +
                (-a + c) * t +
                (2.0 * a - 5.0 * b + 4.0 * c - d) * t2 +
                (-a + 3.0 * b - 3.0 * c + d) * t3);
}

static float4 texel(texture2d<float> tex, sampler samp_point, int2 p, float2 tex_size) {
  return tex.sample(samp_point, (float2(p) + 0.5) / tex_size, level(0));
}

static float4 sample_catmull(texture2d<float> tex, sampler samp_point, float2 uv,
                             float2 tex_size) {
  float2 pos = uv * tex_size - 0.5;
  float2 f = fract(pos);
  int2 i = int2(floor(pos));
  float4 rows[4];
  for (int y = -1; y <= 2; ++y) {
    float4 c0 = texel(tex, samp_point, i + int2(-1, y), tex_size);
    float4 c1 = texel(tex, samp_point, i + int2( 0, y), tex_size);
    float4 c2 = texel(tex, samp_point, i + int2( 1, y), tex_size);
    float4 c3 = texel(tex, samp_point, i + int2( 2, y), tex_size);
    rows[y + 1] = catmull_rom_1d(c0, c1, c2, c3, f.x);
  }
  return saturate(catmull_rom_1d(rows[0], rows[1], rows[2], rows[3], f.y));
}

static float3 background_at(constant Camera& cam, float2 screen) {
  if (cam.background < 0.5) return float3(0.016, 0.018, 0.024);
  if (cam.background < 1.5) return float3(0.214, 0.214, 0.214);
  if (cam.background < 2.5) return float3(1.0, 1.0, 1.0);
  float2 cell = floor((screen - cam.origin) / 12.0);
  float odd = fmod(abs(cell.x + cell.y), 2.0);
  return odd < 0.5 ? float3(0.527, 0.527, 0.527) : float3(0.815, 0.815, 0.815);
}

static float4 sample_filtered(constant Camera& cam, texture2d<float> img, sampler samp_aniso,
                              sampler samp_point, float2 uv, float2 tex_size) {
  if (cam.zoom >= 4.0) return img.sample(samp_point, uv, level(0));
  if (cam.zoom >= 1.0) return sample_catmull(img, samp_point, uv, tex_size);
  return img.sample(samp_aniso, uv);
}

static float2 image_px_at(constant Camera& cam, float2 screen) {
  return cam.pan + (screen - (cam.origin + cam.window_size * 0.5)) / cam.zoom;
}

static float4 finish(constant Camera& cam, float4 c, float2 screen, float2 image_px) {
  float3 bg = background_at(cam, screen);
  float3 rgb = mix(bg, c.rgb, saturate(c.a));

  if (cam.clipping > 0.5 && fract(cam.time * 2.0) < 0.5) {
    float hi = max(c.r, max(c.g, c.b));
    if (hi >= 0.9911) rgb = float3(1.0, 0.0, 0.0);
    else if (hi <= 0.0003) rgb = float3(0.0, 0.25, 1.0);
  }

  if (cam.grid > 0.5 && cam.zoom >= 4.0) {
    float2 f = fract(image_px);
    float edge = 1.0 / cam.zoom;
    if (f.x < edge || f.y < edge) rgb *= 0.6;
  }
  return float4(rgb, cam.opacity);
}

fragment float4 ps_main(VSOut vin [[stage_in]],
                        constant Camera& cam [[buffer(0)]],
                        texture2d<float> img [[texture(0)]],
                        sampler samp_aniso [[sampler(0)]],
                        sampler samp_point [[sampler(1)]]) {
  float2 image_px = image_px_at(cam, vin.pos.xy);
  float2 uv = image_px / cam.image_size;
  if (any(uv < 0.0) || any(uv > 1.0)) {
    return float4(background_at(cam, vin.pos.xy), cam.opacity);
  }
  float3 h = float3(uv, 1.0);
  float2 src = float2(dot(cam.map0.xyz, h), dot(cam.map1.xyz, h));
  if (cam.map0.w > 0.5 && (any(src < 0.0) || any(src > 1.0))) {
    return float4(background_at(cam, vin.pos.xy), cam.opacity);
  }
  return finish(cam, sample_filtered(cam, img, samp_aniso, samp_point, src, cam.texture_size),
               vin.pos.xy, image_px);
}
)";

struct alignas(16) camera_cb {
  float pan_x, pan_y;
  float zoom;
  float opacity;
  float window_w, window_h;
  float image_w, image_h;
  float origin_x, origin_y;
  float texture_w, texture_h;
  float background, clipping, time, grid;
  float map0[4];
  float map1[4];
};

static_assert(sizeof(camera_cb) == 96, "keep in sync with the MSL Camera struct");

}  // namespace

blitter_mac::~blitter_mac() { destroy(); }

expected blitter_mac::create(void* mtl_device, std::uint64_t pixel_format) noexcept {
  destroy();
  if (!mtl_device) return err(status::invalid_arg);

  id<MTLDevice> device = (__bridge id<MTLDevice>)mtl_device;

  NSError* compile_error = nil;
  id<MTLLibrary> library = [device newLibraryWithSource:@(kMsl)
                                                  options:nil
                                                    error:&compile_error];
  if (!library) {
    MV_LOG_ERROR("blitter_mac: MSL compile failed: %s",
                compile_error ? compile_error.localizedDescription.UTF8String : "unknown");
    return err(status::internal);
  }
  id<MTLFunction> vs = [library newFunctionWithName:@"vs_main"];
  id<MTLFunction> ps = [library newFunctionWithName:@"ps_main"];
  if (!vs || !ps) return err(status::internal);

  MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
  desc.vertexFunction = vs;
  desc.fragmentFunction = ps;
  desc.colorAttachments[0].pixelFormat = static_cast<MTLPixelFormat>(pixel_format);

  NSError* pso_error = nil;
  id<MTLRenderPipelineState> opaque = [device newRenderPipelineStateWithDescriptor:desc
                                                                              error:&pso_error];
  if (!opaque) {
    MV_LOG_ERROR("blitter_mac: opaque PSO failed: %s",
                pso_error ? pso_error.localizedDescription.UTF8String : "unknown");
    return err(status::internal);
  }

  // The fade: blended in linear light (the drawable is _sRGB), same as the
  // D3D11 blend_alpha_ state.
  desc.colorAttachments[0].blendingEnabled = YES;
  desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
  desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  desc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
  desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
  desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorZero;
  desc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;

  pso_error = nil;
  id<MTLRenderPipelineState> blend = [device newRenderPipelineStateWithDescriptor:desc
                                                                             error:&pso_error];
  if (!blend) {
    MV_LOG_ERROR("blitter_mac: blend PSO failed: %s",
                pso_error ? pso_error.localizedDescription.UTF8String : "unknown");
    return err(status::internal);
  }

  MTLSamplerDescriptor* aniso_desc = [MTLSamplerDescriptor new];
  aniso_desc.minFilter = MTLSamplerMinMagFilterLinear;
  aniso_desc.magFilter = MTLSamplerMinMagFilterLinear;
  aniso_desc.mipFilter = MTLSamplerMipFilterLinear;
  aniso_desc.maxAnisotropy = 16;
  aniso_desc.sAddressMode = MTLSamplerAddressModeClampToEdge;
  aniso_desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
  id<MTLSamplerState> aniso = [device newSamplerStateWithDescriptor:aniso_desc];

  MTLSamplerDescriptor* point_desc = [MTLSamplerDescriptor new];
  point_desc.minFilter = MTLSamplerMinMagFilterNearest;
  point_desc.magFilter = MTLSamplerMinMagFilterNearest;
  point_desc.mipFilter = MTLSamplerMipFilterNearest;
  point_desc.sAddressMode = MTLSamplerAddressModeClampToEdge;
  point_desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
  id<MTLSamplerState> point = [device newSamplerStateWithDescriptor:point_desc];

  if (!aniso || !point) return err(status::internal);

  pipeline_opaque_ = (__bridge_retained void*)opaque;
  pipeline_blend_ = (__bridge_retained void*)blend;
  sampler_aniso_ = (__bridge_retained void*)aniso;
  sampler_point_ = (__bridge_retained void*)point;
  return {};
}

void blitter_mac::destroy() noexcept {
  if (sampler_point_) { (void)(__bridge_transfer id)sampler_point_; sampler_point_ = nullptr; }
  if (sampler_aniso_) { (void)(__bridge_transfer id)sampler_aniso_; sampler_aniso_ = nullptr; }
  if (pipeline_blend_) { (void)(__bridge_transfer id)pipeline_blend_; pipeline_blend_ = nullptr; }
  if (pipeline_opaque_) { (void)(__bridge_transfer id)pipeline_opaque_; pipeline_opaque_ = nullptr; }
}

void blitter_mac::draw(void* encoder_ptr, void* texture_ptr, const blit_params_mac& p) noexcept {
  if (!encoder_ptr || !texture_ptr || !pipeline_opaque_ || !pipeline_blend_) return;

  id<MTLRenderCommandEncoder> encoder = (__bridge id<MTLRenderCommandEncoder>)encoder_ptr;
  id<MTLTexture> texture = (__bridge id<MTLTexture>)texture_ptr;
  id<MTLRenderPipelineState> pso =
      (__bridge id<MTLRenderPipelineState>)(p.opacity < 0.999f ? pipeline_blend_
                                                                : pipeline_opaque_);

  camera_cb cb{};
  cb.pan_x = p.pan_x;
  cb.pan_y = p.pan_y;
  cb.zoom = (p.zoom > 0.0f) ? p.zoom : 1.0f;
  cb.opacity = p.opacity < 0.0f ? 0.0f : (p.opacity > 1.0f ? 1.0f : p.opacity);
  cb.window_w = p.window_w;
  cb.window_h = p.window_h;
  cb.image_w = p.image_w;
  cb.image_h = p.image_h;
  cb.origin_x = p.origin_x;
  cb.origin_y = p.origin_y;
  cb.texture_w = p.texture_w > 0.0f ? p.texture_w : p.image_w;
  cb.texture_h = p.texture_h > 0.0f ? p.texture_h : p.image_h;
  cb.background = static_cast<float>(p.background);
  cb.clipping = p.clipping ? 1.0f : 0.0f;
  cb.time = p.time_seconds;
  cb.grid = p.pixel_grid ? 1.0f : 0.0f;
  cb.map0[0] = p.uv_map[0];
  cb.map0[1] = p.uv_map[1];
  cb.map0[2] = p.uv_map[2];
  cb.map0[3] = p.clip_to_source ? 1.0f : 0.0f;
  cb.map1[0] = p.uv_map[3];
  cb.map1[1] = p.uv_map[4];
  cb.map1[2] = p.uv_map[5];
  cb.map1[3] = 0.0f;

  [encoder setRenderPipelineState:pso];
  [encoder setFragmentBytes:&cb length:sizeof(cb) atIndex:0];
  [encoder setFragmentTexture:texture atIndex:0];
  [encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)sampler_aniso_ atIndex:0];
  [encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)sampler_point_ atIndex:1];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

}  // namespace mv::gfx
