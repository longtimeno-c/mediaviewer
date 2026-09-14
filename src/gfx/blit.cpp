// SPDX-License-Identifier: GPL-2.0-or-later
#include "gfx/blit.h"

#include <d3dcompiler.h>

#include <cstring>

namespace mv::gfx {
namespace {

constexpr const char kHlsl[] = R"(
cbuffer Camera : register(b0) {
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
};

cbuffer Tile : register(b1) {
  float2 tile_origin;
  float2 tile_scale;
  float2 tile_content;
  float tile_border;
  float tile_tex;
};

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
VSOut vs_tile(uint id : SV_VertexID) {
  static const float2 corners[6] = {
    float2(0, 0), float2(1, 0), float2(0, 1),
    float2(0, 1), float2(1, 0), float2(1, 1)
  };
  VSOut o;
  float2 image_px = min((tile_origin + corners[id] * tile_content) * tile_scale, image_size);
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
float3 background_at(float2 screen) {
  if (background < 0.5) return float3(0.016, 0.018, 0.024);
  if (background < 1.5) return float3(0.214, 0.214, 0.214);
  if (background < 2.5) return float3(1.0, 1.0, 1.0);
  // Checkerboard: fixed 12 px cells in screen space, so it reads as "this is
  // transparency" at every zoom instead of scaling with the image.
  float2 cell = floor((screen - origin) / 12.0);
  float odd = fmod(abs(cell.x + cell.y), 2.0);
  return odd < 0.5 ? float3(0.527, 0.527, 0.527) : float3(0.815, 0.815, 0.815);
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
  float3 bg = background_at(screen);
  float3 rgb = lerp(bg, c.rgb, saturate(c.a));

  // Display-referred blinkies (plan/16 `C`): a channel at sRGB 254+ is a
  // clipped highlight, every channel at sRGB 1 or below is a crushed shadow.
  // The accurate RAW version is PR 10's.
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
  return finish(sample_filtered(uv, texture_size), vin.pos.xy, image_px);
}

float4 ps_tile(VSOut vin) : SV_Target {
  float2 image_px = image_px_at(vin.pos.xy);
  float2 level_px = image_px / tile_scale;
  float2 uv = (level_px - tile_origin + tile_border) / tile_tex;
  return finish(sample_filtered(uv, float2(tile_tex, tile_tex)), vin.pos.xy, image_px);
}
)";

struct alignas(16) blit_cb {
  float pan_x, pan_y;
  float zoom;
  float opacity;
  float window_w, window_h;
  float image_w, image_h;
  float origin_x, origin_y;
  float texture_w, texture_h;
  float background, clipping, time, grid;
};

static_assert(sizeof(blit_cb) == 64, "keep in sync with cbuffer Camera");

struct alignas(16) tile_cb {
  float origin_x, origin_y;
  float scale_x, scale_y;
  float content_w, content_h;
  float border, tex;
};

static_assert(sizeof(tile_cb) == 32, "keep in sync with cbuffer Tile");

expected compile(const char* entry, const char* target, com_ptr<ID3DBlob>& blob) {
  com_ptr<ID3DBlob> errors;
  const HRESULT hr =
      D3DCompile(kHlsl, sizeof(kHlsl) - 1, "blit.hlsl", nullptr, nullptr, entry, target,
                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.GetAddressOf(), errors.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);
  return {};
}

bool upload_cb(ID3D11DeviceContext* ctx, ID3D11Buffer* buffer, const void* data,
               std::size_t size) noexcept {
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(ctx->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
  std::memcpy(mapped.pData, data, size);
  ctx->Unmap(buffer, 0);
  return true;
}

}  // namespace

blitter::~blitter() { destroy(); }

expected blitter::create(ID3D11Device* device) {
  destroy();
  if (!device) return err(status::invalid_arg);

  com_ptr<ID3DBlob> vsb, psb, tvsb, tpsb;
  if (auto r = compile("vs_main", "vs_5_0", vsb); !r) return r;
  if (auto r = compile("ps_main", "ps_5_0", psb); !r) return r;
  if (auto r = compile("vs_tile", "vs_5_0", tvsb); !r) return r;
  if (auto r = compile("ps_tile", "ps_5_0", tpsb); !r) return r;

  HRESULT hr = device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr,
                                          vs_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);
  hr = device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr,
                                 ps_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);
  hr = device->CreateVertexShader(tvsb->GetBufferPointer(), tvsb->GetBufferSize(), nullptr,
                                  tile_vs_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);
  hr = device->CreatePixelShader(tpsb->GetBufferPointer(), tpsb->GetBufferSize(), nullptr,
                                 tile_ps_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);

  D3D11_BUFFER_DESC cbd{};
  cbd.ByteWidth = sizeof(blit_cb);
  cbd.Usage = D3D11_USAGE_DYNAMIC;
  cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  hr = device->CreateBuffer(&cbd, nullptr, cb_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);
  cbd.ByteWidth = sizeof(tile_cb);
  hr = device->CreateBuffer(&cbd, nullptr, tile_cb_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);

  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_ANISOTROPIC;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxAnisotropy = 16;
  sd.MinLOD = 0.0f;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
  hr = device->CreateSamplerState(&sd, aniso_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);

  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sd.MaxAnisotropy = 1;
  hr = device->CreateSamplerState(&sd, point_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);

  D3D11_BLEND_DESC bd{};
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  hr = device->CreateBlendState(&bd, blend_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);

  // The fade: blended in linear light (the RTV is _SRGB), so a cross-fade does
  // not dip in brightness at the midpoint.
  bd.RenderTarget[0].BlendEnable = TRUE;
  bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
  bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  hr = device->CreateBlendState(&bd, blend_alpha_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);

  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  hr = device->CreateRasterizerState(&rd, raster_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);

  return {};
}

void blitter::destroy() noexcept {
  raster_.Reset();
  blend_alpha_.Reset();
  blend_.Reset();
  point_.Reset();
  aniso_.Reset();
  tile_cb_.Reset();
  cb_.Reset();
  tile_ps_.Reset();
  tile_vs_.Reset();
  ps_.Reset();
  vs_.Reset();
}

void blitter::bind_camera(ID3D11DeviceContext* ctx, const blit_params& p, float texture_w,
                          float texture_h) noexcept {
  blit_cb cb{};
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
  cb.texture_w = texture_w;
  cb.texture_h = texture_h;
  cb.background = static_cast<float>(p.background);
  cb.clipping = p.clipping ? 1.0f : 0.0f;
  cb.time = p.time_seconds;
  cb.grid = p.pixel_grid ? 1.0f : 0.0f;
  (void)upload_cb(ctx, cb_.Get(), &cb, sizeof(cb));

  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->IASetInputLayout(nullptr);
  ID3D11Buffer* cbs[] = {cb_.Get(), tile_cb_.Get()};
  ctx->VSSetConstantBuffers(0, 2, cbs);
  ctx->PSSetConstantBuffers(0, 2, cbs);
  ID3D11SamplerState* samps[] = {aniso_.Get(), point_.Get()};
  ctx->PSSetSamplers(0, 2, samps);
  ctx->OMSetBlendState(cb.opacity < 0.999f ? blend_alpha_.Get() : blend_.Get(), nullptr,
                       0xffffffff);
  ctx->RSSetState(raster_.Get());
}

void blitter::draw(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* image,
                   const blit_params& p) noexcept {
  if (!ctx || !image || !vs_ || !ps_ || !cb_) return;

  bind_camera(ctx, p, p.texture_w > 0.0f ? p.texture_w : p.image_w,
              p.texture_h > 0.0f ? p.texture_h : p.image_h);
  ctx->VSSetShader(vs_.Get(), nullptr, 0);
  ctx->PSSetShader(ps_.Get(), nullptr, 0);
  ID3D11ShaderResourceView* srvs[] = {image};
  ctx->PSSetShaderResources(0, 1, srvs);
  ctx->Draw(3, 0);

  ID3D11ShaderResourceView* none[] = {nullptr};
  ctx->PSSetShaderResources(0, 1, none);
}

void blitter::draw_tiles(ID3D11DeviceContext* ctx, std::span<const tile_quad> tiles,
                         const blit_params& p) noexcept {
  if (!ctx || tiles.empty() || !tile_vs_ || !tile_ps_ || !tile_cb_) return;

  bind_camera(ctx, p, 260.0f, 260.0f);
  ctx->VSSetShader(tile_vs_.Get(), nullptr, 0);
  ctx->PSSetShader(tile_ps_.Get(), nullptr, 0);
  for (const tile_quad& t : tiles) {
    if (!t.srv) continue;
    tile_cb cb{};
    cb.origin_x = t.origin_x;
    cb.origin_y = t.origin_y;
    cb.scale_x = t.scale_x;
    cb.scale_y = t.scale_y;
    cb.content_w = t.content_w;
    cb.content_h = t.content_h;
    cb.border = 2.0f;
    cb.tex = 260.0f;
    if (!upload_cb(ctx, tile_cb_.Get(), &cb, sizeof(cb))) continue;
    ID3D11ShaderResourceView* srvs[] = {t.srv};
    ctx->PSSetShaderResources(0, 1, srvs);
    ctx->Draw(6, 0);
  }
  ID3D11ShaderResourceView* none[] = {nullptr};
  ctx->PSSetShaderResources(0, 1, none);
}

}  // namespace mv::gfx
