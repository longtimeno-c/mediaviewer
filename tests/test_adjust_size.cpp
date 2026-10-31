// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #288 Adjust Size, the shared core (docs/design/07 "Adjust Size"): the size and
// unit maths (edit/adjust_size.h), the stated resolution in the edit stack,
// and the encoders writing it — JFIF density read back by libjpeg itself,
// pHYs read back by libspng itself, EXIF X/YResolution patched in place.
// Needs only edit_stack.cpp, encode.cpp and codec/exif.cpp, so the Linux
// portable build runs it too.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include <jpeglib.h>
#include <spng.h>

#include "codec/exif.h"
#include "codec/raster.h"
#include "edit/adjust_size.h"
#include "edit/edit_stack.h"
#include "edit/encode.h"

namespace {

namespace edit = mv::edit;
using Catch::Approx;
using edit::adjust_size;
using edit::resolution;
using edit::resolution_unit;
using edit::size2;
using edit::size_unit;

constexpr size_unit kAllUnits[] = {size_unit::pixels, size_unit::percent, size_unit::inches,
                                   size_unit::cm,     size_unit::mm,      size_unit::points};

// ---- compile-time checks: the header is constexpr all the way down ----
static_assert(edit::to_pixels(1.0, size_unit::inches, 300.0, 0.0) == 300.0);
static_assert(edit::to_pixels(72.0, size_unit::points, 300.0, 0.0) == 300.0);
static_assert(edit::ppi_from(1.0, resolution_unit::per_cm) == 2.54);
static_assert(edit::round_pixels(0.2) == 1 && edit::round_pixels(2.5) == 3 &&
              edit::round_pixels(1e12) == edit::kMaxPixels);
static_assert(edit::adjust_size::open({4000, 3000}, {}).output() == size2{4000, 3000});
static_assert(edit::per_metre(300.0) == 11811 && edit::ppi_from_per_metre(11811) == 300.0);

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

// A little-endian TIFF with IFD0: XResolution, YResolution (RATIONAL, out of
// line), and ResolutionUnit when `with_unit`.
std::vector<std::uint8_t> resolution_exif(std::uint32_t num, std::uint32_t den, bool with_unit,
                                          std::uint16_t unit = 2) {
  std::vector<std::uint8_t> b = {'I', 'I', 42, 0, 8, 0, 0, 0};
  auto u16 = [&](std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
  };
  auto u32 = [&](std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
  };
  const auto n = static_cast<std::uint16_t>(with_unit ? 3 : 2);
  const std::uint32_t values = 8 + 2 + 12u * n + 4;
  u16(n);
  u16(0x011A); u16(5); u32(1); u32(values);
  u16(0x011B); u16(5); u32(1); u32(values + 8);
  if (with_unit) { u16(0x0128); u16(3); u32(1); u32(unit); }
  u32(0);
  u32(num); u32(den);
  u32(num); u32(den);
  return b;
}

struct jfif_read {
  bool saw = false;
  int unit = -1;
  int x = 0, y = 0;
};

struct trap {
  jpeg_error_mgr pub;
  jmp_buf jump;
};

// libjpeg's own reading of the header: an oracle independent of codec/exif.
jfif_read libjpeg_jfif(const std::vector<std::uint8_t>& jpeg) {
  jpeg_decompress_struct cinfo{};
  trap t{};
  cinfo.err = jpeg_std_error(&t.pub);
  t.pub.error_exit = [](j_common_ptr c) { longjmp(reinterpret_cast<trap*>(c->err)->jump, 1); };
  t.pub.output_message = [](j_common_ptr) {};
  jfif_read r;
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4611)
#endif
  if (setjmp(t.jump)) {
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    jpeg_destroy_decompress(&cinfo);
    return r;
  }
  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, jpeg.data(), static_cast<unsigned long>(jpeg.size()));
  if (jpeg_read_header(&cinfo, TRUE) == JPEG_HEADER_OK) {
    r.saw = cinfo.saw_JFIF_marker != 0;
    r.unit = cinfo.density_unit;
    r.x = cinfo.X_density;
    r.y = cinfo.Y_density;
  }
  jpeg_destroy_decompress(&cinfo);
  return r;
}

// libspng's own reading of pHYs. False when the chunk is absent.
bool libspng_phys(const std::vector<std::uint8_t>& png, spng_phys& out) {
  spng_ctx* ctx = spng_ctx_new(0);
  REQUIRE(ctx != nullptr);
  REQUIRE(spng_set_png_buffer(ctx, png.data(), png.size()) == 0);
  const int rc = spng_get_phys(ctx, &out);
  spng_ctx_free(ctx);
  return rc == 0;
}

std::vector<std::uint8_t> encode_with(const raster& r, edit::image_format f, resolution stated,
                                      const edit::metadata_blobs& meta = {}) {
  edit::encode_options opt;
  opt.format = f;
  opt.stated = stated;
  auto bytes = edit::encode(r, opt, meta);
  REQUIRE(bytes);
  return std::move(bytes).value();
}

}  // namespace

// ---------------------------------------------------------------- maths

TEST_CASE("every unit round-trips through pixels at every resolution", "[edit][adjust_size]") {
  for (const double ppi : {72.0, 96.0, 150.0, 300.0, 118.11 * 2.54, 1.0, 65535.0}) {
    for (const size_unit u : kAllUnits) {
      for (const double px : {1.0, 640.0, 1234.5, 6000.0}) {
        const double v = edit::from_pixels(px, u, ppi, 4000.0);
        CHECK(edit::to_pixels(v, u, ppi, 4000.0) == Approx(px).epsilon(1e-12));
      }
    }
  }
  // The definitions, at 300 ppi.
  CHECK(edit::from_pixels(300.0, size_unit::inches, 300.0, 0) == Approx(1.0));
  CHECK(edit::from_pixels(300.0, size_unit::cm, 300.0, 0) == Approx(2.54));
  CHECK(edit::from_pixels(300.0, size_unit::mm, 300.0, 0) == Approx(25.4));
  CHECK(edit::from_pixels(300.0, size_unit::points, 300.0, 0) == Approx(72.0));
  CHECK(edit::from_pixels(2000.0, size_unit::percent, 300.0, 4000.0) == Approx(50.0));
  // Resolution units.
  CHECK(edit::ppi_from(118.11, resolution_unit::per_cm) == Approx(300.0).margin(0.001));
  CHECK(edit::resolution_in(300.0, resolution_unit::per_cm) == Approx(118.110236).margin(1e-6));
  CHECK(edit::resolution_in(edit::ppi_from(42.0, resolution_unit::per_cm), resolution_unit::per_cm) ==
        Approx(42.0));
  // pHYs: every whole ppi comes back whole; a fractional one comes back
  // within the integer's half step.
  int bad = 0;
  for (std::uint32_t ppi = 1; ppi <= 65535; ++ppi) {
    if (edit::ppi_from_per_metre(edit::per_metre(ppi)) != static_cast<double>(ppi)) ++bad;
  }
  CHECK(bad == 0);
  CHECK(edit::ppi_from_per_metre(edit::per_metre(299.5)) == Approx(299.5).margin(0.0127));
}

TEST_CASE("the sheet reads back what was typed, in every unit", "[edit][adjust_size]") {
  for (const size_unit u : kAllUnits) {
    adjust_size s = adjust_size::open({4000, 3000}, resolution{300.0, resolution_unit::per_inch});
    s.proportional = false;
    const double typed = u == size_unit::pixels ? 1600.0 : u == size_unit::percent ? 40.0 : 5.0;
    REQUIRE(s.set_width(typed, u));
    CHECK(s.width_in(u) == Approx(typed));
    REQUIRE(s.set_height(typed, u));
    CHECK(s.height_in(u) == Approx(typed));
  }
  adjust_size s = adjust_size::open({4000, 3000}, resolution{300.0, resolution_unit::per_inch});
  REQUIRE(s.set_width(10.0, size_unit::cm));
  CHECK(s.output() == size2{1181, 886});  // 10 cm at 300 ppi = 1181.1 px; 885.83 → 886
  CHECK(s.width_in(size_unit::mm) == Approx(100.0));
}

TEST_CASE("scale proportionally follows the opened aspect, rounded once", "[edit][adjust_size]") {
  adjust_size s = adjust_size::open({4000, 3000}, {});
  REQUIRE(s.set_width(1001, size_unit::pixels));
  CHECK(s.height == Approx(750.75));
  CHECK(s.output() == size2{1001, 751});
  // Chained edits never drift: the followed side comes from the aspect, not
  // from the other side's rounded value.
  for (int i = 0; i < 50; ++i) {
    REQUIRE(s.set_height(s.height_in(size_unit::cm), size_unit::cm));
    REQUIRE(s.set_width(s.width_in(size_unit::percent), size_unit::percent));
  }
  CHECK(s.output() == size2{1001, 751});
  REQUIRE(s.set_height(1080, size_unit::pixels));
  CHECK(s.output() == size2{1440, 1080});

  // Unlocked: only the typed side moves; locking again re-derives the height.
  s.set_proportional(false);
  REQUIRE(s.set_height(500, size_unit::pixels));
  CHECK(s.output() == size2{1440, 500});
  s.set_proportional(true);
  CHECK(s.output() == size2{1440, 1080});

  // Nonsense is refused and changes nothing.
  const size2 before = s.output();
  CHECK_FALSE(s.set_width(0, size_unit::pixels));
  CHECK_FALSE(s.set_width(-3, size_unit::inches));
  CHECK_FALSE(s.set_width(std::numeric_limits<double>::quiet_NaN(), size_unit::pixels));
  CHECK_FALSE(s.set_resolution(0, resolution_unit::per_inch));
  CHECK(s.output() == before);
  // Clamped to the encoder's ceiling, never 0.
  REQUIRE(s.set_width(1e8, size_unit::pixels));
  CHECK(s.output().w == edit::kMaxPixels);
  REQUIRE(s.set_width(0.01, size_unit::pixels));
  CHECK(s.output() == size2{1, 1});
}

TEST_CASE("fit into fits the box and keeps the ratio", "[edit][adjust_size]") {
  for (const size2 box : edit::kFitPresets) {
    for (const size2 src : {size2{4000, 3000}, size2{3024, 4032}, size2{6000, 4000},
                            size2{320, 200}, size2{1920, 1080}}) {
      adjust_size s = adjust_size::open(src, {});
      REQUIRE(s.fit_into(box));
      const size2 o = s.output();
      CHECK(o.w <= box.w);
      CHECK(o.h <= box.h);
      CHECK((o.w == box.w || o.h == box.h));  // touches the box
      // Ratio kept to within the rounding of one side.
      CHECK(std::abs(static_cast<double>(o.w) * src.h - static_cast<double>(o.h) * src.w) <=
            0.5 * std::max(src.w, src.h) + 1);
      CHECK(s.resample);
      CHECK(s.proportional);
    }
  }
  adjust_size s = adjust_size::open({4000, 3000}, {});
  REQUIRE(s.fit_into({1920, 1080}));
  CHECK(s.output() == size2{1440, 1080});
  // Portrait into a landscape box: the box is not turned.
  s = adjust_size::open({3024, 4032}, {});
  REQUIRE(s.fit_into({1920, 1080}));
  CHECK(s.output() == size2{810, 1080});
  // A smaller picture is enlarged by default (Preview scales up or down),
  // and not with enlarge = false.
  s = adjust_size::open({640, 480}, {});
  REQUIRE(s.fit_into({1024, 768}));
  CHECK(s.output() == size2{1024, 768});
  REQUIRE(s.fit_into({1024, 768}, false));
  CHECK(s.output() == size2{640, 480});
  CHECK_FALSE(s.changes_pixels());
  CHECK_FALSE(s.fit_into({0, 768}));
  // Fit into turns resample back on.
  s.set_resample(false);
  REQUIRE(s.fit_into({320, 240}));
  CHECK(s.resample);
  CHECK(s.output() == size2{320, 240});
}

TEST_CASE("resample on: resolution keeps the physical size", "[edit][adjust_size]") {
  adjust_size s = adjust_size::open({3000, 2000}, resolution{72.0, resolution_unit::per_inch});
  const double inches = s.width_in(size_unit::inches);
  REQUIRE(s.set_resolution(144.0, resolution_unit::per_inch));
  CHECK(s.output() == size2{6000, 4000});
  CHECK(s.width_in(size_unit::inches) == Approx(inches));
  REQUIRE(s.set_resolution(118.11, resolution_unit::per_cm));
  CHECK(s.unit == resolution_unit::per_cm);
  CHECK(s.resolution_value(resolution_unit::per_cm) == Approx(118.11));
  CHECK(s.width_in(size_unit::inches) == Approx(inches));
  CHECK(s.changes_pixels());
  CHECK(s.changes_resolution());
}

TEST_CASE("resample off keeps the pixels and states the resolution", "[edit][adjust_size]") {
  adjust_size s = adjust_size::open({3000, 2000}, resolution{72.0, resolution_unit::per_inch});
  REQUIRE(s.set_width(1200, size_unit::pixels));
  s.set_resample(false);
  CHECK(s.output() == size2{3000, 2000});  // back to the opened pixels

  REQUIRE(s.set_width(10.0, size_unit::inches));
  CHECK(s.ppi == Approx(300.0));
  CHECK(s.output() == size2{3000, 2000});
  CHECK(s.height_in(size_unit::inches) == Approx(2000.0 / 300.0));  // height follows
  CHECK_FALSE(s.changes_pixels());
  CHECK(s.changes_resolution());

  REQUIRE(s.set_height(10.0, size_unit::cm));
  CHECK(s.ppi == Approx(2000.0 / (10.0 / 2.54)));
  CHECK(s.height_in(size_unit::cm) == Approx(10.0));

  // Percent scales the physical size: 50 % is twice the opened resolution.
  REQUIRE(s.set_width(50.0, size_unit::percent));
  CHECK(s.ppi == Approx(144.0));
  CHECK(s.width_in(size_unit::percent) == Approx(50.0));
  CHECK(s.width_in(size_unit::inches) == Approx(3000.0 / 144.0));

  // A pixel value cannot be met without resampling.
  CHECK_FALSE(s.set_width(1500, size_unit::pixels));
  // The resolution alone changes the physical size.
  REQUIRE(s.set_resolution(600.0, resolution_unit::per_inch));
  CHECK(s.output() == size2{3000, 2000});
  CHECK(s.width_in(size_unit::inches) == Approx(5.0));
}

TEST_CASE("resulting size: percent and an estimated file size", "[edit][adjust_size]") {
  adjust_size s = adjust_size::open({4000, 3000}, {});
  CHECK(s.percent_of_original() == Approx(100.0));
  REQUIRE(s.set_width(50.0, size_unit::percent));
  CHECK(s.output() == size2{2000, 1500});
  CHECK(s.percent_of_original() == Approx(25.0));
  // Bytes per pixel of the file, times the output's pixels.
  CHECK(edit::estimated_bytes(1'900'000, {4000, 3000}, s.output()) == 475'000u);
  // No pixel change: the file's own size, exactly.
  CHECK(edit::estimated_bytes(1'900'000, {4000, 3000}, {4000, 3000}) == 1'900'000u);
  CHECK(edit::estimated_bytes(1'900'000, {0, 0}, {10, 10}) == 0);
}

TEST_CASE("a source with no resolution shows 72 and keeps stating none", "[edit][adjust_size]") {
  adjust_size s = adjust_size::open({4000, 3000}, {});
  CHECK(s.ppi == edit::kDefaultPpi);
  CHECK_FALSE(s.changes_resolution());
  const edit::resize_spec r = edit::adjust_size_spec({}, s);
  CHECK(r == edit::resize_spec{});  // nothing changed → no resize, no resolution
}

// ---------------------------------------------------------------- stack

TEST_CASE("older resize specs and stacks mean what they did", "[edit][adjust_size][stack]") {
  // The four-field spelling still compiles and leaves the resolution unstated.
  const edit::resize_spec old{edit::resize_mode::long_edge, 2048, 0, 100.0f};
  CHECK_FALSE(old.stated.stated());
  edit::edit_stack s;
  s.push(edit::op{edit::op_kind::rotate_cw});
  edit::op r{edit::op_kind::resize};
  r.resize = old;
  s.push(r);
  const edit::geometry g = edit::fold(s);
  CHECK(g.resize == old);
  CHECK_FALSE(g.resize.stated.stated());
  const edit::placement p = edit::place(g, {4000, 3000});
  CHECK(p.output == size2{1536, 2048});
}

TEST_CASE("Adjust Size is one resize op: undo, redo and compare", "[edit][adjust_size][stack]") {
  edit::edit_stack s;
  s.push(edit::op{edit::op_kind::rotate_cw});
  const size2 src{4000, 3000};
  const edit::geometry before = edit::fold(s);
  const size2 shown = edit::place(before, src).output;  // 3000 × 4000
  REQUIRE(shown == size2{3000, 4000});

  // Resample off, 10 in wide: same pixels, 300 ppi stated.
  adjust_size sheet = adjust_size::open(shown, {});
  sheet.set_resample(false);
  REQUIRE(sheet.set_width(10.0, size_unit::inches));
  const edit::op o = edit::adjust_size_op(before.resize, sheet);
  CHECK(o.kind == edit::op_kind::resize);
  CHECK(o.resize.mode == edit::resize_mode::none);  // pixels still follow the crop
  CHECK(o.resize.stated == resolution{300.0, resolution_unit::per_inch});
  s.push(o);

  const edit::geometry after = edit::fold(s);
  CHECK(after.resize.stated.ppi == Approx(300.0));
  CHECK(after.identity() == false);  // the rotate
  CHECK(edit::place(after, src).output == shown);
  CHECK(edit::place(after, src).exact_copy);  // no resample: still lossless-eligible

  // Compare: a copy of the stack folds to the same geometry; a different
  // resolution does not compare equal.
  const edit::edit_stack copy = s;
  CHECK(edit::fold(copy).resize == after.resize);
  edit::resize_spec other = after.resize;
  other.stated.ppi = 301.0;
  CHECK_FALSE(other == after.resize);

  // Undo pops exactly that one step; redo (pushing it again) restores it.
  REQUIRE(s.undo());
  CHECK(edit::fold(s).resize == before.resize);
  CHECK_FALSE(edit::fold(s).resize.stated.stated());
  s.push(o);
  CHECK(edit::fold(s).resize == after.resize);

  // A second sheet opens on what is in force and, resampling, keeps the
  // stated resolution while setting exact pixels.
  adjust_size again = adjust_size::open(edit::place(edit::fold(s), src).output,
                                        edit::fold(s).resize.stated);
  CHECK(again.ppi == Approx(300.0));
  REQUIRE(again.set_width(50.0, size_unit::percent));
  const edit::resize_spec r2 = edit::adjust_size_spec(edit::fold(s).resize, again);
  CHECK(r2.mode == edit::resize_mode::exact);
  CHECK(r2.a == 1500);
  CHECK(r2.b == 2000);
  CHECK(r2.stated == resolution{300.0, resolution_unit::per_inch});
  edit::op o2{edit::op_kind::resize};
  o2.resize = r2;
  s.push(o2);
  CHECK(edit::place(edit::fold(s), src).output == size2{1500, 2000});
  REQUIRE(s.undo());
  CHECK(edit::fold(s).resize == after.resize);
}

TEST_CASE("a resolution given per cm is kept per cm", "[edit][adjust_size][stack]") {
  adjust_size sheet = adjust_size::open({1000, 1000}, {});
  sheet.set_resample(false);
  REQUIRE(sheet.set_resolution(40.0, resolution_unit::per_cm));
  const edit::resize_spec r = edit::adjust_size_spec({}, sheet);
  CHECK(r.stated.unit == resolution_unit::per_cm);
  CHECK(r.stated.ppi == Approx(101.6));
  CHECK(r.mode == edit::resize_mode::none);
}

// ---------------------------------------------------------------- encoders

TEST_CASE("JPEG writes the stated resolution as the JFIF density", "[edit][adjust_size][encode]") {
  const raster img = pattern(64, 48);

  const auto inch = encode_with(img, edit::image_format::jpeg, {300.0, resolution_unit::per_inch});
  jfif_read j = libjpeg_jfif(inch);
  CHECK(j.saw);
  CHECK(j.unit == 1);
  CHECK(j.x == 300);
  CHECK(j.y == 300);
  const auto ours = mv::codec::jpeg_jfif_density(inch);
  REQUIRE(ours);
  CHECK(ours->unit == 1);
  CHECK(ours->x == 300);

  const auto cm = encode_with(img, edit::image_format::jpeg, {300.0, resolution_unit::per_cm});
  j = libjpeg_jfif(cm);
  CHECK(j.unit == 2);
  CHECK(j.x == 118);  // 118.11 rounds: JFIF density is an integer
  CHECK(j.y == 118);

  // Not stated: libjpeg's default, aspect 1:1 with no unit (as before #288).
  const auto none = encode_with(img, edit::image_format::jpeg, {});
  j = libjpeg_jfif(none);
  CHECK(j.unit == 0);
  CHECK(j.x == 1);
  CHECK(j.y == 1);
}

TEST_CASE("PNG writes the stated resolution as pHYs", "[edit][adjust_size][encode]") {
  const raster img = pattern(32, 24);
  const auto png = encode_with(img, edit::image_format::png, {300.0, resolution_unit::per_inch});
  spng_phys phys{};
  REQUIRE(libspng_phys(png, phys));
  CHECK(phys.unit_specifier == 1);
  CHECK(phys.ppu_x == 11811);
  CHECK(phys.ppu_y == 11811);
  CHECK(edit::ppi_from_per_metre(phys.ppu_x) == 300.0);

  const auto cm = encode_with(img, edit::image_format::png, {edit::ppi_from(40.0, resolution_unit::per_cm),
                                                             resolution_unit::per_cm});
  REQUIRE(libspng_phys(cm, phys));
  CHECK(phys.ppu_x == 4000);  // 40 per cm is 4000 per metre, exactly

  const auto none = encode_with(img, edit::image_format::png, {});
  CHECK_FALSE(libspng_phys(none, phys));
}

TEST_CASE("EXIF X/YResolution are read and patched in place", "[edit][adjust_size][exif]") {
  std::vector<std::uint8_t> tiff = resolution_exif(72, 1, true);
  const std::size_t size = tiff.size();
  auto r = mv::codec::exif_resolution(tiff);
  REQUIRE(r);
  CHECK(r->x == 72.0);
  CHECK(r->unit == 2);

  REQUIRE(mv::codec::exif_set_resolution(tiff, 300.0, false));
  CHECK(tiff.size() == size);
  r = mv::codec::exif_resolution(tiff);
  REQUIRE(r);
  CHECK(r->x == 300.0);
  CHECK(r->y == 300.0);
  CHECK(r->unit == 2);

  // Per cm, with a ResolutionUnit entry to say so.
  REQUIRE(mv::codec::exif_set_resolution(tiff, 300.0, true));
  r = mv::codec::exif_resolution(tiff);
  REQUIRE(r);
  CHECK(r->unit == 3);
  CHECK(r->x == Approx(118.1102).margin(1e-4));

  // No ResolutionUnit entry: the TIFF default is inches, so inches are written.
  std::vector<std::uint8_t> bare = resolution_exif(96, 1, false);
  REQUIRE(mv::codec::exif_set_resolution(bare, 300.0, true));
  r = mv::codec::exif_resolution(bare);
  REQUIRE(r);
  CHECK(r->unit == 2);
  CHECK(r->x == 300.0);

  // ResolutionUnit 1 (no absolute unit) states nothing; a damaged block is
  // absent; a block without the tags is left alone and reports success.
  CHECK_FALSE(mv::codec::exif_resolution(resolution_exif(72, 1, true, 1)));
  CHECK_FALSE(mv::codec::exif_resolution(resolution_exif(72, 0, true)));
  std::vector<std::uint8_t> junk = {'I', 'I', 42, 0, 0xFF, 0xFF, 0, 0};
  CHECK_FALSE(mv::codec::exif_resolution(junk));
  CHECK_FALSE(mv::codec::exif_set_resolution(junk, 300.0, false));
  std::vector<std::uint8_t> empty_ifd = {'I', 'I', 42, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  CHECK(mv::codec::exif_set_resolution(empty_ifd, 300.0, false));
}

TEST_CASE("an encoded JPEG's EXIF and JFIF agree when both are written", "[edit][adjust_size][encode]") {
  // What export does: patch the carried EXIF to the stated resolution, then
  // encode with it. Both readers then see 240 ppi.
  edit::metadata_blobs meta;
  meta.exif = resolution_exif(72, 1, true);
  REQUIRE(mv::codec::exif_set_resolution(meta.exif, 240.0, false));
  const auto jpeg = encode_with(pattern(16, 16), edit::image_format::jpeg,
                                {240.0, resolution_unit::per_inch}, meta);
  const auto exif = mv::codec::find_jpeg_exif(jpeg);
  REQUIRE(exif);
  const auto r = mv::codec::exif_resolution(std::span(jpeg).subspan(exif->offset, exif->size));
  REQUIRE(r);
  CHECK(r->x == 240.0);
  CHECK(libjpeg_jfif(jpeg).x == 240);
}
