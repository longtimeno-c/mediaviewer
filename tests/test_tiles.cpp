// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Tiled pyramid for large images (plan/04): geometry, CPU pyramid, and the
// request / create / evict cycle against WARP.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <d3d11.h>

#include <chrono>
#include <thread>
#include <vector>

#include "gfx/device.h"
#include "image/tiles.h"
#include "image/upload.h"

using Catch::Matchers::WithinAbs;
using mv::image::k_tile_size;
using mv::image::needs_tiles;
using mv::image::tile_layout;

namespace {

// The WARP test below bounds how long a frame call and a tile create may take.
// Those numbers describe optimised code: an unoptimised build on a software
// rasteriser measures the debug CRT and iterator checking, not the tile
// service, and CI has only just started running this suite in Debug. Scale
// them by configuration rather than delete them — the same call
// tests/test_broken_corpus.cpp makes for its per-call timeout. The real
// pacing gate is tools/frametime on a GPU runner (D6), not this test.
#if defined(__SANITIZE_ADDRESS__)
constexpr bool kAsan = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool kAsan = true;
#else
constexpr bool kAsan = false;
#endif
#else
constexpr bool kAsan = false;
#endif

#if defined(NDEBUG)
constexpr bool kOptimised = !kAsan;
#else
constexpr bool kOptimised = false;
#endif

// One frame call: 2 ms optimised. It must never block on a tile either way.
constexpr long long kFrameBudgetUs = kOptimised ? 2000 : 40000;
// One tile create, against the service's per-tick cap.
constexpr long long kCreateBudgetUs = kOptimised ? 20000 : 400000;
// How long the visible tiles have to land at all. A bound on progress, not
// on speed: the assertions after it are what the test is for.
constexpr int kLandSeconds = kOptimised ? 20 : 90;

}  // namespace

TEST_CASE("tiling starts above ~64 MP or past the texture limit", "[tiles]") {
  REQUIRE_FALSE(needs_tiles(8000, 8000));    // 64 MP exactly
  REQUIRE(needs_tiles(8001, 8000));
  REQUIRE(needs_tiles(12000, 8400));         // 100 MP
  REQUIRE(needs_tiles(20000, 1000));         // 20 MP but 20000 wide
  REQUIRE_FALSE(needs_tiles(16384, 1000));
  REQUIRE(needs_tiles(1000, 16385));
  REQUIRE_FALSE(needs_tiles(6000, 4000));
}

TEST_CASE("layout: floor-half levels, overview <= 2048, 256 px tiles", "[tiles]") {
  const tile_layout L = tile_layout::make(12000, 8400);
  REQUIRE(L.level(0).width == 12000);
  REQUIRE(L.level(1).width == 6000);
  REQUIRE(L.level(3).width == 1500);
  REQUIRE(L.level(3).height == 1050);
  REQUIRE(L.overview_level() == 3);
  REQUIRE(L.level(L.level_count() - 1).width == 1);
  REQUIRE(L.level(L.level_count() - 1).height == 1);
  REQUIRE(L.level(0).tiles_x == 47);  // ceil(12000 / 256)
  REQUIRE(L.level(0).tiles_y == 33);
  REQUIRE(L.level(1).first_index == 47 * 33);
  REQUIRE(L.tile_count() == 47 * 33 + 24 * 17 + 12 * 9);

  // Odd sizes floor, and the level still covers the whole image rect.
  const tile_layout odd = tile_layout::make(20001, 999);
  REQUIRE(odd.level(1).width == 10000);
  REQUIRE(odd.level(1).height == 499);
  REQUIRE_THAT(odd.scale_x(1) * 10000.0f, WithinAbs(20001.0f, 0.01f));

  const tile_layout pano = tile_layout::make(20000, 1000);
  REQUIRE(pano.overview_level() == 4);  // 1250 x 62
  REQUIRE(pano.level(0).tiles_y == 4);
}

TEST_CASE("lod: the coarsest level still at >= one texel per screen pixel", "[tiles]") {
  const tile_layout L = tile_layout::make(12000, 8400);
  REQUIRE(L.lod_for_zoom(1.0f) == 0);
  REQUIRE(L.lod_for_zoom(4.0f) == 0);
  REQUIRE(L.lod_for_zoom(0.75f) == 0);  // minified 1.33x: level 0's tile mip
  REQUIRE(L.lod_for_zoom(0.5f) == 1);
  REQUIRE(L.lod_for_zoom(0.3f) == 1);
  REQUIRE(L.lod_for_zoom(0.25f) == 2);
  REQUIRE(L.lod_for_zoom(0.13f) == 2);
  // At or past the overview's density no tiles are wanted.
  REQUIRE(L.lod_for_zoom(0.125f) == 3);
  REQUIRE(L.lod_for_zoom(0.05f) == 3);
}

TEST_CASE("visible: tiles under the view, grown by the ring, clamped", "[tiles]") {
  const tile_layout L = tile_layout::make(12000, 8400);
  // 100 %, 1024x512 view centred on (6000, 4200): x 5488..6512, y 3944..4456.
  const auto r = L.visible(0, 6000.0f, 4200.0f, 1.0f, 1024.0f, 512.0f, 0);
  REQUIRE(r.x0 == 5488 / k_tile_size);
  REQUIRE(r.x1 == 6512 / k_tile_size + 1);
  REQUIRE(r.y0 == 3944 / k_tile_size);
  REQUIRE(r.y1 == 4456 / k_tile_size + 1);
  const auto ring = L.visible(0, 6000.0f, 4200.0f, 1.0f, 1024.0f, 512.0f, 1);
  REQUIRE(ring.x0 == r.x0 - 1);
  REQUIRE(ring.x1 == r.x1 + 1);
  // Top-left corner clamps at zero.
  const auto corner = L.visible(0, 100.0f, 100.0f, 1.0f, 1024.0f, 512.0f, 1);
  REQUIRE(corner.x0 == 0);
  REQUIRE(corner.y0 == 0);
  // Fitted far out: past the overview, nothing.
  REQUIRE(L.visible(3, 6000.0f, 4200.0f, 0.1f, 1200.0f, 840.0f, 1).empty());
  // Every tile range stays inside its level.
  const auto whole = L.visible(2, 6000.0f, 4200.0f, 0.2f, 4000.0f, 4000.0f, 1);
  REQUIRE(whole.x1 == L.level(2).tiles_x);
  REQUIRE(whole.y1 == L.level(2).tiles_y);
}

namespace {

std::shared_ptr<mv::image::display_image> gradient(std::uint32_t w, std::uint32_t h) {
  auto img = std::make_shared<mv::image::display_image>();
  img->width = w;
  img->height = h;
  img->rgba.resize(static_cast<std::size_t>(w) * h * 4);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      std::uint8_t* p = img->rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4;
      p[0] = static_cast<std::uint8_t>(x * 255 / (w - 1));
      p[1] = static_cast<std::uint8_t>(y * 255 / (h - 1));
      p[2] = 128;
      p[3] = 255;
    }
  }
  return img;
}

}  // namespace

TEST_CASE("CPU pyramid and tile extraction with a clamped border", "[tiles]") {
  auto full = gradient(20000, 600);  // wider than any texture
  auto built = mv::image::tile_source::build(full);
  REQUIRE(built);
  const auto& src = *built.value();
  REQUIRE(src.layout().level_count() > 4);
  // Level 0 is the decoded image, not a copy.
  REQUIRE(src.level_rgba(0) == full->rgba.data());

  std::vector<std::uint8_t> mip0, mip1;
  src.extract_tile(0, 0, 0, mip0, mip1);
  REQUIRE(mip0.size() == 260u * 260u * 4u);
  REQUIRE(mip1.size() == 130u * 130u * 4u);
  // Texel (2,2) is level pixel (0,0); the border repeats it.
  REQUIRE(mip0[(2 * 260 + 2) * 4] == full->rgba[0]);
  REQUIRE(mip0[0] == full->rgba[0]);
  // Texel (3,2) is level pixel (1,0).
  REQUIRE(mip0[(2 * 260 + 3) * 4] == full->rgba[4]);

  // The last column's content runs past the image edge; it clamps.
  const auto& lv = src.layout().level(0);
  const std::uint32_t last = lv.tiles_x - 1;
  src.extract_tile(0, last, 0, mip0, mip1);
  const std::uint32_t last_px = 19999;
  const std::uint32_t local = last_px - last * 256 + 2;
  REQUIRE(mip0[(2 * 260 + local) * 4] == full->rgba[static_cast<std::size_t>(last_px) * 4]);
  REQUIRE(mip0[(2 * 260 + 259) * 4] == full->rgba[static_cast<std::size_t>(last_px) * 4]);
}

TEST_CASE("mean luma of a sparse sample", "[tiles]") {
  mv::image::display_image img;
  img.width = 300;
  img.height = 200;
  img.rgba.assign(300u * 200u * 4u, 100);
  REQUIRE(mv::image::mean_luma(img) == 100);
  img.rgba.assign(300u * 200u * 4u, 255);
  REQUIRE(mv::image::mean_luma(img) == 255);
}

TEST_CASE("tiled upload on WARP: overview, on-demand tiles, budget and cancel", "[tiles][gpu]") {
  mv::gfx::com_ptr<ID3D11Device> device;
  D3D_FEATURE_LEVEL level{};
  const HRESULT hr =
      ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                          D3D11_SDK_VERSION, device.GetAddressOf(), &level, nullptr);
  if (FAILED(hr)) SKIP("WARP is unavailable");

  mv::job_system jobs;
  REQUIRE(jobs.start(1) == mv::status::ok);
  std::atomic<int> landed{0};
  mv::image::tile_service service(
      &jobs, [](void* u) noexcept { static_cast<std::atomic<int>*>(u)->fetch_add(1); }, &landed);

  auto full = gradient(20000, 1000);
  auto gpu = mv::image::upload_tiled(device.Get(), full, jobs.current_generation(), service);
  REQUIRE(gpu);
  REQUIRE(gpu->width == 20000);
  REQUIRE(gpu->height == 1000);
  REQUIRE(gpu->tiles);
  REQUIRE(gpu->texture_width <= mv::image::k_overview_max_edge);
  D3D11_TEXTURE2D_DESC desc{};
  gpu->texture->GetDesc(&desc);
  REQUIRE(desc.Width == gpu->texture_width);

  auto& set = *gpu->tiles;
  std::vector<mv::gfx::tile_quad> draws;
  draws.reserve(256);
  mv::image::tile_view view{10000.0f, 500.0f, 1.0f, 1280.0f, 720.0f};

  // First frame: nothing is ready, requests go out, the render side does not wait.
  const auto t0 = std::chrono::steady_clock::now();
  set.frame(view, draws);
  const auto frame_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
  REQUIRE(draws.empty());
  REQUIRE(set.pending());
  CHECK(frame_us < kFrameBudgetUs);

  // Keep "presenting" until the visible tiles landed.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kLandSeconds);
  const auto visible = set.layout().visible(0, view.pan_x, view.pan_y, view.zoom, view.view_w,
                                            view.view_h, 0);
  const std::size_t want = static_cast<std::size_t>(visible.x1 - visible.x0) *
                           (visible.y1 - visible.y0);
  while (std::chrono::steady_clock::now() < deadline) {
    set.frame(view, draws);
    if (draws.size() >= want && !set.pending()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  REQUIRE(draws.size() >= want);
  REQUIRE(landed.load() > 0);
  for (const auto& q : draws) {
    REQUIRE(q.srv != nullptr);
    REQUIRE(q.content_w <= 256.0f);
  }
  const auto stats = set.stats();
  REQUIRE(stats.created >= want);
  REQUIRE(stats.vram_bytes <= mv::image::k_tile_vram_budget);
  // Per refresh interval the service creates at most k_tiles_per_tick tiles.
  CHECK(stats.last_create_us < kCreateBudgetUs);

  // A navigation bump stops the service creating for this set.
  jobs.bump_generation();
  const auto created_before = set.stats().created;
  view.pan_x = 2000.0f;
  set.frame(view, draws);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  REQUIRE(set.stats().created == created_before);

  gpu = mv::err(mv::status::cancelled);
  jobs.shutdown();
}
