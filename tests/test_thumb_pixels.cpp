// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// image::make_thumb_rgba: the pixels behind the app's JPEG-512 thumbnails, the
// Quick Look extension and (PR 15) the Explorer thumbnail handler.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "codec/decode.h"
#include "fixtures.h"
#include "image/thumb.h"

namespace {

std::vector<std::uint8_t> grey_jpeg(std::uint32_t w, std::uint32_t h) {
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4u, 128);
  auto bytes = mv::codec::encode_jpeg_rgba(rgba, w, h, 90);
  REQUIRE(bytes);
  return std::move(bytes).value();
}

}  // namespace

TEST_CASE("thumbnail pixels fit the long edge and keep the aspect", "[image][thumb]") {
  const auto jpeg = grey_jpeg(1200, 800);
  auto t = mv::image::make_thumb_rgba(jpeg, 256);
  REQUIRE(t);
  CHECK(t->width == 256);
  CHECK(t->height == 170);
  CHECK(t->rgba.size() == static_cast<std::size_t>(t->width) * t->height * 4u);
  CHECK(t->rgba[3] == 255);  // opaque
}

TEST_CASE("thumbnail pixels are never enlarged", "[image][thumb]") {
  const auto jpeg = grey_jpeg(100, 50);
  auto t = mv::image::make_thumb_rgba(jpeg, 256);
  REQUIRE(t);
  CHECK(t->width == 100);
  CHECK(t->height == 50);
}

TEST_CASE("thumbnail pixels refuse what is not an image", "[image][thumb]") {
  const std::vector<std::uint8_t> junk(4096, 0x5A);
  CHECK_FALSE(mv::image::make_thumb_rgba(junk, 256));
  CHECK_FALSE(mv::image::make_thumb_rgba(grey_jpeg(64, 64), 0));
}

TEST_CASE("the JPEG-512 thumbnail is the pixels, encoded", "[image][thumb]") {
  const auto jpeg = grey_jpeg(2048, 1024);
  auto t = mv::image::make_thumb_jpeg(jpeg);
  REQUIRE(t);
  auto size = mv::codec::jpeg_dimensions(t.value());
  REQUIRE(size);
  CHECK(size->width == mv::image::kThumbLongEdge);
  CHECK(size->height == mv::image::kThumbLongEdge / 2);
}

// Issue #169: straight alpha. Odd columns are transparent with garbage green
// under them; a 4:1 box must average only the covered red, and the JPEG must
// show the matte, not the RGB under a transparent pixel.
TEST_CASE("thumbnail colour is weighted by alpha", "[image][thumb]") {
  constexpr std::uint32_t w = 1024, h = 512;
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4u);
  for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
    const bool covered = (i % w) % 2 == 0;
    rgba[i * 4 + 0] = covered ? 255 : 0;
    rgba[i * 4 + 1] = covered ? 0 : 255;
    rgba[i * 4 + 2] = 0;
    rgba[i * 4 + 3] = covered ? 255 : 0;
  }
  const auto png = fixtures::png_rgba(w, h, rgba.data());
  REQUIRE_FALSE(png.empty());
  auto t = mv::image::make_thumb_rgba(png, 256);
  REQUIRE(t);
  REQUIRE(t->width == 256);
  CHECK(t->rgba[0] == 255);
  CHECK(t->rgba[1] == 0);
  CHECK(t->rgba[3] >= 127);
  CHECK(t->rgba[3] <= 128);
}

TEST_CASE("a transparent thumbnail is composited onto the matte", "[image][thumb]") {
  mv::image::thumb_pixels t{2, 1, {200, 10, 90, 255, 255, 255, 255, 0}};
  mv::image::composite_thumb_matte(t);
  CHECK(t.rgba == std::vector<std::uint8_t>{200, 10, 90, 255, mv::image::kThumbMatte,
                                            mv::image::kThumbMatte, mv::image::kThumbMatte, 255});

  // End to end: a clear PNG with white under it encodes as the matte.
  std::vector<std::uint8_t> clear(64u * 64u * 4u, 255);
  for (std::size_t i = 3; i < clear.size(); i += 4) clear[i] = 0;
  const auto png = fixtures::png_rgba(64, 64, clear.data());
  REQUIRE_FALSE(png.empty());
  auto jpeg = mv::image::make_thumb_jpeg(png);
  REQUIRE(jpeg);
  auto back = mv::image::make_thumb_rgba(jpeg.value(), 64);
  REQUIRE(back);
  CHECK(back->rgba[0] >= mv::image::kThumbMatte - 3);
  CHECK(back->rgba[0] <= mv::image::kThumbMatte + 3);
}

// Milestone H: a clip moment's cache row. The AI pack writes it (addon_media
// moment_thumbnail) and both hosts read it for result tiles and the clip's
// first pixel, so the spelling is one function and must not drift.
TEST_CASE("a moment's thumbnail row sits beside the clip's poster", "[image][thumb]") {
  const mv::image::thumb_key poster{"/Volumes/Card/DCIM/CLIP0001.MP4", 1'700'000'000, 123456789};
  const mv::image::thumb_key moment =
      mv::image::moment_thumb_key(poster.path, 83'250, poster.mtime_unix, poster.size);
  CHECK(moment.path == "/Volumes/Card/DCIM/CLIP0001.MP4#t=83250");
  // The file's own stamp: a clip edited since invalidates its moments too.
  CHECK(moment.mtime_unix == poster.mtime_unix);
  CHECK(moment.size == poster.size);
  CHECK(mv::image::moment_thumb_key(poster.path, 0, 1, 2).path ==
        "/Volumes/Card/DCIM/CLIP0001.MP4#t=0");
  CHECK(mv::image::moment_thumb_key(poster.path, 83'251, 1, 2).path != moment.path);
}
