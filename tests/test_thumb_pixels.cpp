// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// image::make_thumb_rgba: the pixels behind the app's JPEG-512 thumbnails, the
// Quick Look extension and (PR 15) the Explorer thumbnail handler.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "codec/decode.h"
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
