// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <d3d11.h>
#include <vector>

#include "gfx/device.h"
#include "image/upload.h"

TEST_CASE("odd dimensions produce a floor-half D3D mip chain", "[upload]") {
  mv::gfx::com_ptr<ID3D11Device> device;
  D3D_FEATURE_LEVEL level{};
  const HRESULT hr =
      ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                          D3D11_SDK_VERSION, device.GetAddressOf(), &level, nullptr);
  if (FAILED(hr)) SKIP("WARP is unavailable");

  mv::image::display_image src;
  src.width = 5;
  src.height = 5;
  src.rgba.assign(static_cast<std::size_t>(5) * 5 * 4, 128);

  auto gpu = mv::image::upload(device.Get(), src, 1);
  REQUIRE(gpu);
  // 5 → 2 → 1. Ceil (3×3) would be a pitch D3D11 does not expect.
  REQUIRE(gpu->mip_levels == 3);
  D3D11_TEXTURE2D_DESC desc{};
  gpu->texture->GetDesc(&desc);
  REQUIRE(desc.Width == 5);
  REQUIRE(desc.Height == 5);
  REQUIRE(desc.MipLevels == 3);
  REQUIRE(gpu->device.Get() == device.Get());
}

TEST_CASE("single-texture uploads are timed", "[upload]") {
  // Issue #166: the whole-texture create of an untiled still had no
  // measurement, unlike tiles' last_create_us.
  mv::gfx::com_ptr<ID3D11Device> device;
  D3D_FEATURE_LEVEL level{};
  const HRESULT hr =
      ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                          D3D11_SDK_VERSION, device.GetAddressOf(), &level, nullptr);
  if (FAILED(hr)) SKIP("WARP is unavailable");

  const auto before = mv::image::texture_upload_stats_now();
  mv::image::display_image src;
  src.width = 64;
  src.height = 32;
  src.rgba.assign(static_cast<std::size_t>(64) * 32 * 4, 200);
  auto gpu = mv::image::upload(device.Get(), src, 1);
  REQUIRE(gpu);
  const auto after = mv::image::texture_upload_stats_now();
  CHECK(after.creates == before.creates + 1);
  CHECK(after.max_create_us >= after.last_create_us);
  CHECK(after.max_create_us >= before.max_create_us);
}
