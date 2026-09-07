// SPDX-License-Identifier: GPL-2.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

#include "codec/format.h"

using mv::codec::format_family;
using mv::codec::probe;

TEST_CASE("probe is by magic bytes, never by length of a guess", "[codec][probe]") {
  REQUIRE(probe({}) == format_family::unknown);
  REQUIRE(probe(std::array<std::uint8_t, 1>{0xFF}) == format_family::unknown);

  const std::uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0};
  REQUIRE(probe(jpeg) == format_family::jpeg);

  const std::uint8_t png[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  REQUIRE(probe(png) == format_family::png);

  const std::uint8_t bmp[] = {'B', 'M', 0, 0, 0, 0};
  REQUIRE(probe(bmp) == format_family::bmp);

  const std::uint8_t tiff_le[] = {'I', 'I', 42, 0};  // PR 7, not yet
  REQUIRE(probe(tiff_le) == format_family::unknown);

  const std::uint8_t webp[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
  REQUIRE(probe(webp) == format_family::unknown);
}
