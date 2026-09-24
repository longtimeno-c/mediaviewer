// SPDX-License-Identifier: GPL-2.0-or-later
// PR 10 verify (plan/10): crop + export a JPEG — on-disk dimensions and EXIF
// orientation match; reset returns the original pixels exactly; lossless
// rotate produces a file with no recompression; keyboard-only rotate of a
// JPEG in the viewer writes that file (the core half: rotate_in_viewer +
// io::replace_atomic). Fixtures are built in the test — no corpus.
#include <catch2/catch_test_macros.hpp>


#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "codec/decode.h"
#include "codec/exif.h"
#include "codec/orient.h"
#include "codec/orientation.h"
#include "edit/edit_stack.h"
#include "edit/encode.h"
#include "edit/export.h"
#include "edit/geometry.h"
#include "edit/lossless_jpeg.h"
#include "io/file.h"
#include "io/replace.h"

namespace {

namespace fs = std::filesystem;
using mv::codec::d4;
using mv::codec::raster;
namespace edit = mv::edit;

// Distinct, smooth-ish content: every pixel different, no flat areas, so a
// wrong mapping cannot hide.
raster pattern(std::uint32_t w, std::uint32_t h) {
  raster r;
  r.width = w;
  r.height = h;
  r.format = mv::codec::format_family::jpeg;
  r.rgba.resize(static_cast<std::size_t>(w) * h * 4);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      std::uint8_t* p = r.rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4;
      p[0] = static_cast<std::uint8_t>(x * 255 / (w > 1 ? w - 1 : 1));
      p[1] = static_cast<std::uint8_t>(y * 255 / (h > 1 ? h - 1 : 1));
      p[2] = static_cast<std::uint8_t>((x * 7 + y * 13) & 0xFF);
      p[3] = 255;
    }
  }
  return r;
}

std::uint32_t pixel(const raster& r, std::uint32_t x, std::uint32_t y) {
  std::uint32_t v;
  std::memcpy(&v, r.rgba.data() + (static_cast<std::size_t>(y) * r.width + x) * 4, 4);
  return v;
}

// Little-endian TIFF writer for EXIF fixtures.
struct tiff_builder {
  std::vector<std::uint8_t> b;
  void u16(std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
  }
  void u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
  }
  void entry(std::uint16_t tag, std::uint16_t type, std::uint32_t count, std::uint32_t value) {
    u16(tag); u16(type); u32(count); u32(value);
  }
};

// IFD0: Orientation, ExifIFD, GPS, next → IFD1. Exif IFD: PixelX/PixelY
// LONG. GPS: LatitudeRef "N", Latitude 3 RATIONAL (out of line). IFD1: one
// entry (a stand-in for the thumbnail).
std::vector<std::uint8_t> camera_exif(int orientation, std::uint32_t w, std::uint32_t h) {
  tiff_builder t;
  t.b = {'I', 'I', 42, 0};
  t.u32(8);
  // IFD0 at 8: 3 entries = 2 + 36 + 4 = 42 bytes → ends at 50.
  const std::uint32_t exif_ifd = 50;         // 2 entries: 2 + 24 + 4 = 30 → 80
  const std::uint32_t gps_ifd = 80;          // 2 entries → 110
  const std::uint32_t gps_lat = 110;         // 24 bytes → 134
  const std::uint32_t ifd1 = 134;            // 1 entry: 18 bytes → 152
  t.u16(3);
  t.entry(0x0112, 3, 1, static_cast<std::uint32_t>(orientation));
  t.entry(0x8769, 4, 1, exif_ifd);
  t.entry(0x8825, 4, 1, gps_ifd);
  t.u32(ifd1);
  t.u16(2);
  t.entry(0xA002, 4, 1, w);
  t.entry(0xA003, 4, 1, h);
  t.u32(0);
  t.u16(2);
  t.entry(0x0001, 2, 2, 'N');
  t.entry(0x0002, 5, 3, gps_lat);
  t.u32(0);
  for (std::uint32_t v : {48u, 1u, 51u, 1u, 29u, 1u}) t.u32(v);
  t.u16(1);
  t.entry(0x0103, 3, 1, 6);
  t.u32(0);
  REQUIRE(t.b.size() == 152);
  return t.b;
}

std::vector<std::uint8_t> make_jpeg(const raster& r, const edit::metadata_blobs& meta = {},
                                    edit::chroma sub = edit::chroma::s420, int quality = 95) {
  edit::encode_options opt;
  opt.format = edit::image_format::jpeg;
  opt.quality = quality;
  opt.subsampling = sub;
  auto bytes = edit::encode(r, opt, meta);
  REQUIRE(bytes);
  return std::move(bytes).value();
}

raster decode_plain(std::span<const std::uint8_t> jpeg) {
  auto r = mv::codec::decode_jpeg(jpeg);
  REQUIRE(r);
  return std::move(r).value();
}

int max_diff(const raster& a, const raster& b) {
  REQUIRE(a.width == b.width);
  REQUIRE(a.height == b.height);
  int m = 0;
  for (std::size_t i = 0; i < a.rgba.size(); ++i) {
    m = std::max(m, std::abs(int(a.rgba[i]) - int(b.rgba[i])));
  }
  return m;
}

std::vector<std::uint8_t> tiff_of(std::span<const std::uint8_t> jpeg) {
  const auto r = mv::codec::find_jpeg_exif(jpeg);
  REQUIRE(r);
  return {jpeg.begin() + static_cast<std::ptrdiff_t>(r->offset),
          jpeg.begin() + static_cast<std::ptrdiff_t>(r->offset + r->size)};
}

// Exif.Photo.PixelXDimension / Y straight from the fixture layout.
std::pair<std::uint32_t, std::uint32_t> pixel_dims(const std::vector<std::uint8_t>& tiff) {
  auto u32 = [&](std::size_t o) {
    return std::uint32_t(tiff[o]) | std::uint32_t(tiff[o + 1]) << 8 | std::uint32_t(tiff[o + 2]) << 16 |
           std::uint32_t(tiff[o + 3]) << 24;
  };
  return {u32(50 + 2 + 8), u32(50 + 2 + 12 + 8)};
}

struct temp_dir {
  fs::path path;
  temp_dir() {
    static int counter = 0;
    path = fs::temp_directory_path() /
           ("mv_edit_test_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) +
            "_" + std::to_string(++counter));
    fs::create_directories(path);
  }
  ~temp_dir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

std::vector<std::uint8_t> read_file(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

}  // namespace

// ---------------------------------------------------------------------------
// Orientation algebra and the display path
// ---------------------------------------------------------------------------

TEST_CASE("the eight EXIF orientations are a group", "[edit][orient]") {
  for (int o = 1; o <= 8; ++o) {
    CHECK(mv::codec::to_exif(mv::codec::from_exif(o)) == o);
    const d4 g = mv::codec::from_exif(o);
    CHECK(mv::codec::compose(g, mv::codec::inverse(g)).identity());
  }
  CHECK(mv::codec::from_exif(6) == mv::codec::kRotateCw);
  CHECK(mv::codec::from_exif(8) == mv::codec::kRotateCcw);
  d4 g{};
  for (int i = 0; i < 4; ++i) g = mv::codec::compose(g, mv::codec::kRotateCw);
  CHECK(g.identity());
  CHECK(mv::codec::compose(mv::codec::kFlipH, mv::codec::kFlipH).identity());
  // Rotate 180 is two quarter turns, and is both flips.
  CHECK(mv::codec::compose(mv::codec::kRotateCw, mv::codec::kRotateCw) == mv::codec::from_exif(3));
  CHECK(mv::codec::compose(mv::codec::kFlipH, mv::codec::kFlipV) == mv::codec::from_exif(3));
}

TEST_CASE("orientation 6 shows the stored frame turned clockwise", "[edit][orient]") {
  raster r = pattern(3, 2);
  const raster stored = r;
  REQUIRE(mv::codec::apply_orientation(r, mv::codec::from_exif(6)));
  REQUIRE(r.width == 2);
  REQUIRE(r.height == 3);
  // Turned clockwise: the stored bottom-left lands top-left, the stored
  // top-left lands top-right.
  CHECK(pixel(r, 0, 0) == pixel(stored, 0, 1));
  CHECK(pixel(r, 1, 0) == pixel(stored, 0, 0));
  CHECK(pixel(r, 1, 2) == pixel(stored, 2, 0));
  CHECK(pixel(r, 0, 2) == pixel(stored, 2, 1));
}

TEST_CASE("apply_orientation agrees with its inverse for every orientation", "[edit][orient]") {
  const raster stored = pattern(5, 3);
  for (int o = 1; o <= 8; ++o) {
    raster r = stored;
    REQUIRE(mv::codec::apply_orientation(r, mv::codec::from_exif(o)));
    REQUIRE(mv::codec::apply_orientation(r, mv::codec::inverse(mv::codec::from_exif(o))));
    CHECK(r.width == stored.width);
    CHECK(r.rgba == stored.rgba);
  }
}

TEST_CASE("a JPEG is displayed through its EXIF orientation", "[edit][orient]") {
  edit::metadata_blobs meta;
  meta.exif = camera_exif(6, 32, 16);
  const auto jpeg = make_jpeg(pattern(32, 16), meta);
  CHECK(mv::codec::jpeg_orientation(jpeg) == 6);
  auto shown = mv::codec::decode(jpeg);
  REQUIRE(shown);
  CHECK(shown->width == 16);
  CHECK(shown->height == 32);
  // The raw decode (RAW previews use it) is still the stored frame.
  const raster stored = decode_plain(jpeg);
  CHECK(stored.width == 32);
}

// ---------------------------------------------------------------------------
// EXIF patching
// ---------------------------------------------------------------------------

TEST_CASE("EXIF edits happen in place and never grow the block", "[edit][exif]") {
  auto tiff = camera_exif(6, 6000, 4000);
  const std::size_t size = tiff.size();
  CHECK(mv::codec::exif_orientation(tiff) == 6);
  REQUIRE(mv::codec::exif_set_orientation(tiff, 1));
  CHECK(mv::codec::exif_orientation(tiff) == 1);
  REQUIRE(mv::codec::exif_set_pixel_dimensions(tiff, 4000, 6000));
  CHECK(pixel_dims(tiff) == std::pair<std::uint32_t, std::uint32_t>{4000, 6000});

  CHECK(mv::codec::exif_has_gps(tiff));
  REQUIRE(mv::codec::exif_strip_gps(tiff));
  CHECK_FALSE(mv::codec::exif_has_gps(tiff));
  // No coordinate survives in the bytes, and IFD0 still walks.
  for (std::size_t i = 110; i < 134; ++i) CHECK(tiff[i] == 0);
  CHECK(mv::codec::exif_orientation(tiff) == 1);
  CHECK(pixel_dims(tiff) == std::pair<std::uint32_t, std::uint32_t>{4000, 6000});
  CHECK(tiff.size() == size);

  REQUIRE(mv::codec::exif_drop_thumbnail(tiff));
  CHECK(mv::codec::exif_orientation(tiff) == 1);
}

TEST_CASE("damaged EXIF reads as absent, not as a crash", "[edit][exif]") {
  auto tiff = camera_exif(6, 10, 10);
  for (std::size_t cut = 0; cut < tiff.size(); ++cut) {
    std::vector<std::uint8_t> t(tiff.begin(), tiff.begin() + static_cast<std::ptrdiff_t>(cut));
    (void)mv::codec::exif_orientation(t);
    (void)mv::codec::exif_set_pixel_dimensions(t, 1, 1);
    (void)mv::codec::exif_strip_gps(t);
    (void)mv::codec::exif_drop_thumbnail(t);
  }
  std::vector<std::uint8_t> junk(64, 0xFF);
  CHECK(mv::codec::exif_orientation(junk) == 0);
}

TEST_CASE("XMP orientation is rewritten in both spellings", "[edit][exif]") {
  const std::string attr = R"(<rdf:Description tiff:Orientation="6" exif:GPSLatitude="1"/>)";
  std::vector<std::uint8_t> a(attr.begin(), attr.end());
  CHECK(mv::codec::xmp_set_orientation(a, 1) == 1);
  CHECK(std::string(a.begin(), a.end()).find("tiff:Orientation=\"1\"") != std::string::npos);
  CHECK(mv::codec::xmp_has_gps(a));
  const std::string elem = "<tiff:Orientation>8</tiff:Orientation>";
  std::vector<std::uint8_t> e(elem.begin(), elem.end());
  CHECK(mv::codec::xmp_set_orientation(e, 1) == 1);
  CHECK(std::string(e.begin(), e.end()) == "<tiff:Orientation>1</tiff:Orientation>");
}

// ---------------------------------------------------------------------------
// The stack and its geometry
// ---------------------------------------------------------------------------

TEST_CASE("reset and undo return the original placement exactly", "[edit][stack]") {
  edit::edit_stack s;
  CHECK(edit::fold(s).identity());
  s.push({edit::op_kind::rotate_cw});
  s.push({edit::op_kind::flip_h});
  edit::op crop{edit::op_kind::crop};
  crop.crop = {0.1f, 0.2f, 0.5f, 0.5f};
  s.push(crop);
  CHECK_FALSE(edit::fold(s).identity());
  REQUIRE(s.undo());
  CHECK(s.ops.size() == 2);
  s.reset();
  const auto p = edit::place(edit::fold(s), {6000, 4000});
  CHECK(p.map.identity());
  CHECK(p.output == edit::size2{6000, 4000});
  CHECK(p.exact_copy);
}

TEST_CASE("four quarter turns and two flips are the identity", "[edit][stack]") {
  edit::edit_stack s;
  for (int i = 0; i < 4; ++i) s.push({edit::op_kind::rotate_cw});
  s.push({edit::op_kind::flip_v});
  s.push({edit::op_kind::flip_v});
  CHECK(edit::fold(s).identity());
}

TEST_CASE("a rotate swaps the frame and maps corners clockwise", "[edit][stack]") {
  edit::geometry g;
  g.orient = mv::codec::kRotateCw;
  const auto p = edit::place(g, {600, 400});
  CHECK(p.oriented == edit::size2{400, 600});
  CHECK(p.output == edit::size2{400, 600});
  // Output top-left shows the source bottom-left (a clockwise turn).
  CHECK(p.map.m[2] == 0.0f);
  CHECK(p.map.m[5] == 1.0f);
  // Output top-right shows the source top-left.
  CHECK(p.map.m[0] + p.map.m[2] == 0.0f);
  CHECK(p.map.m[3] + p.map.m[5] == 0.0f);
}

TEST_CASE("a crop set before a rotate turns with the frame", "[edit][stack]") {
  edit::edit_stack s;
  edit::op crop{edit::op_kind::crop};
  crop.crop = {0.0f, 0.0f, 0.5f, 0.25f};  // top-left strip
  s.push(crop);
  s.push({edit::op_kind::rotate_cw});
  const auto g = edit::fold(s);
  // After a clockwise turn the top-left strip is the top-right column.
  CHECK(std::abs(g.crop.x - 0.75f) < 1e-6f);
  CHECK(std::abs(g.crop.y - 0.0f) < 1e-6f);
  CHECK(std::abs(g.crop.w - 0.25f) < 1e-6f);
  CHECK(std::abs(g.crop.h - 0.5f) < 1e-6f);
}

TEST_CASE("straighten never exports a transparent corner", "[edit][stack]") {
  for (float deg : {-45.0f, -10.0f, -0.5f, 0.5f, 3.0f, 30.0f, 45.0f}) {
    edit::geometry g;
    g.straighten = deg;
    const auto p = edit::place(g, {600, 400});
    CHECK(p.cropped.w < 600);
    CHECK_FALSE(p.exact_copy);
    // Every output corner maps inside the source.
    for (double u : {0.0, 1.0}) {
      for (double v : {0.0, 1.0}) {
        const double su = p.map.m[0] * u + p.map.m[1] * v + p.map.m[2];
        const double sv = p.map.m[3] * u + p.map.m[4] * v + p.map.m[5];
        CHECK(su >= -1e-4);
        CHECK(su <= 1 + 1e-4);
        CHECK(sv >= -1e-4);
        CHECK(sv <= 1 + 1e-4);
      }
    }
    // Aspect kept.
    CHECK(std::abs(double(p.cropped.w) / p.cropped.h - 1.5) < 0.02);
  }
  // A flip reverses the sense of an angle set before it.
  edit::edit_stack s;
  edit::op st{edit::op_kind::straighten};
  st.degrees = 4.0f;
  s.push(st);
  s.push({edit::op_kind::flip_h});
  CHECK(edit::fold(s).straighten == -4.0f);
}

TEST_CASE("resize modes size the output", "[edit][stack]") {
  edit::geometry g;
  g.resize = {edit::resize_mode::long_edge, 2048, 0, 100};
  CHECK(edit::place(g, {6000, 4000}).output == edit::size2{2048, 1365});
  g.resize = {edit::resize_mode::exact, 800, 800, 100};
  CHECK(edit::place(g, {6000, 4000}).output == edit::size2{800, 800});
  g.resize = {edit::resize_mode::percent, 0, 0, 50};
  CHECK(edit::place(g, {6000, 4000}).output == edit::size2{3000, 2000});
}

TEST_CASE("the full-resolution render is the source pixels rearranged", "[edit][render]") {
  const raster src = pattern(7, 5);
  for (int o = 1; o <= 8; ++o) {
    edit::geometry g;
    g.orient = mv::codec::from_exif(o);
    const auto p = edit::place(g, {src.width, src.height});
    auto out = edit::render(src, p);
    REQUIRE(out);
    raster expect = src;
    REQUIRE(mv::codec::apply_orientation(expect, g.orient));
    CHECK(out->rgba == expect.rgba);
  }
  // Crop picks exactly the named pixels.
  edit::geometry g;
  g.crop = {2.0f / 7, 1.0f / 5, 3.0f / 7, 2.0f / 5};
  auto out = edit::render(src, edit::place(g, {7, 5}));
  REQUIRE(out);
  REQUIRE(out->width == 3);
  REQUIRE(out->height == 2);
  CHECK(pixel(*out, 0, 0) == pixel(src, 2, 1));
  CHECK(pixel(*out, 2, 1) == pixel(src, 4, 2));
}

TEST_CASE("a straightened render resamples without leaking the border", "[edit][render]") {
  raster flat = pattern(64, 48);
  for (std::size_t i = 0; i < flat.rgba.size(); i += 4) {
    flat.rgba[i] = 200; flat.rgba[i + 1] = 100; flat.rgba[i + 2] = 50;
  }
  edit::geometry g;
  g.straighten = 7.0f;
  auto out = edit::render(flat, edit::place(g, {64, 48}));
  REQUIRE(out);
  for (std::size_t i = 0; i < out->rgba.size(); i += 4) {
    CHECK(std::abs(int(out->rgba[i]) - 200) <= 1);
    CHECK(out->rgba[i + 3] == 255);
  }
}

// ---------------------------------------------------------------------------
// Lossless JPEG
// ---------------------------------------------------------------------------

TEST_CASE("lossless rotate is perfect or refused", "[edit][lossless]") {
  const auto aligned = make_jpeg(pattern(64, 32));
  auto l = edit::read_layout(aligned);
  REQUIRE(l);
  CHECK(l->mcu_w == 16);
  CHECK(l->mcu_h == 16);
  edit::lossless_request req;
  for (int o = 1; o <= 8; ++o) {
    req.transform = mv::codec::from_exif(o);
    CHECK(edit::lossless_possible(*l, req));
  }
  const auto odd = make_jpeg(pattern(20, 12));
  auto lo = edit::read_layout(odd);
  REQUIRE(lo);
  req.transform = {};
  CHECK(edit::lossless_possible(*lo, req));
  req.transform = mv::codec::kRotateCw;  // reverses the stored y: 12 is not whole MCUs
  CHECK_FALSE(edit::lossless_possible(*lo, req));
  req.transform = mv::codec::from_exif(5);  // a pure transpose reverses nothing
  CHECK(edit::lossless_possible(*lo, req));
  auto refused = edit::transform(odd, {mv::codec::kRotateCw});
  REQUIRE_FALSE(refused);
  CHECK(refused.error() == mv::status::unsupported_format);
}

TEST_CASE("lossless rotate does not recompress", "[edit][lossless]") {
  for (auto sub : {edit::chroma::s420, edit::chroma::s422, edit::chroma::s444}) {
    const auto jpeg = make_jpeg(pattern(64, 48), {}, sub, 80);
    const raster original = decode_plain(jpeg);

    // Four quarter turns bring back the same coefficients, so the same
    // pixels, bit for bit. Any requantisation would drift.
    std::vector<std::uint8_t> bytes = jpeg;
    for (int i = 0; i < 4; ++i) {
      auto r = edit::transform(bytes, {mv::codec::kRotateCw});
      REQUIRE(r);
      bytes = std::move(r).value();
    }
    CHECK(decode_plain(bytes).rgba == original.rgba);

    // One turn is the decoded image turned (up to the IDCT's and chroma
    // upsampling's rounding, which run in the other order).
    auto once = edit::transform(jpeg, {mv::codec::kRotateCw});
    REQUIRE(once);
    raster turned = original;
    REQUIRE(mv::codec::apply_orientation(turned, mv::codec::kRotateCw));
    CHECK(max_diff(decode_plain(*once), turned) <= 3);

    // Flips twice: exact.
    auto f1 = edit::transform(jpeg, {mv::codec::kFlipH});
    REQUIRE(f1);
    auto f2 = edit::transform(*f1, {mv::codec::kFlipH});
    REQUIRE(f2);
    CHECK(decode_plain(*f2).rgba == original.rgba);
  }
}

TEST_CASE("an MCU-aligned crop is lossless and exact", "[edit][lossless]") {
  const auto jpeg = make_jpeg(pattern(64, 48));
  const raster original = decode_plain(jpeg);
  edit::lossless_request req;
  req.crop_x = 16;
  req.crop_y = 16;
  req.crop_w = 37;  // right / bottom edges are free
  req.crop_h = 20;
  auto l = edit::read_layout(jpeg);
  REQUIRE(l);
  REQUIRE(edit::lossless_possible(*l, req));
  auto out = edit::transform(jpeg, req);
  REQUIRE(out);
  const raster c = decode_plain(*out);
  REQUIRE(c.width == 37);
  REQUIRE(c.height == 20);
  // Interior pixels are unchanged; chroma at the new edges may differ by the
  // upsampler's reach, so compare away from them.
  int m = 0;
  for (std::uint32_t y = 2; y < 18; ++y) {
    for (std::uint32_t x = 2; x < 35; ++x) {
      for (int k = 0; k < 3; ++k) {
        const int a = c.rgba[(static_cast<std::size_t>(y) * 37 + x) * 4 + static_cast<std::size_t>(k)];
        const int b = original.rgba[(static_cast<std::size_t>(y + 16) * 64 + x + 16) * 4 + static_cast<std::size_t>(k)];
        m = std::max(m, std::abs(a - b));
      }
    }
  }
  CHECK(m <= 2);
  req.crop_x = 8;  // off the 16-px grid
  CHECK_FALSE(edit::lossless_possible(*l, req));
}

TEST_CASE("lossless rotate bakes the orientation and fixes the metadata", "[edit][lossless]") {
  edit::metadata_blobs meta;
  meta.exif = camera_exif(6, 64, 32);
  const std::string xmp = R"(<x:xmpmeta><rdf:Description tiff:Orientation="6"/></x:xmpmeta>)";
  meta.xmp.assign(xmp.begin(), xmp.end());
  const auto jpeg = make_jpeg(pattern(64, 32), meta);
  auto shown = mv::codec::decode(jpeg);  // 32 x 64, upright
  REQUIRE(shown);

  bool used_tag = true;
  auto out = edit::rotate_in_viewer(jpeg, mv::codec::kRotateCw, &used_tag);
  REQUIRE(out);
  CHECK_FALSE(used_tag);
  // Stored upright now: 6 then a clockwise turn is a half turn of the stored
  // frame, so width/height stay 64 x 32 and the tag is 1.
  auto l = edit::read_layout(*out);
  REQUIRE(l);
  CHECK(l->width == 64);
  CHECK(l->height == 32);
  CHECK(l->orientation == 1);
  const auto tiff = tiff_of(*out);
  CHECK(pixel_dims(tiff) == std::pair<std::uint32_t, std::uint32_t>{64, 32});
  CHECK(mv::codec::exif_has_gps(tiff));  // viewer rotate keeps everything
  const auto x = mv::codec::find_jpeg_xmp(*out);
  REQUIRE(x);
  const std::string out_xmp(out->begin() + static_cast<std::ptrdiff_t>(x->offset),
                            out->begin() + static_cast<std::ptrdiff_t>(x->offset + x->size));
  CHECK(out_xmp.find("tiff:Orientation=\"1\"") != std::string::npos);

  // What is displayed is what was displayed, turned clockwise.
  raster expect = std::move(shown).value();
  REQUIRE(mv::codec::apply_orientation(expect, mv::codec::kRotateCw));
  auto now = mv::codec::decode(*out);
  REQUIRE(now);
  CHECK(max_diff(*now, expect) <= 3);
}

TEST_CASE("an odd-sized JPEG rotates by its tag, pixels untouched", "[edit][lossless]") {
  // No EXIF at all: a minimal APP1 is inserted.
  const auto plain = make_jpeg(pattern(20, 12));
  bool used_tag = false;
  auto a = edit::rotate_in_viewer(plain, mv::codec::kRotateCw, &used_tag);
  REQUIRE(a);
  CHECK(used_tag);
  CHECK(mv::codec::jpeg_orientation(*a) == 6);
  CHECK(decode_plain(*a).rgba == decode_plain(plain).rgba);  // same scan data

  // EXIF with an Orientation: patched in place, same length.
  edit::metadata_blobs meta;
  meta.exif = camera_exif(6, 20, 12);
  const auto tagged = make_jpeg(pattern(20, 12), meta);
  auto b = edit::rotate_in_viewer(tagged, mv::codec::kRotateCw, &used_tag);
  REQUIRE(b);
  CHECK(used_tag);
  CHECK(b->size() == tagged.size());
  CHECK(mv::codec::jpeg_orientation(*b) == 3);
  auto shown = mv::codec::decode(*b);
  REQUIRE(shown);
  CHECK(shown->width == 20);  // 180°: frame not swapped
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

TEST_CASE("crop + export a JPEG: dimensions and orientation match on disk", "[edit][export]") {
  edit::metadata_blobs meta;
  meta.exif = camera_exif(6, 96, 64);
  const auto src = make_jpeg(pattern(96, 64), meta);  // displayed 64 x 96

  edit::geometry g;
  g.crop = {0.1f, 0.2f, 0.5f, 0.25f};  // not on the MCU grid → re-encode
  edit::export_options opt;
  auto r = edit::export_image(src, g, opt);
  REQUIRE(r);
  CHECK_FALSE(r->lossless);
  CHECK(r->width == 32);
  CHECK(r->height == 24);

  temp_dir dir;
  const fs::path out = dir.path / edit::export_file_name("IMG_0001.JPG", edit::image_format::jpeg);
  CHECK(out.filename() == "IMG_0001-edit.jpg");
  REQUIRE(mv::io::write_new(out.string(), r->bytes));
  const auto disk = read_file(out);
  auto l = edit::read_layout(disk);
  REQUIRE(l);
  CHECK(l->width == 32);
  CHECK(l->height == 24);
  CHECK(l->orientation == 1);
  CHECK(pixel_dims(tiff_of(disk)) == std::pair<std::uint32_t, std::uint32_t>{32, 24});
  // An export never overwrites, the export included.
  CHECK_FALSE(mv::io::write_new(out.string(), r->bytes));
}

TEST_CASE("an aligned crop of an upright JPEG exports losslessly", "[edit][export]") {
  const auto src = make_jpeg(pattern(96, 64));
  edit::geometry g;
  g.orient = mv::codec::kRotateCcw;  // displayed 64 x 96
  // The crop is in the rotated frame, on its 16-px grid.
  g.crop = {16.0f / 64, 16.0f / 96, 32.0f / 64, 48.0f / 96};
  auto r = edit::export_image(src, g, {});
  REQUIRE(r);
  CHECK(r->lossless);
  CHECK(r->width == 32);
  CHECK(r->height == 48);
  auto l = edit::read_layout(r->bytes);
  REQUIRE(l);
  CHECK(l->width == 32);
  CHECK(l->height == 48);
}

TEST_CASE("an unedited export returns the original pixels exactly", "[edit][export]") {
  const raster r = pattern(40, 30);
  edit::encode_options png;
  png.format = edit::image_format::png;
  auto src = edit::encode(r, png, {});
  REQUIRE(src);
  edit::export_options opt;
  opt.encode.format = edit::image_format::png;
  auto out = edit::export_image(*src, edit::geometry{}, opt);
  REQUIRE(out);
  auto back = mv::codec::decode(out->bytes);
  REQUIRE(back);
  CHECK(back->rgba == r.rgba);

  // And a stack that was edited then reset is the same as never edited.
  edit::edit_stack s;
  s.push({edit::op_kind::rotate_ccw});
  s.push({edit::op_kind::flip_h});
  s.reset();
  auto again = edit::export_image(*src, edit::fold(s), opt);
  REQUIRE(again);
  CHECK(again->bytes == out->bytes);
}

TEST_CASE("the metadata policy is applied to the exported bytes", "[edit][export]") {
  edit::metadata_blobs meta;
  meta.exif = camera_exif(1, 32, 32);
  const std::string xmp = R"(<rdf:Description exif:GPSLatitude="48,51.5N"/>)";
  meta.xmp.assign(xmp.begin(), xmp.end());
  const auto src = make_jpeg(pattern(32, 32), meta);

  for (bool lossless : {true, false}) {
    edit::export_options opt;
    opt.prefer_lossless = lossless;
    opt.policy = edit::metadata_policy::all;
    auto all = edit::export_image(src, {}, opt);
    REQUIRE(all);
    CHECK(all->lossless == lossless);
    CHECK(mv::codec::exif_has_gps(tiff_of(all->bytes)));
    CHECK(mv::codec::find_jpeg_xmp(all->bytes));

    opt.policy = edit::metadata_policy::minus_gps;
    auto no_gps = edit::export_image(src, {}, opt);
    REQUIRE(no_gps);
    CHECK_FALSE(mv::codec::exif_has_gps(tiff_of(no_gps->bytes)));
    CHECK_FALSE(mv::codec::find_jpeg_xmp(no_gps->bytes));
    const std::string text(no_gps->bytes.begin(), no_gps->bytes.end());
    CHECK(text.find("GPSLatitude") == std::string::npos);

    opt.policy = edit::metadata_policy::none;
    auto none = edit::export_image(src, {}, opt);
    REQUIRE(none);
    CHECK_FALSE(mv::codec::find_jpeg_exif(none->bytes));
    CHECK_FALSE(mv::codec::find_jpeg_xmp(none->bytes));
  }
}

TEST_CASE("PNG export carries EXIF and XMP", "[edit][export]") {
  edit::metadata_blobs meta;
  meta.exif = camera_exif(1, 16, 16);
  const std::string xmp = "<x:xmpmeta/>";
  meta.xmp.assign(xmp.begin(), xmp.end());
  edit::encode_options png;
  png.format = edit::image_format::png;
  auto src = edit::encode(pattern(16, 16), png, meta);
  REQUIRE(src);
  const auto back = edit::source_metadata(*src, edit::metadata_policy::all);
  CHECK(back.exif == meta.exif);
  CHECK(back.xmp == meta.xmp);
}

// ---------------------------------------------------------------------------
// The viewer's in-place write
// ---------------------------------------------------------------------------

TEST_CASE("replace_atomic swaps the whole file or nothing", "[edit][io]") {
  temp_dir dir;
  const fs::path p = dir.path / "a.jpg";
  REQUIRE(mv::io::write_new(p.string(), std::vector<std::uint8_t>{1, 2, 3}));
  const std::vector<std::uint8_t> next{9, 8, 7, 6};
  REQUIRE(mv::io::replace_atomic(p.string(), next));
  CHECK(read_file(p) == next);
  // No temporary left behind.
  int files = 0;
  for (const auto& e : fs::directory_iterator(dir.path)) { (void)e; ++files; }
  CHECK(files == 1);
  CHECK_FALSE(mv::io::replace_atomic((dir.path / "missing.jpg").string(), next));
}

TEST_CASE("keyboard rotate of a JPEG writes that file", "[edit][io]") {
  temp_dir dir;
  const fs::path p = dir.path / "IMG_0002.jpg";
  const auto original = make_jpeg(pattern(64, 48));
  REQUIRE(mv::io::write_new(p.string(), original));
  auto bytes = mv::io::read_all(p.string());
  REQUIRE(bytes);
  auto rotated = edit::rotate_in_viewer(*bytes, mv::codec::kRotateCcw);
  REQUIRE(rotated);
  REQUIRE(mv::io::replace_atomic(p.string(), *rotated));
  auto l = edit::read_layout(read_file(p));
  REQUIRE(l);
  CHECK(l->width == 48);
  CHECK(l->height == 64);
  // `]` then `[` is the original image again, bit for bit.
  auto back = edit::rotate_in_viewer(read_file(p), mv::codec::kRotateCw);
  REQUIRE(back);
  CHECK(decode_plain(*back).rgba == decode_plain(original).rgba);
}

TEST_CASE("the export dialog's long edge sizes the output", "[edit][export]") {
  const auto src = make_jpeg(pattern(96, 64));
  edit::export_options opt;
  opt.long_edge = 48;
  auto r = edit::export_image(src, {}, opt);
  REQUIRE(r);
  CHECK_FALSE(r->lossless);  // a resize is pixels
  CHECK(r->width == 48);
  CHECK(r->height == 32);
}

TEST_CASE("an export is deterministic: same source and stack, same bytes", "[edit][export]") {
  // plan/10 PR 10 (both platforms): the same crop on the same JPEG exports
  // byte-identical on Windows and Mac. The op graph and the encoders are shared;
  // this pins the half a single machine can prove. Nothing time-, thread- or
  // address-dependent may reach the bytes.
  edit::metadata_blobs meta;
  meta.exif = camera_exif(6, 96, 64);
  const auto src = make_jpeg(pattern(96, 64), meta);
  for (const bool png : {false, true}) {
    edit::geometry g;
    g.crop = {0.1f, 0.2f, 0.5f, 0.25f};
    g.straighten = 3.0f;
    edit::export_options opt;
    opt.encode.format = png ? edit::image_format::png : edit::image_format::jpeg;
    auto a = edit::export_image(src, g, opt);
    auto b = edit::export_image(src, g, opt);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->bytes == b->bytes);
  }
  edit::geometry lossless;
  lossless.orient = mv::codec::kRotateCw;
  auto a = edit::export_image(src, lossless, {});
  auto b = edit::export_image(src, lossless, {});
  REQUIRE(a);
  REQUIRE(b);
  CHECK(a->lossless);
  CHECK(a->bytes == b->bytes);
}
