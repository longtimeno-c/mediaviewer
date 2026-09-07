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
  float _pad0;
  float2 window_size;
  float2 image_size;
  float2 origin;
  float2 _pad1;
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
  return catmull_rom_1d(rows[0], rows[1], rows[2], rows[3], f.y);
}

float4 ps_main(VSOut vin) : SV_Target {
  float2 image_px = pan + (vin.pos.xy - (origin + window_size * 0.5)) / zoom;
  float2 uv = image_px / image_size;
  if (any(uv < 0.0) || any(uv > 1.0)) {
    return float4(0.016, 0.018, 0.024, 1.0);
  }

  if (zoom >= 4.0) {
    return img.Sample(samp_point, uv);
  }
  if (zoom >= 1.0) {
    return sample_catmull(img, uv, image_size);
  }
  return img.Sample(samp_aniso, uv);
}
)";

struct alignas(16) blit_cb {
  float pan_x, pan_y;
  float zoom;
  float pad0;
  float window_w, window_h;
  float image_w, image_h;
  float origin_x, origin_y;
  float pad1, pad2;
};

expected compile(const char* entry, const char* target, com_ptr<ID3DBlob>& blob) {
  com_ptr<ID3DBlob> errors;
  const HRESULT hr =
      D3DCompile(kHlsl, sizeof(kHlsl) - 1, "blit.hlsl", nullptr, nullptr, entry, target,
                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.GetAddressOf(), errors.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);
  return {};
}

}  // namespace

blitter::~blitter() { destroy(); }

expected blitter::create(ID3D11Device* device) {
  destroy();
  if (!device) return err(status::invalid_arg);

  com_ptr<ID3DBlob> vsb, psb;
  if (auto r = compile("vs_main", "vs_5_0", vsb); !r) return r;
  if (auto r = compile("ps_main", "ps_5_0", psb); !r) return r;

  HRESULT hr = device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr,
                                          vs_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);
  hr = device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr,
                                 ps_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);

  D3D11_BUFFER_DESC cbd{};
  cbd.ByteWidth = sizeof(blit_cb);
  cbd.Usage = D3D11_USAGE_DYNAMIC;
  cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  hr = device->CreateBuffer(&cbd, nullptr, cb_.GetAddressOf());
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
  blend_.Reset();
  point_.Reset();
  aniso_.Reset();
  cb_.Reset();
  ps_.Reset();
  vs_.Reset();
}

void blitter::draw(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* image,
                   const blit_params& p) noexcept {
  if (!ctx || !image || !vs_ || !ps_ || !cb_) return;

  blit_cb cb{};
  cb.pan_x = p.pan_x;
  cb.pan_y = p.pan_y;
  cb.zoom = (p.zoom > 0.0f) ? p.zoom : 1.0f;
  cb.window_w = p.window_w;
  cb.window_h = p.window_h;
  cb.image_w = p.image_w;
  cb.image_h = p.image_h;
  cb.origin_x = p.origin_x;
  cb.origin_y = p.origin_y;

  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (SUCCEEDED(ctx->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    std::memcpy(mapped.pData, &cb, sizeof(cb));
    ctx->Unmap(cb_.Get(), 0);
  }

  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->IASetInputLayout(nullptr);
  ctx->VSSetShader(vs_.Get(), nullptr, 0);
  ctx->PSSetShader(ps_.Get(), nullptr, 0);
  ID3D11Buffer* cbs[] = {cb_.Get()};
  ctx->VSSetConstantBuffers(0, 1, cbs);
  ctx->PSSetConstantBuffers(0, 1, cbs);
  ID3D11ShaderResourceView* srvs[] = {image};
  ctx->PSSetShaderResources(0, 1, srvs);
  ID3D11SamplerState* samps[] = {aniso_.Get(), point_.Get()};
  ctx->PSSetSamplers(0, 2, samps);
  ctx->OMSetBlendState(blend_.Get(), nullptr, 0xffffffff);
  ctx->RSSetState(raster_.Get());
  ctx->Draw(3, 0);

  ID3D11ShaderResourceView* none[] = {nullptr};
  ctx->PSSetShaderResources(0, 1, none);
}

}  // namespace mv::gfx
