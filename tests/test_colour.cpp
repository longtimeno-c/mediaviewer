// SPDX-License-Identifier: GPL-2.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <vector>

#include "codec/decode.h"
#include "core/job_system.h"
#include "fixtures.h"
#include "image/colour.h"
#include "image/pipeline.h"

using mv::image::decode_bytes;

TEST_CASE("untagged pixels are treated as sRGB and not tone-mapped", "[colour][d6]") {
  // A mid-grey camera JPEG must come out as mid-grey. Reinhard/ACES would
  // shift it, which is the bug D6 exists to prevent.
  std::vector<std::uint8_t> rgb(16 * 16 * 3, 128);
  auto bytes = fixtures::jpeg_rgb(16, 16, rgb.data());
  auto img = decode_bytes(bytes);
  REQUIRE(img);
  REQUIRE_FALSE(img->icc_tagged);
  REQUIRE(img->intent == mv::codec::transfer_intent::display_referred);
  const int g = img->rgba[0];
  REQUIRE(std::abs(g - 128) <= 2);
  REQUIRE(img->rgba[1] == img->rgba[0]);
  REQUIRE(img->rgba[2] == img->rgba[0]);
}

TEST_CASE("a tagged AdobeRGB file does not decode as sRGB", "[colour][d6]") {
  constexpr int w = 8, h = 8;
  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w) * h * 3);
  for (std::size_t i = 0; i < rgb.size(); i += 3) {
    rgb[i] = 40;
    rgb[i + 1] = 200;
    rgb[i + 2] = 60;
  }

  auto untagged = fixtures::jpeg_rgb(w, h, rgb.data());
  auto tagged = fixtures::jpeg_rgb(w, h, rgb.data(), fixtures::adobe_rgb_icc());
  REQUIRE_FALSE(untagged.empty());
  REQUIRE_FALSE(tagged.empty());

  auto a = decode_bytes(untagged);
  auto b = decode_bytes(tagged);
  REQUIRE(a);
  REQUIRE(b);
  REQUIRE_FALSE(a->icc_tagged);
  REQUIRE(b->icc_tagged);

  // Same encoded bytes, different colour space: the display-referred convert
  // must move the green. If both come out identical, LCMS was skipped.
  int diff = 0;
  for (std::size_t i = 0; i < a->rgba.size(); i += 4) {
    diff += std::abs(static_cast<int>(a->rgba[i]) - static_cast<int>(b->rgba[i]));
    diff += std::abs(static_cast<int>(a->rgba[i + 1]) - static_cast<int>(b->rgba[i + 1]));
    diff += std::abs(static_cast<int>(a->rgba[i + 2]) - static_cast<int>(b->rgba[i + 2]));
  }
  REQUIRE(diff > 100);

  // No tone-map: values stay in 0..255 and a mid-bright green is still bright.
  REQUIRE(b->rgba[1] > 100);
}

TEST_CASE("a broken ICC profile fails rather than displaying as sRGB", "[colour][d6]") {
  constexpr int w = 8, h = 8;
  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w) * h * 3, 40);
  const std::vector<std::uint8_t> garbage{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
  auto tagged = fixtures::jpeg_rgb(w, h, rgb.data(), garbage);
  REQUIRE_FALSE(tagged.empty());
  auto img = decode_bytes(tagged);
  REQUIRE_FALSE(img);
  REQUIRE(img.error() == mv::status::corrupt);
}

TEST_CASE("a cancelled ICC transform does not fall back to untagged", "[colour]") {
  constexpr int w = 8, h = 8;
  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w) * h * 3, 40);
  auto tagged = fixtures::jpeg_rgb(w, h, rgb.data(), fixtures::adobe_rgb_icc());
  REQUIRE_FALSE(tagged.empty());
  auto raster = mv::codec::decode(tagged);
  REQUIRE(raster);
  std::atomic<mv::generation> current{2};
  mv::job_context ctx(1, 1, &current, 0);
  auto img = mv::image::to_display(std::move(raster).value(), &ctx);
  REQUIRE_FALSE(img);
  REQUIRE(img.error() == mv::status::cancelled);
}

TEST_CASE("PNG with iCCP AdobeRGB is tagged", "[colour][png]") {
  const std::uint8_t rgba[] = {40, 200, 60, 255, 40, 200, 60, 255,
                               40, 200, 60, 255, 40, 200, 60, 255};
  auto bytes = fixtures::png_rgba(2, 2, rgba, fixtures::adobe_rgb_icc());
  auto img = decode_bytes(bytes);
  REQUIRE(img);
  REQUIRE(img->icc_tagged);
  REQUIRE(img->format == mv::codec::format_family::png);
}
