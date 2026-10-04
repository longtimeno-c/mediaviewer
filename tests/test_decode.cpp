// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdlib>
#include <string>
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

TEST_CASE("BMP BI_BITFIELDS with an empty channel mask decodes that channel as 0", "[codec][bmp]") {
  // A 1x1 32-bit BI_BITFIELDS image whose green mask is zero: a channel the
  // file does not carry. The fuzzer found the scaler dividing by its zero
  // maximum (src/codec/dib.cpp channel::get); it reads 0 now.
  std::vector<std::uint8_t> b;
  auto put16 = [&](std::uint32_t v) { b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF); };
  auto put32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xFF); };
  b.push_back('B'); b.push_back('M');
  put32(14 + 40 + 12 + 4);  // file size
  put32(0);                 // reserved
  put32(14 + 40 + 12);      // pixel offset: after the header and the three masks
  put32(40);                // BITMAPINFOHEADER
  put32(1); put32(1);       // 1 x 1
  put16(1); put16(32);      // planes, bpp
  put32(3);                 // BI_BITFIELDS
  put32(4);                 // image size
  put32(2835); put32(2835); // ppm
  put32(0); put32(0);       // colours used / important
  put32(0x00FF0000u);       // red mask
  put32(0u);                // green mask: EMPTY
  put32(0x000000FFu);       // blue mask
  put32(0x00AA00CCu);       // the pixel: red 0xAA, blue 0xCC
  auto img = decode(b);
  REQUIRE(img);
  REQUIRE(img->format == format_family::bmp);
  REQUIRE(img->width == 1);
  REQUIRE(img->height == 1);
  REQUIRE(img->rgba.size() == 4);
  CHECK(img->rgba[0] == 0xAA);
  CHECK(img->rgba[1] == 0);
  CHECK(img->rgba[2] == 0xCC);
  CHECK(img->rgba[3] == 255);
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

TEST_CASE("a JPEG header declaring a gigapixel is refused without allocating it",
          "[codec][jpeg]") {
  // fuzz_jpeg (CI run 36159301248): 291 bytes whose SOF0 declares 56528x18759.
  // libjpeg sizes its working memory from that inside jpeg_start_decompress,
  // before our output check, so this cost 2.1 GB at 1:1 and 6.9 GB at 1/4
  // until the decoder gave libjpeg a memory budget.
  const char* hex =
      "ffd8ffe000104a4649006d6e7472520807070000ffdb0043000a070708073439003b3e3e4947"
      "4946383961433ce0100d0a110e0b0b1016101113141515150c0f171816141812140e14ffdb00"
      "43010304040504050b053005140d0b0d14141414141414141414141414141414141414141414"
      "14141414141414141414141c14141414141414141414141414111414ffc00011084947dcd003"
      "011100021101031101ff00104a4649006d6e520807072b00ffdb0043000a070708073439003b"
      "3e3e49433c1e1e1e1e1e1e1e1e1e1e1e1e48373d3e3bffc0000b0c0020003001011100ffc400"
      "1f0000010501010101010100000000000000000102030405065a0200000708090a0b3bc400b5"
      "f4f5f6f7f8fdfaffda0008010100003f3fa575003f001c6674";
  std::vector<std::uint8_t> bytes;
  for (const char* p = hex; p[0] && p[1]; p += 2) {
    bytes.push_back(static_cast<std::uint8_t>(std::strtoul(std::string(p, 2).c_str(), nullptr, 16)));
  }
  REQUIRE(bytes.size() == 291);
  for (int scale : {1, 2, 4, 8}) {
    CHECK_FALSE(mv::codec::decode_jpeg(bytes, nullptr, scale));
  }
}

TEST_CASE("truncated JPEG is corrupt", "[codec][jpeg]") {
  const std::uint8_t truncated[] = {0xFF, 0xD8, 0xFF};
  auto img = decode(truncated);
  REQUIRE_FALSE(img);
  REQUIRE(img.error() == mv::status::corrupt);
}
