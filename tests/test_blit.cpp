// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The tile blit on WARP (issue #164): every tile's record goes up in one
// instance-buffer write per batch and each tile draws from its own record and
// texture; the background-only base pass stands in for the overview.
#include <catch2/catch_test_macros.hpp>

#include <d3d11.h>

#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "gfx/blit.h"
#include "gfx/device.h"
#include "gfx/texture.h"

namespace {

using mv::gfx::com_ptr;

// A 260x260 single-colour tile texture (the tile path's size: 256 + 2-texel border).
com_ptr<ID3D11ShaderResourceView> solid_tile(ID3D11Device* device, std::uint8_t r,
                                             std::uint8_t g, std::uint8_t b) {
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(260) * 260 * 4);
  for (std::size_t i = 0; i < rgba.size(); i += 4) {
    rgba[i] = r;
    rgba[i + 1] = g;
    rgba[i + 2] = b;
    rgba[i + 3] = 255;
  }
  const mv::gfx::rgba8_level level{rgba.data(), 260, 260};
  com_ptr<ID3D11Texture2D> texture;
  com_ptr<ID3D11ShaderResourceView> srv;
  const std::span<const mv::gfx::rgba8_level> levels(&level, 1);
  if (mv::gfx::create_srgb_texture(device, levels, texture, srv) != mv::status::ok) {
    return {};
  }
  return srv;
}

bool close_to(int a, int b) { return std::abs(a - b) <= 2; }

}  // namespace

TEST_CASE("tiles draw from one instance buffer, batched past its capacity", "[blit][gpu]") {
  com_ptr<ID3D11Device> device;
  com_ptr<ID3D11DeviceContext> ctx;
  D3D_FEATURE_LEVEL level{};
  const HRESULT hr =
      ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                          D3D11_SDK_VERSION, device.GetAddressOf(), &level, ctx.GetAddressOf());
  if (FAILED(hr)) SKIP("WARP is unavailable");

  // A 66x66 image at 100 % in a 70x66 view: image pixel == screen pixel, and
  // the four columns right of it are background.
  constexpr UINT kW = 70;
  constexpr UINT kH = 66;
  constexpr int kGrid = 33;  // 33 x 33 tiles of 2 x 2 px: 1089, past one 1024-record batch

  D3D11_TEXTURE2D_DESC td{};
  td.Width = kW;
  td.Height = kH;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  td.SampleDesc = {1, 0};
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  com_ptr<ID3D11Texture2D> target;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&td, nullptr, target.GetAddressOf())));
  com_ptr<ID3D11RenderTargetView> rtv;
  REQUIRE(SUCCEEDED(device->CreateRenderTargetView(target.Get(), nullptr, rtv.GetAddressOf())));
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  com_ptr<ID3D11Texture2D> staging;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&td, nullptr, staging.GetAddressOf())));

  mv::gfx::blitter blitter;
  REQUIRE(blitter.create(device.Get()));

  const auto red = solid_tile(device.Get(), 255, 0, 0);
  const auto blue = solid_tile(device.Get(), 0, 0, 255);
  REQUIRE(red);
  REQUIRE(blue);

  // Tile (0, 0) has no texture yet: it is skipped, and the records after it
  // must still line up with their tiles.
  std::vector<mv::gfx::tile_quad> tiles;
  for (int ty = 0; ty < kGrid; ++ty) {
    for (int tx = 0; tx < kGrid; ++tx) {
      mv::gfx::tile_quad q;
      q.srv = (tx == 0 && ty == 0) ? nullptr : ((tx + ty) % 2 == 0 ? red.Get() : blue.Get());
      q.origin_x = static_cast<float>(tx * 2);
      q.origin_y = static_cast<float>(ty * 2);
      q.content_w = 2.0f;
      q.content_h = 2.0f;
      tiles.push_back(q);
    }
  }

  mv::gfx::blit_params p;
  p.pan_x = static_cast<float>(kW) * 0.5f;
  p.pan_y = static_cast<float>(kH) * 0.5f;
  p.zoom = 1.0f;
  p.window_w = static_cast<float>(kW);
  p.window_h = static_cast<float>(kH);
  p.image_w = 66.0f;
  p.image_h = 66.0f;
  // White, so the background pass is told apart from the black clear (#408:
  // the theme background is the only canvas background).
  for (float& c : p.theme_background) c = 1.0f;
  p.pixel_grid = false;

  ID3D11RenderTargetView* rtvs[] = {rtv.Get()};
  ctx->OMSetRenderTargets(1, rtvs, nullptr);
  const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH), 0.0f, 1.0f};
  ctx->RSSetViewports(1, &vp);
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  ctx->ClearRenderTargetView(rtv.Get(), black);

  blitter.draw_background(ctx.Get(), p);
  blitter.draw_tiles(ctx.Get(), tiles, p);
  ctx->CopyResource(staging.Get(), target.Get());

  D3D11_MAPPED_SUBRESOURCE mapped{};
  REQUIRE(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
  const auto* base = static_cast<const std::uint8_t*>(mapped.pData);
  auto px = [&](UINT x, UINT y) { return base + y * mapped.RowPitch + x * 4; };

  int wrong = 0;
  for (int ty = 0; ty < kGrid; ++ty) {
    for (int tx = 0; tx < kGrid; ++tx) {
      const std::uint8_t* c = px(static_cast<UINT>(tx * 2 + 1), static_cast<UINT>(ty * 2 + 1));
      bool ok = false;
      if (tx == 0 && ty == 0) {
        ok = close_to(c[0], 255) && close_to(c[1], 255) && close_to(c[2], 255);  // background
      } else if ((tx + ty) % 2 == 0) {
        ok = close_to(c[0], 255) && close_to(c[1], 0) && close_to(c[2], 0);
      } else {
        ok = close_to(c[0], 0) && close_to(c[1], 0) && close_to(c[2], 255);
      }
      if (!ok) ++wrong;
    }
  }
  CHECK(wrong == 0);
  // Right of the image: the background pass, not the clear.
  for (UINT y = 0; y < kH; y += 13) {
    const std::uint8_t* c = px(kW - 1, y);
    CHECK((close_to(c[0], 255) && close_to(c[1], 255) && close_to(c[2], 255)));
  }
  ctx->Unmap(staging.Get(), 0);

  // The tile pass leaves no vertex input behind for the passes after it.
  com_ptr<ID3D11InputLayout> layout;
  ctx->IAGetInputLayout(layout.GetAddressOf());
  CHECK(layout == nullptr);
}
