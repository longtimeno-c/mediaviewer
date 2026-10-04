// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Less common variants of the D5 formats, each of which used to come back
// unsupported_format: CMYK / YCCK, 12-bit and lossless JPEG; BMP palette,
// 16-bit, BITFIELDS, RLE, OS/2 and V5 headers; ICO entries with those DIBs;
// signed, half-float and sub-byte-plane TIFF.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

#include <jpeglib.h>
#include <tiffio.h>

#include "codec/decode.h"
#include "fixtures.h"
#include "fixtures_tiff_ico.h"

using mv::status;
using mv::codec::decode;
using mv::codec::decode_bmp;
using mv::codec::decode_ico;
using mv::codec::decode_jpeg;
using mv::codec::decode_tiff;
using mv::codec::format_family;

namespace {

bool near(int a, int b, int tol) { return std::abs(a - b) <= tol; }

bool pixel_near(const mv::codec::raster& img, std::size_t i, int r, int g, int b, int a, int tol) {
  const std::uint8_t* p = img.rgba.data() + i * 4;
  return near(p[0], r, tol) && near(p[1], g, tol) && near(p[2], b, tol) && p[3] == a;
}

// ---- JPEG writer: any precision, colour space, lossy or lossless -----------

struct jpeg_spec {
  std::uint32_t width = 16;
  std::uint32_t height = 16;
  int precision = 8;                // 8 or 12 lossy; 2-16 lossless
  J_COLOR_SPACE in = JCS_RGB;       // what the samples are
  int components = 3;
  J_COLOR_SPACE stored = JCS_UNKNOWN;  // jpeg_set_colorspace target; UNKNOWN: library default
  bool lossless = false;
  bool adobe_marker = true;         // CMYK / YCCK only
  bool arithmetic = false;
  std::vector<std::uint8_t> icc;
};

struct jpeg_sink {
  jpeg_destination_mgr pub;
  std::vector<std::uint8_t>* out = nullptr;
  std::array<std::uint8_t, 4096> buf{};
};

// `samples`: interleaved, one std::uint16_t per sample whatever the precision.
std::vector<std::uint8_t> jpeg_write(const jpeg_spec& s, const std::vector<std::uint16_t>& samples) {
  std::vector<std::uint8_t> out;
  jpeg_compress_struct cinfo{};
  jpeg_error_mgr err{};
  cinfo.err = jpeg_std_error(&err);
  jpeg_create_compress(&cinfo);
  jpeg_sink dest{};
  dest.out = &out;
  dest.pub.init_destination = [](j_compress_ptr c) {
    auto* d = reinterpret_cast<jpeg_sink*>(c->dest);
    c->dest->next_output_byte = d->buf.data();
    c->dest->free_in_buffer = d->buf.size();
  };
  dest.pub.empty_output_buffer = [](j_compress_ptr c) -> boolean {
    auto* d = reinterpret_cast<jpeg_sink*>(c->dest);
    d->out->insert(d->out->end(), d->buf.begin(), d->buf.end());
    c->dest->next_output_byte = d->buf.data();
    c->dest->free_in_buffer = d->buf.size();
    return TRUE;
  };
  dest.pub.term_destination = [](j_compress_ptr c) {
    auto* d = reinterpret_cast<jpeg_sink*>(c->dest);
    const std::size_t used = d->buf.size() - c->dest->free_in_buffer;
    d->out->insert(d->out->end(), d->buf.begin(), d->buf.begin() + static_cast<std::ptrdiff_t>(used));
  };
  cinfo.dest = &dest.pub;
  cinfo.image_width = s.width;
  cinfo.image_height = s.height;
  cinfo.input_components = s.components;
  cinfo.in_color_space = s.in;
  cinfo.data_precision = s.precision;
  jpeg_set_defaults(&cinfo);
  if (s.stored != JCS_UNKNOWN) jpeg_set_colorspace(&cinfo, s.stored);
  if (s.lossless) {
    jpeg_enable_lossless(&cinfo, 1, 0);
    // jpeg_set_colorspace(YCbCr) subsamples chroma 2x2; a lossless fixture wants every sample.
    for (int c = 0; c < cinfo.num_components; ++c) {
      cinfo.comp_info[c].h_samp_factor = 1;
      cinfo.comp_info[c].v_samp_factor = 1;
    }
  }
  if (!s.lossless) jpeg_set_quality(&cinfo, 100, TRUE);
  if (s.components == 4) cinfo.write_Adobe_marker = s.adobe_marker ? TRUE : FALSE;
  cinfo.arith_code = s.arithmetic ? TRUE : FALSE;
  jpeg_start_compress(&cinfo, TRUE);
  if (!s.icc.empty()) {
    std::vector<std::uint8_t> marker(14 + s.icc.size());
    std::memcpy(marker.data(), "ICC_PROFILE\0", 12);
    marker[12] = 1;
    marker[13] = 1;
    std::memcpy(marker.data() + 14, s.icc.data(), s.icc.size());
    jpeg_write_marker(&cinfo, JPEG_APP0 + 2, marker.data(), static_cast<unsigned>(marker.size()));
  }
  const std::size_t row_samples = static_cast<std::size_t>(s.width) * s.components;
  std::vector<std::uint8_t> row8(row_samples);
  std::vector<J12SAMPLE> row12(row_samples);
  std::vector<J16SAMPLE> row16(row_samples);
  while (cinfo.next_scanline < cinfo.image_height) {
    const std::uint16_t* src = samples.data() + cinfo.next_scanline * row_samples;
    if (s.precision <= 8) {
      for (std::size_t i = 0; i < row_samples; ++i) row8[i] = static_cast<std::uint8_t>(src[i]);
      JSAMPROW rows[1] = {row8.data()};
      jpeg_write_scanlines(&cinfo, rows, 1);
    } else if (s.precision <= 12) {
      for (std::size_t i = 0; i < row_samples; ++i) row12[i] = static_cast<J12SAMPLE>(src[i]);
      J12SAMPROW rows[1] = {row12.data()};
      jpeg12_write_scanlines(&cinfo, rows, 1);
    } else {
      for (std::size_t i = 0; i < row_samples; ++i) row16[i] = static_cast<J16SAMPLE>(src[i]);
      J16SAMPROW rows[1] = {row16.data()};
      jpeg16_write_scanlines(&cinfo, rows, 1);
    }
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  return out;
}

std::vector<std::uint16_t> solid(const jpeg_spec& s, std::span<const std::uint16_t> px) {
  std::vector<std::uint16_t> v;
  v.reserve(static_cast<std::size_t>(s.width) * s.height * px.size());
  for (std::uint32_t i = 0; i < s.width * s.height; ++i) v.insert(v.end(), px.begin(), px.end());
  return v;
}
std::vector<std::uint16_t> solid(const jpeg_spec& s, std::initializer_list<std::uint16_t> px) {
  return solid(s, std::span<const std::uint16_t>(px.begin(), px.size()));
}

// ---- BMP / DIB writer --------------------------------------------------------

struct le_buf {
  std::vector<std::uint8_t> b;
  void u8(std::uint8_t v) { b.push_back(v); }
  void u16(std::uint16_t v) {
    u8(static_cast<std::uint8_t>(v));
    u8(static_cast<std::uint8_t>(v >> 8));
  }
  void u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i)));
  }
  void pad_to(std::size_t n) { b.resize(n, 0); }
  void put32(std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
  }
};

// An info header of `size` bytes (40, 52, 56, 108 or 124), V2+ masks filled in.
le_buf info_header(std::uint32_t size, std::int32_t w, std::int32_t h, std::uint16_t bpp,
                   std::uint32_t compression, std::uint32_t clr_used = 0,
                   std::array<std::uint32_t, 4> masks = {}) {
  le_buf d;
  d.u32(size);
  d.u32(static_cast<std::uint32_t>(w));
  d.u32(static_cast<std::uint32_t>(h));
  d.u16(1);
  d.u16(bpp);
  d.u32(compression);
  d.u32(0);  // image size
  d.u32(2835);
  d.u32(2835);
  d.u32(clr_used);
  d.u32(0);
  d.pad_to(size);
  if (size >= 52) {
    for (int i = 0; i < 3; ++i) d.put32(40 + 4 * i, masks[i]);
  }
  if (size >= 56) d.put32(52, masks[3]);
  return d;
}

void palette(le_buf& d, std::initializer_list<std::array<std::uint8_t, 3>> rgb, bool core = false) {
  for (const auto& c : rgb) {
    d.u8(c[2]);
    d.u8(c[1]);
    d.u8(c[0]);
    if (!core) d.u8(0);
  }
}

std::vector<std::uint8_t> bmp_file(const le_buf& dib, const std::vector<std::uint8_t>& bits) {
  le_buf f;
  f.u8('B');
  f.u8('M');
  f.u32(static_cast<std::uint32_t>(14 + dib.b.size() + bits.size()));
  f.u32(0);
  f.u32(static_cast<std::uint32_t>(14 + dib.b.size()));
  f.b.insert(f.b.end(), dib.b.begin(), dib.b.end());
  f.b.insert(f.b.end(), bits.begin(), bits.end());
  return f.b;
}

std::vector<std::uint8_t> fake_cmyk_icc() {
  std::vector<std::uint8_t> icc(132, 0);
  icc[3] = 132;
  std::memcpy(icc.data() + 12, "prtr", 4);
  std::memcpy(icc.data() + 16, "CMYK", 4);
  std::memcpy(icc.data() + 36, "acsp", 4);
  return icc;
}

}  // namespace

// ---- JPEG -------------------------------------------------------------------

TEST_CASE("JPEG Adobe CMYK (inverted inks) decodes to RGB", "[codec][jpeg][variants]") {
  jpeg_spec s;
  s.in = JCS_CMYK;
  s.components = 4;
  s.stored = JCS_CMYK;
  s.icc = fake_cmyk_icc();
  // Photoshop stores 255 = no ink. Red is no cyan, full magenta + yellow, no black.
  const auto bytes = jpeg_write(s, solid(s, {255, 0, 0, 255}));
  REQUIRE_FALSE(bytes.empty());
  auto img = decode(bytes);
  REQUIRE(img);
  CHECK(img->format == format_family::jpeg);
  CHECK(img->width == 16);
  CHECK(pixel_near(*img, 0, 255, 0, 0, 255, 2));
  CHECK(pixel_near(*img, 255, 255, 0, 0, 255, 2));
  CHECK(img->icc.empty());  // the CMYK profile is not handed to an RGBA transform
}

TEST_CASE("JPEG Adobe YCCK decodes to RGB", "[codec][jpeg][variants]") {
  jpeg_spec s;
  s.in = JCS_CMYK;
  s.components = 4;
  s.stored = JCS_YCCK;
  // Inverted inks: 50 % grey through K only, and a blue through C+M.
  auto px = solid(s, {255, 255, 255, 128});
  auto bytes = jpeg_write(s, px);
  auto img = decode(bytes);
  REQUIRE(img);
  CHECK(pixel_near(*img, 17, 128, 128, 128, 255, 3));

  bytes = jpeg_write(s, solid(s, {0, 0, 255, 255}));
  img = decode(bytes);
  REQUIRE(img);
  CHECK(pixel_near(*img, 17, 0, 0, 255, 255, 3));
}

TEST_CASE("JPEG CMYK without an Adobe marker is not inverted", "[codec][jpeg][variants]") {
  jpeg_spec s;
  s.in = JCS_CMYK;
  s.components = 4;
  s.stored = JCS_CMYK;
  s.adobe_marker = false;
  const auto bytes = jpeg_write(s, solid(s, {0, 255, 255, 0}));  // red: magenta + yellow ink
  auto img = decode(bytes);
  REQUIRE(img);
  CHECK(pixel_near(*img, 5, 255, 0, 0, 255, 2));
}

TEST_CASE("JPEG CMYK honours DCT scaling", "[codec][jpeg][variants]") {
  jpeg_spec s;
  s.width = 64;
  s.height = 32;
  s.in = JCS_CMYK;
  s.components = 4;
  s.stored = JCS_YCCK;
  const auto bytes = jpeg_write(s, solid(s, {0, 255, 0, 255}));  // green: full cyan + yellow
  auto img = decode_jpeg(bytes, nullptr, 4);
  REQUIRE(img);
  CHECK(img->width == 16);
  CHECK(img->height == 8);
  CHECK(pixel_near(*img, 9, 0, 255, 0, 255, 3));
}

TEST_CASE("JPEG 12-bit lossy decodes and scales to 8 bits", "[codec][jpeg][variants]") {
  jpeg_spec s;
  s.precision = 12;
  const auto bytes = jpeg_write(s, solid(s, {4095, 2048, 0}));
  REQUIRE_FALSE(bytes.empty());
  auto img = decode(bytes);
  REQUIRE(img);
  CHECK(img->width == 16);
  CHECK(pixel_near(*img, 0, 255, 128, 0, 255, 2));
  CHECK(pixel_near(*img, 255, 255, 128, 0, 255, 2));

  SECTION("grey, keeping its ICC") {
    jpeg_spec g = s;
    g.in = JCS_GRAYSCALE;
    g.components = 1;
    g.icc = fixtures::linear_grey_icc();
    const auto gb = jpeg_write(g, solid(g, {1024}));
    auto gi = decode(gb);
    REQUIRE(gi);
    CHECK(pixel_near(*gi, 3, 64, 64, 64, 255, 1));
    CHECK(gi->icc == g.icc);
  }
  SECTION("1/2 scale") {
    auto half = decode_jpeg(bytes, nullptr, 2);
    REQUIRE(half);
    CHECK(half->width == 8);
    CHECK(pixel_near(*half, 0, 255, 128, 0, 255, 2));
  }
}

TEST_CASE("JPEG lossless decodes exactly", "[codec][jpeg][variants]") {
  SECTION("16-bit grey gradient") {
    jpeg_spec s;
    s.width = 9;
    s.height = 3;
    s.precision = 16;
    s.in = JCS_GRAYSCALE;
    s.components = 1;
    s.lossless = true;
    std::vector<std::uint16_t> px;
    for (std::uint32_t i = 0; i < s.width * s.height; ++i) px.push_back(static_cast<std::uint16_t>(i * 2500));
    const auto bytes = jpeg_write(s, px);
    REQUIRE_FALSE(bytes.empty());
    auto img = decode(bytes);
    REQUIRE(img);
    REQUIRE(img->width == 9);
    for (std::size_t i = 0; i < px.size(); ++i) {
      const int want = static_cast<int>((px[i] * 255u + 32767u) / 65535u);
      CHECK(pixel_near(*img, i, want, want, want, 255, 0));
    }
  }
  SECTION("8-bit RGB, asked for 1/4 scale, still 1:1") {
    jpeg_spec s;
    s.width = 5;
    s.height = 4;
    s.stored = JCS_RGB;
    s.lossless = true;
    std::vector<std::uint16_t> px;
    for (std::uint32_t i = 0; i < s.width * s.height; ++i) {
      px.insert(px.end(), {static_cast<std::uint16_t>(i * 12), static_cast<std::uint16_t>(255 - i * 7),
                           static_cast<std::uint16_t>((i * 37) & 255)});
    }
    const auto bytes = jpeg_write(s, px);
    auto img = decode_jpeg(bytes, nullptr, 4);
    REQUIRE(img);
    REQUIRE(img->width == 5);
    REQUIRE(img->height == 4);
    for (std::size_t i = 0; i < 20; ++i) {
      CHECK(pixel_near(*img, i, px[i * 3], px[i * 3 + 1], px[i * 3 + 2], 255, 0));
    }
  }
  SECTION("lossless YCbCr is refused, not shown wrong") {
    jpeg_spec s;
    s.width = 4;
    s.height = 2;
    s.precision = 12;
    s.in = JCS_YCbCr;
    s.stored = JCS_YCbCr;
    s.lossless = true;
    const auto bytes = jpeg_write(s, solid(s, {2048, 2048, 2048}));
    auto img = decode(bytes);
    REQUIRE_FALSE(img);
    CHECK(img.error() == status::unsupported_format);
  }
}

TEST_CASE("JPEG arithmetic coding decodes", "[codec][jpeg][variants]") {
  jpeg_spec s;
  s.arithmetic = true;
  const auto bytes = jpeg_write(s, solid(s, {30, 200, 90}));
  auto img = decode(bytes);
  REQUIRE(img);
  CHECK(pixel_near(*img, 7, 30, 200, 90, 255, 2));
}

TEST_CASE("JPEG variants fail cleanly when truncated", "[codec][jpeg][variants]") {
  jpeg_spec cmyk;
  cmyk.in = JCS_CMYK;
  cmyk.components = 4;
  cmyk.stored = JCS_YCCK;
  jpeg_spec lossless;
  lossless.precision = 16;
  lossless.in = JCS_GRAYSCALE;
  lossless.components = 1;
  lossless.lossless = true;
  // Vectors, not initializer_lists: an initializer_list inside a braced pair
  // keeps no backing array past the full expression (ASan: use-after-scope).
  for (const auto& [spec, px] : {std::pair{cmyk, std::vector<std::uint16_t>{1, 2, 3, 4}},
                                 std::pair{lossless, std::vector<std::uint16_t>{777}}}) {
    const auto full = jpeg_write(spec, solid(spec, px));
    for (std::size_t len = 0; len < full.size(); len += 7) {
      CAPTURE(len);
      auto r = decode(std::span<const std::uint8_t>(full.data(), len));
      if (r) CHECK(r->rgba.size() == static_cast<std::size_t>(r->width) * r->height * 4);
    }
  }
}

// ---- BMP --------------------------------------------------------------------

TEST_CASE("BMP 8-bit palette, bottom-up and top-down", "[codec][bmp][variants]") {
  auto d = info_header(40, 3, 2, 8, 0, 3);
  palette(d, {{{255, 0, 0}}, {{0, 255, 0}}, {{0, 0, 255}}});
  // Rows padded to 4 bytes. Bottom-up: the first stored row is the bottom one.
  const std::vector<std::uint8_t> bits = {2, 1, 0, 0, 0, 1, 2, 0};
  auto img = decode(bmp_file(d, bits));
  REQUIRE(img);
  CHECK(img->format == format_family::bmp);
  CHECK(pixel_near(*img, 0, 255, 0, 0, 255, 0));
  CHECK(pixel_near(*img, 1, 0, 255, 0, 255, 0));
  CHECK(pixel_near(*img, 2, 0, 0, 255, 255, 0));
  CHECK(pixel_near(*img, 3, 0, 0, 255, 255, 0));
  CHECK(pixel_near(*img, 5, 255, 0, 0, 255, 0));

  auto td = info_header(40, 3, -2, 8, 0, 3);
  palette(td, {{{255, 0, 0}}, {{0, 255, 0}}, {{0, 0, 255}}});
  auto top = decode(bmp_file(td, bits));
  REQUIRE(top);
  CHECK(pixel_near(*top, 0, 0, 0, 255, 255, 0));
  CHECK(pixel_near(*top, 3, 255, 0, 0, 255, 0));
}

TEST_CASE("BMP 1, 2 and 4-bit palettes with odd widths", "[codec][bmp][variants]") {
  SECTION("1-bit") {
    auto d = info_header(40, 9, 1, 1, 0);
    palette(d, {{{0, 0, 0}}, {{255, 255, 255}}});
    auto img = decode(bmp_file(d, {0b10110000, 0b10000000, 0, 0}));
    REQUIRE(img);
    const int want[9] = {1, 0, 1, 1, 0, 0, 0, 0, 1};
    for (int x = 0; x < 9; ++x) CHECK(img->rgba[x * 4] == (want[x] ? 255 : 0));
  }
  SECTION("2-bit") {
    auto d = info_header(40, 3, 1, 2, 0);
    palette(d, {{{0, 0, 0}}, {{85, 85, 85}}, {{170, 170, 170}}, {{255, 255, 255}}});
    auto img = decode(bmp_file(d, {0b11100100, 0, 0, 0}));
    REQUIRE(img);
    CHECK(img->rgba[0] == 255);
    CHECK(img->rgba[4] == 170);
    CHECK(img->rgba[8] == 85);
  }
  SECTION("4-bit, index past the palette is black") {
    auto d = info_header(40, 3, 1, 4, 0, 2);
    palette(d, {{{10, 20, 30}}, {{40, 50, 60}}});
    auto img = decode(bmp_file(d, {0x10, 0x70, 0, 0}));
    REQUIRE(img);
    CHECK(pixel_near(*img, 0, 40, 50, 60, 255, 0));
    CHECK(pixel_near(*img, 1, 10, 20, 30, 255, 0));
    CHECK(pixel_near(*img, 2, 0, 0, 0, 255, 0));
  }
}

TEST_CASE("BMP 16-bit 5-5-5 and 5-6-5", "[codec][bmp][variants]") {
  SECTION("BI_RGB is 5-5-5") {
    auto d = info_header(40, 4, 1, 16, 0);
    auto img = decode(bmp_file(d, {0x00, 0x7C, 0xE0, 0x03, 0x1F, 0x00, 0x10, 0x42}));
    REQUIRE(img);
    CHECK(pixel_near(*img, 0, 255, 0, 0, 255, 0));
    CHECK(pixel_near(*img, 1, 0, 255, 0, 255, 0));
    CHECK(pixel_near(*img, 2, 0, 0, 255, 255, 0));
    CHECK(pixel_near(*img, 3, 132, 132, 132, 255, 0));
  }
  SECTION("BI_BITFIELDS 5-6-5, masks after a 40-byte header") {
    auto d = info_header(40, 2, 1, 16, 3);
    d.u32(0xF800);
    d.u32(0x07E0);
    d.u32(0x001F);
    auto img = decode(bmp_file(d, {0x00, 0xF8, 0xE0, 0x07}));
    REQUIRE(img);
    CHECK(pixel_near(*img, 0, 255, 0, 0, 255, 0));
    CHECK(pixel_near(*img, 1, 0, 255, 0, 255, 0));
  }
}

TEST_CASE("BMP 32-bit alpha: real alpha kept, all-zero means opaque", "[codec][bmp][variants]") {
  SECTION("BI_BITFIELDS without an alpha mask ignores the fourth byte") {
    auto d = info_header(40, 1, 1, 32, 3);
    d.u32(0x00FF0000);
    d.u32(0x0000FF00);
    d.u32(0x000000FF);
    auto img = decode(bmp_file(d, {30, 20, 10, 0}));
    REQUIRE(img);
    CHECK(pixel_near(*img, 0, 10, 20, 30, 255, 0));
  }
  SECTION("V5 with an alpha mask and an embedded profile") {
    auto d = info_header(124, 2, 1, 32, 3, 0, {0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000});
    const auto icc = fixtures::adobe_rgb_icc();
    d.put32(56, 0x4D424544);  // PROFILE_EMBEDDED
    d.put32(112, 124 + 8);    // after the pixels, from the header start
    d.put32(116, static_cast<std::uint32_t>(icc.size()));
    std::vector<std::uint8_t> bits = {30, 20, 10, 128, 0, 0, 255, 255};
    bits.insert(bits.end(), icc.begin(), icc.end());
    auto img = decode(bmp_file(d, bits));
    REQUIRE(img);
    CHECK(pixel_near(*img, 0, 10, 20, 30, 128, 0));
    CHECK(pixel_near(*img, 1, 255, 0, 0, 255, 0));
    CHECK(img->icc == icc);
  }
  SECTION("BI_RGB with all-zero alpha bytes is opaque") {
    auto d = info_header(40, 2, 1, 32, 0);
    auto img = decode(bmp_file(d, {1, 2, 3, 0, 4, 5, 6, 0}));
    REQUIRE(img);
    CHECK(pixel_near(*img, 0, 3, 2, 1, 255, 0));
    CHECK(pixel_near(*img, 1, 6, 5, 4, 255, 0));
  }
  SECTION("BI_RGB with some alpha keeps it") {
    auto d = info_header(40, 2, 1, 32, 0);
    auto img = decode(bmp_file(d, {1, 2, 3, 0, 4, 5, 6, 200}));
    REQUIRE(img);
    CHECK(img->rgba[3] == 0);
    CHECK(img->rgba[7] == 200);
  }
  SECTION("V4 sRGB colour space tags the file") {
    auto d = info_header(108, 1, 1, 24, 0);
    d.put32(56, 0x73524742);  // LCS_sRGB
    auto img = decode(bmp_file(d, {1, 2, 3, 0}));
    REQUIRE(img);
    CHECK(img->tagged_srgb);
    CHECK(img->icc.empty());
  }
}

TEST_CASE("BMP RLE8 and RLE4", "[codec][bmp][variants]") {
  SECTION("RLE8: runs, absolute run, delta and end of bitmap") {
    auto d = info_header(40, 4, 3, 8, 1, 3);
    palette(d, {{{255, 0, 0}}, {{0, 255, 0}}, {{0, 0, 255}}});
    const std::vector<std::uint8_t> bits = {
        3, 1,              // bottom row: three green
        0, 0,              // end of line
        0, 3, 2, 0, 2, 0,  // absolute: blue red blue, padded to even
        0, 0,              //
        0, 2, 1, 0,        // delta +1: top row starts at x = 1
        1, 0,              // one red
        0, 1,              // end of bitmap
    };
    auto img = decode(bmp_file(d, bits));
    REQUIRE(img);
    // Top row (y = 0): x0 skipped (clear), x1 red, rest skipped.
    CHECK(img->rgba[3] == 0);
    CHECK(pixel_near(*img, 1, 255, 0, 0, 255, 0));
    CHECK(img->rgba[2 * 4 + 3] == 0);
    // Middle row: blue red blue, x3 clear.
    CHECK(pixel_near(*img, 4, 0, 0, 255, 255, 0));
    CHECK(pixel_near(*img, 5, 255, 0, 0, 255, 0));
    CHECK(pixel_near(*img, 6, 0, 0, 255, 255, 0));
    CHECK(img->rgba[7 * 4 + 3] == 0);
    // Bottom row: green green green, x3 clear.
    CHECK(pixel_near(*img, 8, 0, 255, 0, 255, 0));
    CHECK(pixel_near(*img, 10, 0, 255, 0, 255, 0));
    CHECK(img->rgba[11 * 4 + 3] == 0);
  }
  SECTION("RLE4: alternating nibbles and an odd absolute run") {
    auto d = info_header(40, 5, 2, 4, 2, 3);
    palette(d, {{{0, 0, 0}}, {{255, 0, 0}}, {{0, 255, 0}}});
    const std::vector<std::uint8_t> bits = {
        5, 0x12,           // bottom row: 1 2 1 2 1
        0, 0,
        0, 3, 0x21, 0x20,  // absolute: 2 1 2 (two bytes, already even)
        0, 1,
    };
    auto img = decode(bmp_file(d, bits));
    REQUIRE(img);
    CHECK(pixel_near(*img, 5, 255, 0, 0, 255, 0));
    CHECK(pixel_near(*img, 6, 0, 255, 0, 255, 0));
    CHECK(pixel_near(*img, 9, 255, 0, 0, 255, 0));
    CHECK(pixel_near(*img, 0, 0, 255, 0, 255, 0));
    CHECK(pixel_near(*img, 1, 255, 0, 0, 255, 0));
    CHECK(img->rgba[3 * 4 + 3] == 0);
  }
  SECTION("an absolute run past the data is corrupt") {
    auto d = info_header(40, 4, 1, 8, 1, 1);
    palette(d, {{{1, 2, 3}}});
    auto img = decode_bmp(bmp_file(d, {0, 40, 0, 0}));
    REQUIRE_FALSE(img);
    CHECK(img.error() == status::corrupt);
  }
}

TEST_CASE("BMP OS/2 BITMAPCOREHEADER", "[codec][bmp][variants]") {
  le_buf d;
  d.u32(12);
  d.u16(2);
  d.u16(1);
  d.u16(1);
  d.u16(8);
  palette(d, {{{9, 8, 7}}, {{200, 100, 50}}}, true);
  auto img = decode(bmp_file(d, {1, 0, 0, 0}));
  REQUIRE(img);
  CHECK(pixel_near(*img, 0, 200, 100, 50, 255, 0));
  CHECK(pixel_near(*img, 1, 9, 8, 7, 255, 0));
}

TEST_CASE("BMP with a PNG payload", "[codec][bmp][variants]") {
  const std::uint8_t rgba[] = {10, 20, 30, 255, 40, 50, 60, 128};
  const auto png = fixtures::png_rgba(2, 1, rgba);
  auto d = info_header(40, 2, 1, 0, 5);
  auto img = decode(bmp_file(d, png));
  REQUIRE(img);
  CHECK(img->format == format_family::bmp);
  CHECK(pixel_near(*img, 1, 40, 50, 60, 128, 0));
}

TEST_CASE("BMP variants fail cleanly when truncated or hostile", "[codec][bmp][variants]") {
  auto d = info_header(40, 7, 5, 4, 2, 16);
  palette(d, {{{1, 1, 1}}, {{2, 2, 2}}});
  const auto full = bmp_file(d, {7, 0x11, 0, 2, 255, 255, 3, 0x22, 0, 0, 0, 1});
  for (std::size_t len = 0; len < full.size(); ++len) {
    CAPTURE(len);
    auto r = decode_bmp(std::span<const std::uint8_t>(full.data(), len));
    if (r) CHECK(r->rgba.size() == static_cast<std::size_t>(r->width) * r->height * 4);
  }
  SECTION("bitfields with no masks") {
    auto b = info_header(40, 1, 1, 32, 3);
    b.u32(0);
    b.u32(0);
    b.u32(0);
    auto img = decode_bmp(bmp_file(b, {0, 0, 0, 0}));
    REQUIRE_FALSE(img);
    CHECK(img.error() == status::corrupt);
  }
  SECTION("RLE8 at the wrong depth") {
    auto img = decode_bmp(bmp_file(info_header(40, 1, 1, 24, 1), {0, 1}));
    REQUIRE_FALSE(img);
    CHECK(img.error() == status::unsupported_format);
  }
}

// ---- ICO --------------------------------------------------------------------

TEST_CASE("ICO 16-bit entry takes transparency from the AND mask", "[codec][ico][variants]") {
  auto d = info_header(40, 2, 2 * 2, 16, 0);
  // XOR rows (bottom-up, 4-byte stride): red green / blue white.
  const std::vector<std::uint8_t> xor_rows = {0x1F, 0x00, 0xFF, 0x7F, 0x00, 0x7C, 0xE0, 0x03};
  // AND rows: bottom row clear, top row's second pixel transparent.
  const std::vector<std::uint8_t> and_rows = {0, 0, 0, 0, 0x40, 0, 0, 0};
  d.b.insert(d.b.end(), xor_rows.begin(), xor_rows.end());
  d.b.insert(d.b.end(), and_rows.begin(), and_rows.end());
  auto file = fixtures::ico_file({{d.b, 2, 2, 16}});
  auto img = decode_ico(file);
  REQUIRE(img);
  CHECK(img->format == format_family::ico);
  CHECK(pixel_near(*img, 0, 255, 0, 0, 255, 0));
  CHECK(img->rgba[1 * 4 + 3] == 0);
  CHECK(pixel_near(*img, 2, 0, 0, 255, 255, 0));
  CHECK(pixel_near(*img, 3, 255, 255, 255, 255, 0));
}

// ---- TIFF -------------------------------------------------------------------

TEST_CASE("TIFF signed integers are shown offset-binary", "[codec][tiff][variants]") {
  SECTION("8-bit grey") {
    const std::int8_t px[] = {-128, 0, 127};
    fixtures::tiff_spec s;
    s.width = 3;
    s.spp = 1;
    s.photometric = PHOTOMETRIC_MINISBLACK;
    s.sample_format = SAMPLEFORMAT_INT;
    auto bytes = fixtures::tiff_write(s, reinterpret_cast<const std::uint8_t*>(px));
    REQUIRE_FALSE(bytes.empty());
    auto img = decode_tiff(bytes);
    REQUIRE(img);
    CHECK(img->rgba[0] == 0);
    CHECK(img->rgba[4] == 128);
    CHECK(img->rgba[8] == 255);
  }
  SECTION("16-bit RGB") {
    const std::int16_t px[] = {32767, -32768, 0};
    fixtures::tiff_spec s;
    s.bps = 16;
    s.sample_format = SAMPLEFORMAT_INT;
    auto bytes = fixtures::tiff_write(s, reinterpret_cast<const std::uint8_t*>(px));
    auto img = decode_tiff(bytes);
    REQUIRE(img);
    CHECK(pixel_near(*img, 0, 255, 0, 128, 255, 0));
  }
  SECTION("signed palette is still refused") {
    const std::int8_t px[] = {0};
    fixtures::tiff_spec s;
    s.spp = 1;
    s.photometric = PHOTOMETRIC_PALETTE;
    s.sample_format = SAMPLEFORMAT_INT;
    s.cmap.assign(3 * 256, 0);
    auto bytes = fixtures::tiff_write(s, reinterpret_cast<const std::uint8_t*>(px));
    REQUIRE_FALSE(bytes.empty());
    auto img = decode_tiff(bytes);
    REQUIRE_FALSE(img);
    CHECK(img.error() == status::unsupported_format);
  }
}

TEST_CASE("TIFF 16-bit half float, untagged, is sRGB-encoded", "[codec][tiff][variants]") {
  // 0.0, 0.5, 1.0, 2.0 (clamped), -1.0 (clamped) as IEEE half.
  const std::uint16_t px[] = {0x0000, 0x3800, 0x3C00, 0x4000, 0xBC00};
  fixtures::tiff_spec s;
  s.width = 5;
  s.spp = 1;
  s.bps = 16;
  s.photometric = PHOTOMETRIC_MINISBLACK;
  s.sample_format = SAMPLEFORMAT_IEEEFP;
  auto bytes = fixtures::tiff_write(s, reinterpret_cast<const std::uint8_t*>(px));
  REQUIRE_FALSE(bytes.empty());
  auto img = decode_tiff(bytes);
  REQUIRE(img);
  CHECK(img->rgba[0] == 0);
  CHECK(near(img->rgba[4], 188, 1));  // sRGB encode of linear 0.5
  CHECK(img->rgba[8] == 255);
  CHECK(img->rgba[12] == 255);
  CHECK(img->rgba[16] == 0);
}

TEST_CASE("TIFF sub-byte separate planes", "[codec][tiff][variants]") {
  // 3 x 2, 4-bit RGB, one plane per channel. Written by hand: the shared
  // fixture writer only does byte-sized separate samples.
  fixtures::tiff_sink sink;
  TIFF* tif = TIFFClientOpen("fixture", "w", &sink, fixtures::tiff_sink_read, fixtures::tiff_sink_write,
                             fixtures::tiff_sink_seek, fixtures::tiff_sink_close, fixtures::tiff_sink_size,
                             fixtures::tiff_sink_map, fixtures::tiff_sink_unmap);
  REQUIRE(tif);
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 3u);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 2u);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 4);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 2u);
  // Per plane, per row: three nibbles packed MSB first, padded to 2 bytes.
  const std::uint8_t planes[3][2][2] = {
      {{0xF0, 0x80}, {0x00, 0x00}},  // R: 15 0 8 / 0 0 0
      {{0x0F, 0x00}, {0x48, 0xC0}},  // G: 0 15 0 / 4 8 12
      {{0x00, 0xF0}, {0x11, 0x10}},  // B: 0 0 15 / 1 1 1
  };
  bool ok = true;
  for (std::uint16_t p = 0; p < 3; ++p) {
    for (std::uint32_t y = 0; y < 2; ++y) {
      ok &= TIFFWriteScanline(tif, const_cast<std::uint8_t*>(planes[p][y]), y, p) == 1;
    }
  }
  TIFFClose(tif);
  REQUIRE(ok);
  auto img = decode_tiff(sink.data);
  REQUIRE(img);
  CHECK(pixel_near(*img, 0, 255, 0, 0, 255, 0));
  CHECK(pixel_near(*img, 1, 0, 255, 0, 255, 0));
  CHECK(pixel_near(*img, 2, 136, 0, 255, 255, 0));
  CHECK(pixel_near(*img, 3, 0, 68, 17, 255, 0));
  CHECK(pixel_near(*img, 5, 0, 204, 17, 255, 0));
}



