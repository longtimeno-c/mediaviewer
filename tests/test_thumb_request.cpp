// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 15: the Explorer thumbnail handler's portable half (shellext/thumb_request).
// The COM wrapper is Windows-only; everything it decides is decided here.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

#include "codec/raster.h"
#include "edit/encode.h"
#include "shellext/thumb_request.h"

namespace {

std::vector<std::uint8_t> png(std::uint32_t w, std::uint32_t h, std::uint8_t r, std::uint8_t g,
                              std::uint8_t b, std::uint8_t a) {
  mv::codec::raster img;
  img.width = w;
  img.height = h;
  img.rgba.resize(static_cast<std::size_t>(w) * h * 4u);
  for (std::size_t i = 0; i < img.rgba.size(); i += 4) {
    img.rgba[i] = r;
    img.rgba[i + 1] = g;
    img.rgba[i + 2] = b;
    img.rgba[i + 3] = a;
  }
  mv::edit::encode_options opt;
  opt.format = mv::edit::image_format::png;
  auto bytes = mv::edit::encode(img, opt, {});
  REQUIRE(bytes);
  return std::move(bytes).value();
}

}  // namespace

TEST_CASE("an Explorer thumbnail is BGRA at the asked size", "[shellext]") {
  const auto src = png(600, 300, 200, 100, 50, 255);
  auto t = mv::shellext::render_thumbnail(src, 96);
  REQUIRE(t);
  CHECK(t->width == 96);
  CHECK(t->height == 48);
  CHECK_FALSE(t->has_alpha);
  REQUIRE(t->bgra.size() == 96u * 48u * 4u);
  CHECK(t->bgra[0] == 50);   // B
  CHECK(t->bgra[1] == 100);  // G
  CHECK(t->bgra[2] == 200);  // R
  CHECK(t->bgra[3] == 255);
}

TEST_CASE("a transparent source is premultiplied for WTSAT_ARGB", "[shellext]") {
  const auto src = png(32, 32, 200, 200, 200, 128);
  auto t = mv::shellext::render_thumbnail(src, 256);
  REQUIRE(t);
  CHECK(t->has_alpha);
  CHECK(t->width == 32);  // never enlarged
  CHECK(t->bgra[3] == 128);
  CHECK(t->bgra[0] <= 128);  // 200 * 128 / 255, give or take the colour stage
}

TEST_CASE("the size is clamped, never refused", "[shellext]") {
  const auto src = png(4000, 2000, 10, 20, 30, 255);
  auto big = mv::shellext::render_thumbnail(src, 100000);
  REQUIRE(big);
  CHECK(big->width == mv::shellext::kMaxThumbEdge);
  auto tiny = mv::shellext::render_thumbnail(src, 1);
  REQUIRE(tiny);
  CHECK(tiny->width == 16);
}

TEST_CASE("junk and empty input are no thumbnail, not a crash", "[shellext]") {
  const std::vector<std::uint8_t> junk(10000, 0xA5);
  CHECK_FALSE(mv::shellext::render_thumbnail(junk, 256));
  CHECK_FALSE(mv::shellext::render_thumbnail({}, 256));
  // A truncated file: a valid header and nothing behind it.
  auto src = png(300, 300, 1, 2, 3, 255);
  src.resize(src.size() / 3);
  CHECK_FALSE(mv::shellext::render_thumbnail(src, 256));
}

TEST_CASE("the deadline path answers, and abandons a late decode", "[shellext]") {
  auto ok = mv::shellext::render_thumbnail_by(png(64, 64, 9, 9, 9, 255), 32, std::chrono::seconds(10));
  REQUIRE(ok);
  CHECK(ok->width == 32);
  // No time at all: the answer is "no thumbnail", and the abandoned decode
  // finishes on its own thread without touching anything of the caller's.
  auto late = mv::shellext::render_thumbnail_by(png(3000, 3000, 9, 9, 9, 255), 256,
                                                std::chrono::milliseconds(0));
  CHECK_FALSE(late);
}
