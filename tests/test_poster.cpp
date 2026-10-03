// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// player/poster: a clip's tile is turned the way the player shows it. A
// portrait phone clip is coded landscape with a 90 degree display matrix in
// its container; under jpg512.2 its poster (and the player) ignored it, so
// the clip was sideways everywhere but in Photos. Synthetic clips, no corpus.
#include "catch_compat.h"

#include <cmath>
#include <cstdint>
#include <string>

#include "clip_fixture.h"
#include "import_fixture.h"
#include "player/poster.h"

using namespace mv::test;
namespace fx = mv::test::clipfx;

namespace {

// The fixture's frames: a flat bottom half, noise on top. After the poster is
// made, how "flat" a region is tells which half of the coded frame it came
// from: the mean absolute difference between horizontal neighbours.
double roughness(const mv::player::poster_image& img, std::uint32_t x0, std::uint32_t y0,
                 std::uint32_t x1, std::uint32_t y1) {
  double sum = 0.0;
  std::uint64_t n = 0;
  for (std::uint32_t y = y0; y < y1; ++y) {
    for (std::uint32_t x = x0 + 1; x < x1; ++x) {
      const std::uint8_t* a = img.rgba.data() + (static_cast<std::size_t>(y) * img.width + x) * 4;
      const std::uint8_t* b = a - 4;
      sum += std::abs(static_cast<int>(a[1]) - static_cast<int>(b[1]));
      ++n;
    }
  }
  return n ? sum / static_cast<double>(n) : 0.0;
}

}  // namespace

TEST_CASE("a clip without a display matrix posters at its coded size", "[player][poster]") {
  scratch_dir d("poster_flat");
  const std::string path = utf8(d / "flat.mp4");
  fx::spec s;
  s.audio = false;
  s.frames = 30;
  REQUIRE(fx::make(path, s));
  auto poster = mv::player::poster_frame(path.c_str(), 512, nullptr);
  REQUIRE(poster);
  CHECK(poster->width == 320);
  CHECK(poster->height == 240);
  // The flat half is the bottom; the noise is on top.
  const double top = roughness(*poster, 0, 0, 320, 110);
  const double bottom = roughness(*poster, 0, 130, 320, 240);
  CHECK(top > 4.0);
  CHECK(bottom < 1.0);
}

TEST_CASE("a 90 degree display matrix turns the poster clockwise", "[player][poster][rotation]") {
  scratch_dir d("poster_turned");
  const std::string path = utf8(d / "portrait.mp4");
  fx::spec s;
  s.audio = false;
  s.frames = 30;
  s.rotation = 90;
  REQUIRE(fx::make(path, s));
  auto poster = mv::player::poster_frame(path.c_str(), 512, nullptr);
  REQUIRE(poster);
  // Coded 320x240, shown 240x320.
  CHECK(poster->width == 240);
  CHECK(poster->height == 320);
  // A clockwise quarter turn carries the coded bottom half to the LEFT half
  // of the picture (bottom-left corner -> top-left corner), the noisy top
  // half to the right.
  const double left = roughness(*poster, 0, 0, 110, 320);
  const double right = roughness(*poster, 130, 0, 240, 320);
  CHECK(left < 1.0);
  CHECK(right > 4.0);
}

TEST_CASE("a 180 degree display matrix flips the poster", "[player][poster][rotation]") {
  scratch_dir d("poster_upside_down");
  const std::string path = utf8(d / "flipped.mp4");
  fx::spec s;
  s.audio = false;
  s.frames = 30;
  s.rotation = 180;
  REQUIRE(fx::make(path, s));
  auto poster = mv::player::poster_frame(path.c_str(), 512, nullptr);
  REQUIRE(poster);
  CHECK(poster->width == 320);
  CHECK(poster->height == 240);
  // Upside down: the flat half is now on top.
  CHECK(roughness(*poster, 0, 0, 320, 110) < 1.0);
  CHECK(roughness(*poster, 0, 130, 320, 240) > 4.0);
}
