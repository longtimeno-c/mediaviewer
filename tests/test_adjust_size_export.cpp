// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #288 Adjust Size through export (docs/design/07 "Adjust Size"): the stated
// resolution reaches the file on every path — lossless JPEG, re-encode, PNG —
// in the encoder's header and in EXIF X/YResolution alike; a stack that states
// none keeps the source's; a source that states none is written with none.
// Fixtures are built in the test.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <span>
#include <vector>

#include "codec/decode.h"
#include "codec/exif.h"
#include "edit/adjust_size.h"
#include "edit/edit_stack.h"
#include "edit/encode.h"
#include "edit/export.h"

namespace {

namespace edit = mv::edit;
using Catch::Approx;
using edit::resolution;
using edit::resolution_unit;
using mv::codec::raster;

raster pattern(std::uint32_t w, std::uint32_t h) {
  raster r;
  r.width = w;
  r.height = h;
  r.format = mv::codec::format_family::jpeg;
  r.rgba.resize(static_cast<std::size_t>(w) * h * 4);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      std::uint8_t* p = r.rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4;
      p[0] = static_cast<std::uint8_t>(x * 255 / (w - 1));
      p[1] = static_cast<std::uint8_t>(y * 255 / (h - 1));
      p[2] = static_cast<std::uint8_t>((x * 7 + y * 13) & 0xFF);
      p[3] = 255;
    }
  }
  return r;
}

// IFD0: Orientation 1, XResolution, YResolution, ResolutionUnit (inch).
std::vector<std::uint8_t> exif_at(std::uint32_t ppi) {
  std::vector<std::uint8_t> b = {'I', 'I', 42, 0, 8, 0, 0, 0};
  auto u16 = [&](std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
  };
  auto u32 = [&](std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
  };
  const std::uint32_t values = 8 + 2 + 12 * 4 + 4;
  u16(4);
  u16(0x0112); u16(3); u32(1); u32(1);
  u16(0x011A); u16(5); u32(1); u32(values);
  u16(0x011B); u16(5); u32(1); u32(values + 8);
  u16(0x0128); u16(3); u32(1); u32(2);
  u32(0);
  u32(ppi); u32(1);
  u32(ppi); u32(1);
  return b;
}

std::vector<std::uint8_t> encode(const raster& r, edit::image_format f, resolution stated,
                                 const edit::metadata_blobs& meta = {}) {
  edit::encode_options opt;
  opt.format = f;
  opt.quality = 95;
  opt.stated = stated;
  auto bytes = edit::encode(r, opt, meta);
  REQUIRE(bytes);
  return std::move(bytes).value();
}

double exif_ppi(std::span<const std::uint8_t> jpeg) {
  const auto e = mv::codec::find_jpeg_exif(jpeg);
  if (!e) return -1;
  const auto r = mv::codec::exif_resolution(jpeg.subspan(e->offset, e->size));
  return r ? r->x : 0.0;
}

int jfif_ppi(std::span<const std::uint8_t> jpeg) {
  const auto j = mv::codec::jpeg_jfif_density(jpeg);
  if (!j) return -1;
  return j->unit == 1 ? j->x : 0;
}

edit::geometry stating(double ppi, edit::geometry g = {}) {
  g.resize.stated = resolution{ppi, resolution_unit::per_inch};
  return g;
}

}  // namespace

TEST_CASE("source_resolution reads JFIF, EXIF and pHYs", "[edit][adjust_size][export]") {
  const raster img = pattern(64, 48);
  // JFIF wins over EXIF in a JPEG.
  edit::metadata_blobs meta;
  meta.exif = exif_at(72);
  const auto both = encode(img, edit::image_format::jpeg, {300.0, resolution_unit::per_inch}, meta);
  CHECK(edit::source_resolution(both) == resolution{300.0, resolution_unit::per_inch});
  // JFIF unit 0 (no unit): EXIF's.
  const auto exif_only = encode(img, edit::image_format::jpeg, {}, meta);
  CHECK(edit::source_resolution(exif_only) == resolution{72.0, resolution_unit::per_inch});
  // Per cm stays per cm.
  const auto cm = encode(img, edit::image_format::jpeg, {edit::ppi_from(40, resolution_unit::per_cm),
                                                         resolution_unit::per_cm});
  const resolution rc = edit::source_resolution(cm);
  CHECK(rc.unit == resolution_unit::per_cm);
  CHECK(rc.ppi == Approx(101.6));
  // Nothing stated.
  CHECK_FALSE(edit::source_resolution(encode(img, edit::image_format::jpeg, {})).stated());
  // PNG pHYs, snapped back to the whole ppi it was written from.
  const auto png = encode(img, edit::image_format::png, {300.0, resolution_unit::per_inch});
  CHECK(edit::source_resolution(png) == resolution{300.0, resolution_unit::per_inch});
  CHECK_FALSE(edit::source_resolution(encode(img, edit::image_format::png, {})).stated());
  // PNG with only eXIf.
  const auto png_exif = encode(img, edit::image_format::png, {}, meta);
  CHECK(edit::source_resolution(png_exif) == resolution{72.0, resolution_unit::per_inch});
  // Another format: the carried EXIF.
  const std::vector<std::uint8_t> other = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
  CHECK(edit::source_resolution(other, &meta) == resolution{72.0, resolution_unit::per_inch});
  CHECK_FALSE(edit::source_resolution(other).stated());
}

TEST_CASE("a lossless rotate states the new resolution, pixels untouched", "[edit][adjust_size][export]") {
  edit::metadata_blobs meta;
  meta.exif = exif_at(72);
  const auto src = encode(pattern(64, 48), edit::image_format::jpeg, {}, meta);

  edit::geometry g;
  g.orient = mv::codec::kRotateCw;
  edit::export_options opt;
  auto plain = edit::export_image(src, g, opt);
  REQUIRE(plain);
  REQUIRE(plain->lossless);
  CHECK(exif_ppi(plain->bytes) == 72.0);  // the source's, kept

  auto stated = edit::export_image(src, stating(300.0, g), opt);
  REQUIRE(stated);
  CHECK(stated->lossless);  // a resolution alone never forces a re-encode
  CHECK(jfif_ppi(stated->bytes) == 300);
  CHECK(exif_ppi(stated->bytes) == 300.0);

  auto a = mv::codec::decode_jpeg(plain->bytes);
  auto b = mv::codec::decode_jpeg(stated->bytes);
  REQUIRE(a);
  REQUIRE(b);
  CHECK(a->rgba == b->rgba);

  // A resolution-only stack (no rotate) is lossless as well.
  auto only = edit::export_image(src, stating(240.0), opt);
  REQUIRE(only);
  CHECK(only->lossless);
  CHECK(jfif_ppi(only->bytes) == 240);
  CHECK(exif_ppi(only->bytes) == 240.0);
}

TEST_CASE("a re-encode writes one resolution, header and EXIF", "[edit][adjust_size][export]") {
  edit::metadata_blobs meta;
  meta.exif = exif_at(72);
  const auto src = encode(pattern(64, 48), edit::image_format::jpeg, {}, meta);

  edit::geometry g;
  g.resize.mode = edit::resize_mode::exact;
  g.resize.a = 32;
  g.resize.b = 24;
  auto out = edit::export_image(src, stating(150.0, g), edit::export_options{});
  REQUIRE(out);
  CHECK_FALSE(out->lossless);
  CHECK(out->width == 32);
  CHECK(jfif_ppi(out->bytes) == 150);
  CHECK(exif_ppi(out->bytes) == 150.0);

  // The export dialog's long edge replaces the pixel size only.
  edit::export_options le;
  le.long_edge = 16;
  auto small = edit::export_image(src, stating(150.0, g), le);
  REQUIRE(small);
  CHECK(small->width == 16);
  CHECK(jfif_ppi(small->bytes) == 150);

  // To PNG: pHYs, and the eXIf agrees.
  edit::export_options png;
  png.encode.format = edit::image_format::png;
  auto p = edit::export_image(src, stating(150.0, g), png);
  REQUIRE(p);
  CHECK(edit::source_resolution(p->bytes) == resolution{150.0, resolution_unit::per_inch});

  // Under metadata policy none the resolution is still written (it is the
  // picture's size, like the ICC profile); EXIF is gone.
  edit::export_options none;
  none.policy = edit::metadata_policy::none;
  auto n = edit::export_image(src, stating(150.0, g), none);
  REQUIRE(n);
  CHECK(jfif_ppi(n->bytes) == 150);
  CHECK(exif_ppi(n->bytes) == -1);
}

TEST_CASE("an unstated stack keeps the source's resolution, or none", "[edit][adjust_size][export]") {
  edit::geometry g;
  g.resize.mode = edit::resize_mode::percent;
  g.resize.percent = 50.0f;

  // JFIF 200 dpi in, no EXIF: JPEG and PNG out both say 200.
  const auto jfif = encode(pattern(64, 48), edit::image_format::jpeg, {200.0, resolution_unit::per_inch});
  auto j = edit::export_image(jfif, g, edit::export_options{});
  REQUIRE(j);
  CHECK(jfif_ppi(j->bytes) == 200);
  edit::export_options png;
  png.encode.format = edit::image_format::png;
  auto p = edit::export_image(jfif, g, png);
  REQUIRE(p);
  CHECK(edit::source_resolution(p->bytes) == resolution{200.0, resolution_unit::per_inch});

  // A source that states nothing: nothing stated in the export.
  const auto bare = encode(pattern(64, 48), edit::image_format::jpeg, {});
  auto b = edit::export_image(bare, g, edit::export_options{});
  REQUIRE(b);
  CHECK(jfif_ppi(b->bytes) == 0);  // JFIF unit 0, aspect only
  auto bp = edit::export_image(bare, g, png);
  REQUIRE(bp);
  CHECK_FALSE(edit::source_resolution(bp->bytes).stated());
}
