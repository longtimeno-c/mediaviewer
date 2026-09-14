// SPDX-License-Identifier: GPL-2.0-or-later
// PR 7: TIFF (libtiff) and ICO decoders.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

#include "codec/decode.h"
#include "core/job_system.h"
#include "fixtures.h"
#include "fixtures_tiff_ico.h"
#include "image/pipeline.h"

using mv::status;
using mv::codec::decode_ico;
using mv::codec::decode_tiff;
using mv::codec::format_family;

namespace {

std::vector<std::uint8_t> gradient_rgb(std::uint32_t w, std::uint32_t h) {
  std::vector<std::uint8_t> v(static_cast<std::size_t>(w) * h * 3);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 3;
      v[i] = static_cast<std::uint8_t>(x * 37 + y);
      v[i + 1] = static_cast<std::uint8_t>(y * 53 + x);
      v[i + 2] = static_cast<std::uint8_t>((x ^ y) * 11);
    }
  }
  return v;
}

bool same_rgb_opaque(const mv::codec::raster& img, const std::vector<std::uint8_t>& rgb) {
  if (img.rgba.size() != rgb.size() / 3 * 4) return false;
  for (std::size_t p = 0; p < rgb.size() / 3; ++p) {
    if (img.rgba[p * 4] != rgb[p * 3] || img.rgba[p * 4 + 1] != rgb[p * 3 + 1] ||
        img.rgba[p * 4 + 2] != rgb[p * 3 + 2] || img.rgba[p * 4 + 3] != 255) {
      return false;
    }
  }
  return true;
}

bool near(int a, int b, int tol) { return std::abs(a - b) <= tol; }

fixtures::tiff_spec rgb8(std::uint32_t w, std::uint32_t h) {
  fixtures::tiff_spec s;
  s.width = w;
  s.height = h;
  return s;
}

}  // namespace

// ---- TIFF ------------------------------------------------------------------

TEST_CASE("TIFF 8-bit RGB strips decode exactly", "[codec][tiff]") {
  const auto rgb = gradient_rgb(7, 5);
  auto bytes = fixtures::tiff_write(rgb8(7, 5), rgb.data());
  REQUIRE_FALSE(bytes.empty());
  auto img = decode_tiff(bytes);
  REQUIRE(img);
  CHECK(img->format == format_family::tiff);
  CHECK(img->width == 7);
  CHECK(img->height == 5);
  CHECK(img->icc.empty());
  CHECK_FALSE(img->tagged_srgb);
  CHECK(same_rgb_opaque(*img, rgb));
}

TEST_CASE("TIFF LZW, Deflate and PackBits decode exactly", "[codec][tiff]") {
  const auto rgb = gradient_rgb(33, 17);
  for (std::uint16_t c : {std::uint16_t{COMPRESSION_LZW}, std::uint16_t{COMPRESSION_ADOBE_DEFLATE},
                          std::uint16_t{COMPRESSION_PACKBITS}}) {
    CAPTURE(c);
    auto spec = rgb8(33, 17);
    spec.compression = c;
    spec.rows_per_strip = 5;
    auto bytes = fixtures::tiff_write(spec, rgb.data());
    REQUIRE_FALSE(bytes.empty());
    auto img = decode_tiff(bytes);
    REQUIRE(img);
    CHECK(same_rgb_opaque(*img, rgb));
  }
}

TEST_CASE("TIFF 16-bit RGB rounds to 8 bits", "[codec][tiff]") {
  const std::uint16_t samples[] = {0xFFFF, 0x8080, 0x0000, 0x0101, 0x7F7F, 0x1234};
  auto spec = rgb8(2, 1);
  spec.bps = 16;
  spec.compression = COMPRESSION_ADOBE_DEFLATE;
  auto bytes = fixtures::tiff_write(spec, reinterpret_cast<const std::uint8_t*>(samples));
  REQUIRE_FALSE(bytes.empty());
  auto img = decode_tiff(bytes);
  REQUIRE(img);
  const std::uint8_t expect[] = {255, 128, 0, 255, 1, 127, 18, 255};
  REQUIRE(img->rgba.size() == 8);
  for (int i = 0; i < 8; ++i) CHECK(img->rgba[i] == expect[i]);
}

TEST_CASE("TIFF tiled with partial edge tiles decodes exactly", "[codec][tiff]") {
  const auto rgb = gradient_rgb(40, 24);
  auto spec = rgb8(40, 24);
  spec.tile = 16;
  SECTION("uncompressed") {}
  SECTION("LZW") { spec.compression = COMPRESSION_LZW; }
  auto bytes = fixtures::tiff_write(spec, rgb.data());
  REQUIRE_FALSE(bytes.empty());
  auto img = decode_tiff(bytes);
  REQUIRE(img);
  CHECK(img->width == 40);
  CHECK(img->height == 24);
  CHECK(same_rgb_opaque(*img, rgb));
}

TEST_CASE("TIFF separate planes decode exactly (strips and tiles)", "[codec][tiff]") {
  const auto rgb = gradient_rgb(19, 21);
  auto spec = rgb8(19, 21);
  spec.planar = PLANARCONFIG_SEPARATE;
  SECTION("strips") { spec.rows_per_strip = 4; }
  SECTION("tiles") { spec.tile = 16; }
  auto bytes = fixtures::tiff_write(spec, rgb.data());
  REQUIRE_FALSE(bytes.empty());
  auto img = decode_tiff(bytes);
  REQUIRE(img);
  CHECK(same_rgb_opaque(*img, rgb));
}

TEST_CASE("TIFF 8-bit palette maps through the 16-bit colormap", "[codec][tiff]") {
  std::vector<std::uint16_t> cmap(3 * 256);
  for (int i = 0; i < 256; ++i) {
    cmap[i] = static_cast<std::uint16_t>(i * 257);
    cmap[256 + i] = static_cast<std::uint16_t>((255 - i) * 257);
    cmap[512 + i] = static_cast<std::uint16_t>(((i * 3) % 256) * 257);
  }
  std::vector<std::uint8_t> idx(6 * 3);
  for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = static_cast<std::uint8_t>(i * 13);
  fixtures::tiff_spec spec;
  spec.width = 6;
  spec.height = 3;
  spec.spp = 1;
  spec.photometric = PHOTOMETRIC_PALETTE;
  spec.cmap = cmap;
  auto bytes = fixtures::tiff_write(spec, idx.data());
  REQUIRE_FALSE(bytes.empty());
  auto img = decode_tiff(bytes);
  REQUIRE(img);
  for (std::size_t p = 0; p < idx.size(); ++p) {
    CHECK(img->rgba[p * 4] == idx[p]);
    CHECK(img->rgba[p * 4 + 1] == 255 - idx[p]);
    CHECK(img->rgba[p * 4 + 2] == (idx[p] * 3) % 256);
    CHECK(img->rgba[p * 4 + 3] == 255);
  }
}

TEST_CASE("TIFF 4-bit palette with an odd width unpacks nibbles", "[codec][tiff]") {
  std::vector<std::uint16_t> cmap(3 * 16);
  for (int i = 0; i < 16; ++i) {
    cmap[i] = static_cast<std::uint16_t>(i * 17 * 257);
    cmap[16 + i] = 0;
    cmap[32 + i] = 0xFFFF;
  }
  // 5 x 2, 3 bytes per row: indices 0..9.
  const std::uint8_t packed[] = {0x01, 0x23, 0x40, 0x56, 0x78, 0x90};
  fixtures::tiff_spec spec;
  spec.width = 5;
  spec.height = 2;
  spec.spp = 1;
  spec.bps = 4;
  spec.photometric = PHOTOMETRIC_PALETTE;
  spec.cmap = cmap;
  auto bytes = fixtures::tiff_write(spec, packed);
  REQUIRE_FALSE(bytes.empty());
  auto img = decode_tiff(bytes);
  REQUIRE(img);
  for (int p = 0; p < 10; ++p) {
    CHECK(img->rgba[p * 4] == p * 17);
    CHECK(img->rgba[p * 4 + 1] == 0);
    CHECK(img->rgba[p * 4 + 2] == 255);
  }
}

TEST_CASE("TIFF greyscale: 8-bit min-is-black and 1-bit min-is-white", "[codec][tiff]") {
  SECTION("8-bit") {
    const std::uint8_t grey[] = {0, 64, 128, 255};
    fixtures::tiff_spec spec;
    spec.width = 4;
    spec.spp = 1;
    spec.photometric = PHOTOMETRIC_MINISBLACK;
    auto bytes = fixtures::tiff_write(spec, grey);
    REQUIRE_FALSE(bytes.empty());
    auto img = decode_tiff(bytes);
    REQUIRE(img);
    for (int p = 0; p < 4; ++p) {
      CHECK(img->rgba[p * 4] == grey[p]);
      CHECK(img->rgba[p * 4 + 1] == grey[p]);
      CHECK(img->rgba[p * 4 + 2] == grey[p]);
    }
  }
  SECTION("1-bit min-is-white, CCITT-free and PackBits") {
    // 10 x 1: bits 1011000011 → ink (black) where 1.
    const std::uint8_t packed[] = {0xB0, 0xC0};
    fixtures::tiff_spec spec;
    spec.width = 10;
    spec.spp = 1;
    spec.bps = 1;
    spec.photometric = PHOTOMETRIC_MINISWHITE;
    spec.compression = COMPRESSION_PACKBITS;
    auto bytes = fixtures::tiff_write(spec, packed);
    REQUIRE_FALSE(bytes.empty());
    auto img = decode_tiff(bytes);
    REQUIRE(img);
    const int bits[] = {1, 0, 1, 1, 0, 0, 0, 0, 1, 1};
    for (int p = 0; p < 10; ++p) CHECK(img->rgba[p * 4] == (bits[p] ? 0 : 255));
  }
}

TEST_CASE("TIFF RGBA: unassociated is straight, associated is un-premultiplied", "[codec][tiff]") {
  auto spec = rgb8(2, 1);
  spec.spp = 4;
  SECTION("unassociated") {
    spec.extra = EXTRASAMPLE_UNASSALPHA;
    const std::uint8_t px[] = {200, 100, 50, 128, 1, 2, 3, 0};
    auto bytes = fixtures::tiff_write(spec, px);
    REQUIRE_FALSE(bytes.empty());
    auto img = decode_tiff(bytes);
    REQUIRE(img);
    for (int i = 0; i < 8; ++i) CHECK(img->rgba[i] == px[i]);
  }
  SECTION("associated") {
    spec.extra = EXTRASAMPLE_ASSOCALPHA;
    const std::uint8_t px[] = {100, 50, 25, 128, 0, 0, 0, 0};
    auto bytes = fixtures::tiff_write(spec, px);
    REQUIRE_FALSE(bytes.empty());
    auto img = decode_tiff(bytes);
    REQUIRE(img);
    CHECK(near(img->rgba[0], 200, 1));
    CHECK(near(img->rgba[1], 100, 1));
    CHECK(near(img->rgba[2], 50, 1));
    CHECK(img->rgba[3] == 128);
    CHECK(img->rgba[7] == 0);
  }
}

TEST_CASE("TIFF ICC: an RGB profile is carried, a grey profile is dropped", "[codec][tiff]") {
  const auto rgb = gradient_rgb(4, 4);
  auto spec = rgb8(4, 4);
  spec.icc = fixtures::adobe_rgb_icc();
  REQUIRE_FALSE(spec.icc.empty());
  auto bytes = fixtures::tiff_write(spec, rgb.data());
  REQUIRE_FALSE(bytes.empty());
  auto img = decode_tiff(bytes);
  REQUIRE(img);
  CHECK(img->icc == spec.icc);

  // Through the display pipeline the profile is honoured (D6).
  auto shown = mv::image::decode_bytes(bytes);
  REQUIRE(shown);
  CHECK(shown->icc_tagged);
  CHECK(shown->width == 4);

  const std::uint8_t grey[] = {10, 20, 30, 40};
  fixtures::tiff_spec g;
  g.width = 4;
  g.spp = 1;
  g.photometric = PHOTOMETRIC_MINISBLACK;
  g.icc = fixtures::grey_icc();
  REQUIRE_FALSE(g.icc.empty());
  auto gbytes = fixtures::tiff_write(g, grey);
  REQUIRE_FALSE(gbytes.empty());
  auto gimg = decode_tiff(gbytes);
  REQUIRE(gimg);
  CHECK(gimg->icc.empty());
  CHECK(mv::image::decode_bytes(gbytes));
}

TEST_CASE("TIFF 32-bit float: untagged is sRGB-encoded, tagged keeps its TRC", "[codec][tiff]") {
  const float values[] = {0.0f, 0.5f, 1.0f, std::numeric_limits<float>::quiet_NaN(), 2.0f, -1.0f};
  fixtures::tiff_spec spec;
  spec.width = 6;
  spec.spp = 1;
  spec.bps = 32;
  spec.sample_format = SAMPLEFORMAT_IEEEFP;
  spec.photometric = PHOTOMETRIC_MINISBLACK;
  auto bytes = fixtures::tiff_write(spec, reinterpret_cast<const std::uint8_t*>(values));
  REQUIRE_FALSE(bytes.empty());
  auto img = decode_tiff(bytes);
  REQUIRE(img);
  const int expect[] = {0, 188, 255, 0, 255, 0};
  for (int p = 0; p < 6; ++p) CHECK(near(img->rgba[p * 4], expect[p], 1));

  const float rgbf[] = {0.5f, 0.5f, 0.5f};
  fixtures::tiff_spec t;
  t.bps = 32;
  t.sample_format = SAMPLEFORMAT_IEEEFP;
  t.icc = fixtures::adobe_rgb_icc();
  auto tbytes = fixtures::tiff_write(t, reinterpret_cast<const std::uint8_t*>(rgbf));
  REQUIRE_FALSE(tbytes.empty());
  auto timg = decode_tiff(tbytes);
  REQUIRE(timg);
  CHECK(timg->icc == t.icc);
  CHECK(near(timg->rgba[0], 128, 1));
}

TEST_CASE("TIFF CMYK converts naively", "[codec][tiff]") {
  const std::uint8_t cmyk[] = {255, 0, 0, 0, 0, 0, 0, 255, 0, 0, 0, 0, 0, 128, 0, 0};
  fixtures::tiff_spec spec;
  spec.width = 4;
  spec.spp = 4;
  spec.photometric = PHOTOMETRIC_SEPARATED;
  auto bytes = fixtures::tiff_write(spec, cmyk);
  REQUIRE_FALSE(bytes.empty());
  auto img = decode_tiff(bytes);
  REQUIRE(img);
  const std::uint8_t expect[] = {0, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255, 255, 127, 255, 255};
  for (int i = 0; i < 16; ++i) CHECK(near(img->rgba[i], expect[i], 1));
}

TEST_CASE("TIFF JPEG-compressed YCbCr decodes to RGB", "[codec][tiff]") {
  std::vector<std::uint8_t> rgb(32 * 32 * 3);
  for (std::size_t i = 0; i < rgb.size(); i += 3) {
    rgb[i] = 200;
    rgb[i + 1] = 30;
    rgb[i + 2] = 40;
  }
  auto spec = rgb8(32, 32);
  spec.jpeg_ycbcr = true;
  spec.rows_per_strip = 16;
  auto bytes = fixtures::tiff_write(spec, rgb.data());
  REQUIRE_FALSE(bytes.empty());
  auto img = decode_tiff(bytes);
  REQUIRE(img);
  for (std::size_t p = 0; p < 32 * 32; p += 97) {
    CHECK(near(img->rgba[p * 4], 200, 8));
    CHECK(near(img->rgba[p * 4 + 1], 30, 8));
    CHECK(near(img->rgba[p * 4 + 2], 40, 8));
  }
}

TEST_CASE("TIFF raw YCbCr goes through TIFFRGBAImage in stored order", "[codec][tiff]") {
  // 1 x 2, neutral chroma: Y=50 on top, Y=200 below, orientation bottom-left.
  const std::uint8_t ycc[] = {50, 128, 128, 200, 128, 128};
  fixtures::tiff_spec spec;
  spec.width = 1;
  spec.height = 2;
  spec.photometric = PHOTOMETRIC_YCBCR;
  spec.ycbcr_1x1 = true;
  spec.orientation = ORIENTATION_BOTLEFT;
  auto bytes = fixtures::tiff_write(spec, ycc);
  REQUIRE_FALSE(bytes.empty());
  auto img = decode_tiff(bytes);
  REQUIRE(img);
  CHECK(near(img->rgba[0], 50, 3));
  CHECK(near(img->rgba[4], 200, 3));
  CHECK(img->rgba[3] == 255);
}

TEST_CASE("TIFF orientation tag is not applied: pixels stay in stored order", "[codec][tiff]") {
  const auto rgb = gradient_rgb(3, 2);
  auto spec = rgb8(3, 2);
  spec.orientation = ORIENTATION_BOTRIGHT;
  auto bytes = fixtures::tiff_write(spec, rgb.data());
  REQUIRE_FALSE(bytes.empty());
  auto img = decode_tiff(bytes);
  REQUIRE(img);
  CHECK(same_rgb_opaque(*img, rgb));
}

TEST_CASE("TIFF truncated at every length fails without crashing", "[codec][tiff]") {
  const auto rgb = gradient_rgb(32, 32);
  auto spec = rgb8(32, 32);
  spec.compression = COMPRESSION_LZW;
  spec.rows_per_strip = 8;
  const auto full = fixtures::tiff_write(spec, rgb.data());
  REQUIRE_FALSE(full.empty());
  for (std::size_t len = 0; len < full.size(); ++len) {
    std::span<const std::uint8_t> part(full.data(), len);
    auto img = decode_tiff(part);
    if (len <= full.size() / 2) {
      CAPTURE(len);
      CHECK_FALSE(img);
    }
    if (img) CHECK(img->width == 32);
  }
}

TEST_CASE("TIFF hostile dimensions are refused before allocation", "[codec][tiff]") {
  SECTION("wider than 65535") {
    auto img = decode_tiff(fixtures::tiff_header_only(70000, 10));
    REQUIRE_FALSE(img);
    CHECK(img.error() == status::unsupported_format);
  }
  SECTION("over 256 MP") {
    auto img = decode_tiff(fixtures::tiff_header_only(60000, 60000));
    REQUIRE_FALSE(img);
    CHECK(img.error() == status::unsupported_format);
  }
  SECTION("at the limit but with 16 bytes of pixels") {
    auto img = decode_tiff(fixtures::tiff_header_only(16000, 16000));
    REQUIRE_FALSE(img);
    CHECK(img.error() == status::corrupt);
  }
  SECTION("zero width") {
    auto img = decode_tiff(fixtures::tiff_header_only(0, 10));
    REQUIRE_FALSE(img);
  }
}

TEST_CASE("TIFF decode leaves the input bytes untouched", "[codec][tiff]") {
  const auto rgb = gradient_rgb(16, 16);
  auto spec = rgb8(16, 16);
  spec.compression = COMPRESSION_LZW;
  const auto bytes = fixtures::tiff_write(spec, rgb.data());
  REQUIRE_FALSE(bytes.empty());
  const auto copy = bytes;
  REQUIRE(decode_tiff(bytes));
  CHECK(bytes == copy);
}

TEST_CASE("TIFF honours cancellation", "[codec][tiff]") {
  const auto rgb = gradient_rgb(8, 8);
  auto bytes = fixtures::tiff_write(rgb8(8, 8), rgb.data());
  REQUIRE_FALSE(bytes.empty());
  std::atomic<mv::generation> current{2};
  mv::job_context ctx(1, 1, &current, 0);
  auto img = decode_tiff(bytes, &ctx);
  REQUIRE_FALSE(img);
  CHECK(img.error() == status::cancelled);
}

TEST_CASE("TIFF round-trips through image::decode_bytes", "[codec][tiff]") {
  const auto rgb = gradient_rgb(9, 7);
  auto bytes = fixtures::tiff_write(rgb8(9, 7), rgb.data());
  REQUIRE_FALSE(bytes.empty());
  auto shown = mv::image::decode_bytes(bytes);
  REQUIRE(shown);
  CHECK(shown->format == format_family::tiff);
  CHECK(shown->width == 9);
  CHECK(shown->height == 7);
  CHECK_FALSE(shown->icc_tagged);
  REQUIRE(shown->rgba.size() == 9 * 7 * 4);
  CHECK(shown->rgba[0] == rgb[0]);
  CHECK(shown->rgba[1] == rgb[1]);
  CHECK(shown->rgba[2] == rgb[2]);
}

// ---- ICO -------------------------------------------------------------------

namespace {

std::vector<std::uint8_t> solid_rgba(std::uint32_t w, std::uint32_t h, std::uint8_t r, std::uint8_t g,
                                     std::uint8_t b, std::uint8_t a) {
  std::vector<std::uint8_t> v(static_cast<std::size_t>(w) * h * 4);
  for (std::size_t i = 0; i < v.size(); i += 4) {
    v[i] = r;
    v[i + 1] = g;
    v[i + 2] = b;
    v[i + 3] = a;
  }
  return v;
}

fixtures::ico_entry dib_entry(std::uint32_t w, std::uint32_t h, std::uint16_t bpp,
                              std::vector<std::uint8_t> payload) {
  fixtures::ico_entry e;
  e.payload = std::move(payload);
  e.w_byte = static_cast<std::uint8_t>(w >= 256 ? 0 : w);
  e.h_byte = static_cast<std::uint8_t>(h >= 256 ? 0 : h);
  e.bpp = bpp;
  return e;
}

}  // namespace

TEST_CASE("ICO 32-bit BMP entry keeps its alpha", "[codec][ico]") {
  const std::uint8_t rgba[] = {255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0,
                               10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 255};
  auto file = fixtures::ico_file({dib_entry(3, 2, 32, fixtures::ico_dib(3, 2, 32, rgba))});
  auto img = decode_ico(file);
  REQUIRE(img);
  CHECK(img->format == format_family::ico);
  CHECK(img->width == 3);
  CHECK(img->height == 2);
  REQUIRE(img->rgba.size() == 24);
  for (int i = 0; i < 24; ++i) CHECK(img->rgba[i] == rgba[i]);
}

TEST_CASE("ICO picks the largest entry, then the deepest", "[codec][ico]") {
  const auto red = solid_rgba(16, 16, 255, 0, 0, 255);
  const auto green = solid_rgba(32, 32, 0, 255, 0, 255);
  const auto blue = solid_rgba(32, 32, 0, 0, 255, 255);
  auto file = fixtures::ico_file({
      dib_entry(16, 16, 32, fixtures::ico_dib(16, 16, 32, red.data())),
      dib_entry(32, 32, 24, fixtures::ico_dib(32, 32, 24, green.data())),
      dib_entry(32, 32, 32, fixtures::ico_dib(32, 32, 32, blue.data())),
  });
  auto img = decode_ico(file);
  REQUIRE(img);
  CHECK(img->width == 32);
  CHECK(img->height == 32);
  CHECK(img->rgba[0] == 0);
  CHECK(img->rgba[1] == 0);
  CHECK(img->rgba[2] == 255);
}

TEST_CASE("ICO a lone 16x16 icon still decodes", "[codec][ico]") {
  const auto px = solid_rgba(16, 16, 1, 2, 3, 255);
  auto file = fixtures::ico_file({dib_entry(16, 16, 32, fixtures::ico_dib(16, 16, 32, px.data()))});
  auto img = decode_ico(file);
  REQUIRE(img);
  CHECK(img->width == 16);
  CHECK(img->rgba == px);
}

TEST_CASE("ICO PNG entry (256 px, directory byte 0) decodes via libspng", "[codec][ico]") {
  auto px = solid_rgba(256, 256, 12, 34, 56, 200);
  px[4 * 5 + 3] = 7;
  fixtures::ico_entry e;
  e.payload = fixtures::png_rgba(256, 256, px.data());
  REQUIRE_FALSE(e.payload.empty());
  const auto small = solid_rgba(48, 48, 255, 255, 255, 255);
  auto file = fixtures::ico_file({dib_entry(48, 48, 32, fixtures::ico_dib(48, 48, 32, small.data())), e});
  auto img = decode_ico(file);
  REQUIRE(img);
  CHECK(img->format == format_family::ico);
  CHECK(img->width == 256);
  CHECK(img->height == 256);
  CHECK(img->rgba == px);
}

TEST_CASE("ICO 24-bit entry takes transparency from the AND mask", "[codec][ico]") {
  auto px = solid_rgba(4, 4, 9, 8, 7, 255);
  std::vector<std::uint8_t> transparent(16, 0);
  for (int i = 0; i < 16; ++i) transparent[i] = static_cast<std::uint8_t>(((i / 4) + i) & 1);
  auto file = fixtures::ico_file(
      {dib_entry(4, 4, 24, fixtures::ico_dib(4, 4, 24, px.data(), {}, transparent))});
  auto img = decode_ico(file);
  REQUIRE(img);
  for (int i = 0; i < 16; ++i) {
    CHECK(img->rgba[i * 4] == 9);
    CHECK(img->rgba[i * 4 + 3] == (transparent[i] ? 0 : 255));
  }
}

TEST_CASE("ICO 32-bit entry with all-zero alpha falls back to the AND mask", "[codec][ico]") {
  auto px = solid_rgba(9, 2, 100, 150, 200, 0);
  std::vector<std::uint8_t> transparent(18, 0);
  transparent[0] = 1;
  transparent[17] = 1;
  auto file = fixtures::ico_file(
      {dib_entry(9, 2, 32, fixtures::ico_dib(9, 2, 32, px.data(), {}, transparent))});
  auto img = decode_ico(file);
  REQUIRE(img);
  CHECK(img->rgba[3] == 0);
  CHECK(img->rgba[7] == 255);
  CHECK(img->rgba[17 * 4 + 3] == 0);
  CHECK(img->rgba[16 * 4 + 3] == 255);
  CHECK(img->rgba[4] == 100);
}

TEST_CASE("ICO palette entries: 8, 4 and 1 bpp", "[codec][ico]") {
  const std::vector<std::array<std::uint8_t, 3>> pal2 = {{0, 0, 0}, {255, 128, 1}};
  std::vector<std::array<std::uint8_t, 3>> pal16(16);
  for (int i = 0; i < 16; ++i) pal16[i] = {static_cast<std::uint8_t>(i * 16), 7, 9};
  std::vector<std::array<std::uint8_t, 3>> pal256(256);
  for (int i = 0; i < 256; ++i) pal256[i] = {1, static_cast<std::uint8_t>(i), 2};

  SECTION("8 bpp") {
    std::vector<std::uint8_t> idx(6 * 3);
    for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = static_cast<std::uint8_t>(i * 14);
    auto file = fixtures::ico_file({dib_entry(6, 3, 8, fixtures::ico_dib(6, 3, 8, idx.data(), pal256))});
    auto img = decode_ico(file);
    REQUIRE(img);
    for (std::size_t p = 0; p < idx.size(); ++p) CHECK(img->rgba[p * 4 + 1] == idx[p]);
  }
  SECTION("4 bpp, odd width") {
    std::vector<std::uint8_t> idx(5 * 2);
    for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = static_cast<std::uint8_t>(i + 3);
    auto file = fixtures::ico_file({dib_entry(5, 2, 4, fixtures::ico_dib(5, 2, 4, idx.data(), pal16))});
    auto img = decode_ico(file);
    REQUIRE(img);
    for (std::size_t p = 0; p < idx.size(); ++p) CHECK(img->rgba[p * 4] == idx[p] * 16);
  }
  SECTION("1 bpp with mask") {
    std::vector<std::uint8_t> idx(9);
    std::vector<std::uint8_t> transparent(9, 0);
    for (int i = 0; i < 9; ++i) idx[i] = static_cast<std::uint8_t>(i % 3 == 0);
    transparent[8] = 1;
    auto file = fixtures::ico_file(
        {dib_entry(9, 1, 1, fixtures::ico_dib(9, 1, 1, idx.data(), pal2, transparent))});
    auto img = decode_ico(file);
    REQUIRE(img);
    for (int p = 0; p < 9; ++p) {
      CHECK(img->rgba[p * 4] == (idx[p] ? 255 : 0));
      CHECK(img->rgba[p * 4 + 1] == (idx[p] ? 128 : 0));
    }
    CHECK(img->rgba[8 * 4 + 3] == 0);
    CHECK(img->rgba[7 * 4 + 3] == 255);
  }
}

TEST_CASE("ICO hostile and truncated files fail without crashing", "[codec][ico]") {
  const auto px = solid_rgba(16, 16, 1, 2, 3, 255);
  const auto full = fixtures::ico_file({dib_entry(16, 16, 32, fixtures::ico_dib(16, 16, 32, px.data()))});
  SECTION("every truncation") {
    for (std::size_t len = 0; len < full.size(); ++len) {
      CAPTURE(len);
      CHECK_FALSE(decode_ico(std::span<const std::uint8_t>(full.data(), len)));
    }
  }
  SECTION("entry offset past the end") {
    auto bad = full;
    bad[6 + 12] = 0xFF;
    bad[6 + 13] = 0xFF;
    auto img = decode_ico(bad);
    REQUIRE_FALSE(img);
    CHECK(img.error() == status::corrupt);
  }
  SECTION("directory count beyond the buffer") {
    auto bad = full;
    bad[4] = 0xFF;
    bad[5] = 0x7F;
    auto img = decode_ico(bad);
    REQUIRE_FALSE(img);
    CHECK(img.error() == status::corrupt);
  }
  SECTION("absurd DIB dimensions") {
    auto bad = full;
    const std::size_t dib = 6 + 16;
    bad[dib + 4] = 0xA0;  // width 100000
    bad[dib + 5] = 0x86;
    bad[dib + 6] = 0x01;
    bad[dib + 7] = 0x00;
    auto img = decode_ico(bad);
    REQUIRE_FALSE(img);
    CHECK(img.error() == status::unsupported_format);
  }
  SECTION("XOR bitmap larger than the entry") {
    auto bad = full;
    bad[6 + 16 + 4] = 200;  // width 200, still < 256, but no data for it
    auto img = decode_ico(bad);
    REQUIRE_FALSE(img);
    CHECK(img.error() == status::corrupt);
  }
}

TEST_CASE("ICO round-trips through image::decode_bytes", "[codec][ico]") {
  const auto px = solid_rgba(24, 24, 40, 80, 120, 255);
  auto file = fixtures::ico_file({dib_entry(24, 24, 32, fixtures::ico_dib(24, 24, 32, px.data()))});
  auto shown = mv::image::decode_bytes(file);
  REQUIRE(shown);
  CHECK(shown->format == format_family::ico);
  CHECK(shown->width == 24);
  CHECK(shown->height == 24);
  REQUIRE(shown->rgba.size() == px.size());
  CHECK(shown->rgba[0] == 40);
  CHECK(shown->rgba[1] == 80);
  CHECK(shown->rgba[2] == 120);
  CHECK(shown->rgba[3] == 255);
}
