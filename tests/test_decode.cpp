// SPDX-License-Identifier: GPL-2.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdlib>
#include <vector>

#include "codec/decode.h"
#include "core/job_system.h"
#include "fixtures.h"

using mv::codec::decode;
using mv::codec::decode_jpeg;
using mv::codec::decode_png;
using mv::codec::format_family;

TEST_CASE("BMP 24-bit round-trips BGR to RGBA", "[codec][bmp]") {
  const std::uint8_t rgba[] = {
      255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255,
  };
  auto bytes = fixtures::bmp_rgba(2, 2, rgba);
  auto img = decode(bytes);
  REQUIRE(img);
  REQUIRE(img->format == format_family::bmp);
  REQUIRE(img->width == 2);
  REQUIRE(img->height == 2);
  REQUIRE(img->icc.empty());
  REQUIRE(img->rgba.size() == 16);
  REQUIRE(img->rgba[0] == 255);
  REQUIRE(img->rgba[1] == 0);
  REQUIRE(img->rgba[2] == 0);
  REQUIRE(img->rgba[4] == 0);
  REQUIRE(img->rgba[5] == 255);
}

TEST_CASE("PNG round-trips RGBA", "[codec][png]") {
  const std::uint8_t rgba[] = {10, 20, 30, 255, 40, 50, 60, 128};
  auto bytes = fixtures::png_rgba(2, 1, rgba);
  REQUIRE_FALSE(bytes.empty());
  auto img = decode(bytes);
  REQUIRE(img);
  REQUIRE(img->format == format_family::png);
  REQUIRE(img->width == 2);
  REQUIRE(img->height == 1);
  REQUIRE(img->rgba[0] == 10);
  REQUIRE(img->rgba[1] == 20);
  REQUIRE(img->rgba[2] == 30);
  REQUIRE(img->rgba[3] == 255);
  REQUIRE(img->rgba[7] == 128);
}

TEST_CASE("JPEG round-trips a solid colour within encoder noise", "[codec][jpeg]") {
  std::vector<std::uint8_t> rgb(8 * 8 * 3, 0);
  for (std::size_t i = 0; i < rgb.size(); i += 3) {
    rgb[i] = 200;
    rgb[i + 1] = 30;
    rgb[i + 2] = 40;
  }
  auto bytes = fixtures::jpeg_rgb(8, 8, rgb.data());
  REQUIRE_FALSE(bytes.empty());
  auto img = decode(bytes);
  REQUIRE(img);
  REQUIRE(img->format == format_family::jpeg);
  REQUIRE(img->width == 8);
  REQUIRE(img->height == 8);
  REQUIRE(img->rgba[3] == 255);
  REQUIRE(std::abs(static_cast<int>(img->rgba[0]) - 200) < 8);
  REQUIRE(std::abs(static_cast<int>(img->rgba[1]) - 30) < 8);
}

TEST_CASE("unknown magic is unsupported, not corrupt", "[codec]") {
  const std::uint8_t garbage[] = {0, 1, 2, 3, 4, 5, 6, 7};
  auto img = decode(garbage);
  REQUIRE_FALSE(img);
  REQUIRE(img.error() == mv::status::unsupported_format);
}

TEST_CASE("JPEG DCT 1/4 is a quarter of 1:1", "[codec][jpeg]") {
  std::vector<std::uint8_t> rgb(32 * 32 * 3, 80);
  auto bytes = fixtures::jpeg_rgb(32, 32, rgb.data());
  REQUIRE_FALSE(bytes.empty());
  auto full = decode_jpeg(bytes);
  auto quarter = decode_jpeg(bytes, nullptr, 4);
  REQUIRE(full);
  REQUIRE(quarter);
  REQUIRE(full->width == 32);
  REQUIRE(full->height == 32);
  REQUIRE(quarter->width == 8);
  REQUIRE(quarter->height == 8);
}

TEST_CASE("a cancelled PNG decode stops before finishing", "[codec][png]") {
  const std::uint8_t rgba[] = {10, 20, 30, 255, 40, 50, 60, 128};
  auto bytes = fixtures::png_rgba(2, 1, rgba);
  REQUIRE_FALSE(bytes.empty());
  std::atomic<mv::generation> current{2};
  mv::job_context ctx(1, 1, &current, 0);
  auto img = decode_png(bytes, &ctx);
  REQUIRE_FALSE(img);
  REQUIRE(img.error() == mv::status::cancelled);
}

TEST_CASE("truncated JPEG is corrupt", "[codec][jpeg]") {
  const std::uint8_t truncated[] = {0xFF, 0xD8, 0xFF};
  auto img = decode(truncated);
  REQUIRE_FALSE(img);
  REQUIRE(img.error() == mv::status::corrupt);
}
