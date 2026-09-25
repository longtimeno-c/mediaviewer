// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
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

  const std::uint8_t tiff_le[] = {'I', 'I', 42, 0};
  REQUIRE(probe(tiff_le) == format_family::tiff);
  const std::uint8_t tiff_be[] = {'M', 'M', 0, 42};
  REQUIRE(probe(tiff_be) == format_family::tiff);

  const std::uint8_t ico[] = {0, 0, 1, 0, 1, 0};
  REQUIRE(probe(ico) == format_family::ico);
  const std::uint8_t ico_empty[] = {0, 0, 1, 0, 0, 0};
  REQUIRE(probe(ico_empty) == format_family::unknown);

  // ftyp heic at offset 4.
  std::uint8_t heic[12] = {0, 0, 0, 20, 'f', 't', 'y', 'p', 'h', 'e', 'i', 'c'};
  REQUIRE(probe(heic) == format_family::heic);
  std::uint8_t avif[12] = {0, 0, 0, 20, 'f', 't', 'y', 'p', 'a', 'v', 'i', 'f'};
  REQUIRE(probe(avif) == format_family::avif);
  std::uint8_t cr3[12] = {0, 0, 0, 20, 'f', 't', 'y', 'p', 'c', 'r', 'x', ' '};
  REQUIRE(probe(cr3) == format_family::raw);

  const std::uint8_t raf[] = {'F', 'U', 'J', 'I', 'F', 'I', 'L', 'M', 'C', 'C', 'D', '-',
                              'R', 'A', 'W', 0};
  REQUIRE(probe(raf) == format_family::raw);

  // PR 6 brought WebP and GIF forward from PR 7 (plan/12 2026-09-13).
  const std::uint8_t webp[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
  REQUIRE(probe(webp) == format_family::webp);
  const std::uint8_t riff_short[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B'};
  REQUIRE(probe(riff_short) == format_family::unknown);

  const std::uint8_t gif87[] = {'G', 'I', 'F', '8', '7', 'a'};
  const std::uint8_t gif89[] = {'G', 'I', 'F', '8', '9', 'a'};
  const std::uint8_t gif_bad[] = {'G', 'I', 'F', '8', '8', 'a'};
  REQUIRE(probe(gif87) == format_family::gif);
  REQUIRE(probe(gif89) == format_family::gif);
  REQUIRE(probe(gif_bad) == format_family::unknown);
}
