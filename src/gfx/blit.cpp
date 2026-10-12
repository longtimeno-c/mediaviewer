// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/blit.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

// Built from gfx/blit.hlsl by fxc (CMakeLists.txt, mv_fxc; issue #163): one
// `const BYTE` array per entry point, so device create never runs D3DCompile.
#include "blit_ps_background.h"
#include "blit_ps_main.h"
#include "blit_ps_tile.h"
#include "blit_vs_main.h"
#include "blit_vs_tile.h"

namespace mv::gfx {
namespace {

struct alignas(16) blit_cb {
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
  float inv0[4];
  float inv1[4];
  float source_size[4];
  float adj0[4];
  float adj1[4];
  float theme_background[4];
};

static_assert(sizeof(blit_cb) == 192, "keep in sync with cbuffer Camera");

// One tile's per-instance record (vs_tile's TILE0 / TILE1).
struct tile_instance {
  float origin_x, origin_y;
  float scale_x, scale_y;
  float content_w, content_h;
  float border, tex;
};

static_assert(sizeof(tile_instance) == 32, "keep in sync with vs_tile's TILE0 / TILE1");

// Records per Map of the instance buffer (32 KB). A 4K view draws a few
// hundred tiles; a frame with more maps again per batch (WRITE_DISCARD renames
// the buffer) rather than growing it on the render thread.
constexpr UINT k_tile_batch = 1024;

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

  HRESULT hr = device->CreateVertexShader(kBlitVsMain, sizeof(kBlitVsMain), nullptr,
                                          vs_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);
  hr = device->CreatePixelShader(kBlitPsMain, sizeof(kBlitPsMain), nullptr, ps_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);
  hr = device->CreatePixelShader(kBlitPsBackground, sizeof(kBlitPsBackground), nullptr,
                                 background_ps_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);
  hr = device->CreateVertexShader(kBlitVsTile, sizeof(kBlitVsTile), nullptr,
                                  tile_vs_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);
  // Per-instance, so DrawInstanced's StartInstanceLocation selects the record
  // (SV_InstanceID would not include it).
  const D3D11_INPUT_ELEMENT_DESC tile_elements[] = {
      {"TILE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
      {"TILE", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
  };
  hr = device->CreateInputLayout(tile_elements, 2, kBlitVsTile, sizeof(kBlitVsTile),
                                 tile_layout_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);
  hr = device->CreatePixelShader(kBlitPsTile, sizeof(kBlitPsTile), nullptr,
                                 tile_ps_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);

  D3D11_BUFFER_DESC cbd{};
  cbd.ByteWidth = sizeof(blit_cb);
  cbd.Usage = D3D11_USAGE_DYNAMIC;
  cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  hr = device->CreateBuffer(&cbd, nullptr, cb_.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);
  D3D11_BUFFER_DESC ibd{};
  ibd.ByteWidth = static_cast<UINT>(sizeof(tile_instance)) * k_tile_batch;
  ibd.Usage = D3D11_USAGE_DYNAMIC;
  ibd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  ibd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  hr = device->CreateBuffer(&ibd, nullptr, tile_instances_.GetAddressOf());
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
  tile_instances_.Reset();
  cb_.Reset();
  tile_layout_.Reset();
  tile_ps_.Reset();
  tile_vs_.Reset();
  background_ps_.Reset();
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
  for (int i = 0; i < 3; ++i) cb.theme_background[i] = p.theme_background[i];
  // A zero map would sample one texel everywhere: always write it.
  cb.map0[0] = p.uv_map[0];
  cb.map0[1] = p.uv_map[1];
  cb.map0[2] = p.uv_map[2];
  cb.map0[3] = p.clip_to_source ? 1.0f : 0.0f;
  cb.map1[0] = p.uv_map[3];
  cb.map1[1] = p.uv_map[4];
  cb.map1[2] = p.uv_map[5];
  cb.map1[3] = 0.0f;
  for (int i = 0; i < 3; ++i) {
    cb.inv0[i] = p.uv_inverse[i];
    cb.inv1[i] = p.uv_inverse[3 + i];
  }
  cb.source_size[0] = p.source_w > 0.0f ? p.source_w : p.image_w;
  cb.source_size[1] = p.source_h > 0.0f ? p.source_h : p.image_h;
  for (int i = 0; i < 4; ++i) {
    cb.adj0[i] = p.adjust0[i];
    cb.adj1[i] = p.adjust1[i];
  }
  cb.adj1[3] = p.adjust ? 1.0f : 0.0f;
  (void)upload_cb(ctx, cb_.Get(), &cb, sizeof(cb));

  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->IASetInputLayout(nullptr);
  ID3D11Buffer* cbs[] = {cb_.Get()};
  ctx->VSSetConstantBuffers(0, 1, cbs);
  ctx->PSSetConstantBuffers(0, 1, cbs);
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

void blitter::draw_background(ID3D11DeviceContext* ctx, const blit_params& p) noexcept {
  if (!ctx || !vs_ || !background_ps_ || !cb_) return;

  bind_camera(ctx, p, p.image_w, p.image_h);
  ctx->VSSetShader(vs_.Get(), nullptr, 0);
  ctx->PSSetShader(background_ps_.Get(), nullptr, 0);
  ctx->Draw(3, 0);
}

void blitter::draw_tiles(ID3D11DeviceContext* ctx, std::span<const tile_quad> tiles,
                         const blit_params& p) noexcept {
  if (!ctx || tiles.empty() || !tile_vs_ || !tile_ps_ || !tile_layout_ || !tile_instances_) {
    return;
  }

  bind_camera(ctx, p, 260.0f, 260.0f);
  ctx->IASetInputLayout(tile_layout_.Get());
  ID3D11Buffer* vbs[] = {tile_instances_.Get()};
  const UINT strides[] = {static_cast<UINT>(sizeof(tile_instance))};
  const UINT offsets[] = {0};
  ctx->IASetVertexBuffers(0, 1, vbs, strides, offsets);
  ctx->VSSetShader(tile_vs_.Get(), nullptr, 0);
  ctx->PSSetShader(tile_ps_.Get(), nullptr, 0);
  // Issue #164: every record in one Map per batch (one per frame up to
  // k_tile_batch tiles), then one draw per tile for its own texture.
  for (std::size_t first = 0; first < tiles.size(); first += k_tile_batch) {
    const std::size_t n = std::min<std::size_t>(k_tile_batch, tiles.size() - first);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(tile_instances_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) break;
    auto* records = static_cast<tile_instance*>(mapped.pData);
    for (std::size_t i = 0; i < n; ++i) {
      const tile_quad& t = tiles[first + i];
      records[i] = tile_instance{t.origin_x,  t.origin_y,  t.scale_x, t.scale_y,
                                 t.content_w, t.content_h, 2.0f,      260.0f};
    }
    ctx->Unmap(tile_instances_.Get(), 0);
    for (std::size_t i = 0; i < n; ++i) {
      ID3D11ShaderResourceView* srv = tiles[first + i].srv;
      if (!srv) continue;
      ID3D11ShaderResourceView* srvs[] = {srv};
      ctx->PSSetShaderResources(0, 1, srvs);
      ctx->DrawInstanced(6, 1, 0, static_cast<UINT>(i));
    }
  }
  ID3D11ShaderResourceView* none[] = {nullptr};
  ctx->PSSetShaderResources(0, 1, none);
  // Every other pass draws without vertex input.
  ID3D11Buffer* no_vbs[] = {nullptr};
  const UINT zeros[] = {0};
  ctx->IASetVertexBuffers(0, 1, no_vbs, zeros, zeros);
  ctx->IASetInputLayout(nullptr);
}

}  // namespace mv::gfx
