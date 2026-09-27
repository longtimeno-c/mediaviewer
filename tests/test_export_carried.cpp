// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 10: a re-encoded export of a HEIC / TIFF / RAW / WebP carries its EXIF and
// XMP (meta::read_carried, wired by shell::run_export) — the photograph's
// metadata, never the source container's own image-structure tags. Fixtures
// are built in the test with libwebp / libtiff and Exiv2.
#include <catch2/catch_test_macros.hpp>

#include <exiv2/exiv2.hpp>
#include <webp/encode.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "codec/exif.h"
#include "edit/lossless_jpeg.h"
#include "fixtures_tiff_ico.h"
#include "meta/meta.h"
#include "shell/edit_session.h"

namespace {

namespace fs = std::filesystem;

std::vector<std::uint8_t> rgb_pattern(std::uint32_t w, std::uint32_t h, int channels) {
  std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * static_cast<std::size_t>(channels));
  for (std::size_t i = 0; i < px.size(); ++i) px[i] = static_cast<std::uint8_t>((i * 37) & 0xFF);
  return px;
}

std::vector<std::uint8_t> webp_bytes(std::uint32_t w, std::uint32_t h) {
  const auto rgb = rgb_pattern(w, h, 3);
  std::uint8_t* out = nullptr;
  const std::size_t n = WebPEncodeRGB(rgb.data(), static_cast<int>(w), static_cast<int>(h),
                                      static_cast<int>(w * 3), 90.0f, &out);
  REQUIRE(n > 0);
  std::vector<std::uint8_t> bytes(out, out + n);
  WebPFree(out);
  return bytes;
}

std::vector<std::uint8_t> tiff_bytes(std::uint32_t w, std::uint32_t h) {
  fixtures::tiff_spec s;
  s.width = w;
  s.height = h;
  const auto rgb = rgb_pattern(w, h, 3);
  auto bytes = fixtures::tiff_write(s, rgb.data());
  REQUIRE_FALSE(bytes.empty());
  return bytes;
}

// The camera's metadata, written into `bytes` by Exiv2 the way a camera or a
// cataloguing app would leave it.
std::vector<std::uint8_t> with_metadata(const std::vector<std::uint8_t>& bytes, int orientation) {
  auto image = Exiv2::ImageFactory::open(bytes.data(), bytes.size());
  REQUIRE(image);
  image->readMetadata();
  Exiv2::ExifData& exif = image->exifData();
  exif["Exif.Image.Make"] = "Canon";
  exif["Exif.Image.Model"] = "Canon EOS R5";
  exif["Exif.Image.Orientation"] = static_cast<std::uint16_t>(orientation);
  exif["Exif.Photo.DateTimeOriginal"] = "2024:05:01 14:03:22";
  exif["Exif.Photo.ExposureTime"] = Exiv2::Rational(1, 250);
  exif["Exif.GPSInfo.GPSLatitudeRef"] = "N";
  exif["Exif.GPSInfo.GPSLatitude"] = "48/1 51/1 29/1";
  image->xmpData()["Xmp.dc.title"] = "Harbour at dusk";
  image->writeMetadata();
  Exiv2::BasicIo& io = image->io();
  io.open();
  Exiv2::DataBuf buf = io.read(io.size());
  io.close();
  return {buf.c_data(), buf.c_data() + buf.size()};
}

Exiv2::ExifData decode_exif(const std::vector<std::uint8_t>& tiff) {
  Exiv2::ExifData exif;
  REQUIRE_FALSE(tiff.empty());
  (void)Exiv2::ExifParser::decode(exif, tiff.data(), tiff.size());
  return exif;
}

bool has(const Exiv2::ExifData& e, const char* key) {
  return e.findKey(Exiv2::ExifKey(key)) != e.end();
}

struct temp_dir {
  fs::path path;
  temp_dir() {
    static int counter = 0;
    path = fs::temp_directory_path() /
           ("mv_export_carried_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
            std::to_string(++counter));
    fs::create_directories(path);
  }
  ~temp_dir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  fs::path write(const char* name, const std::vector<std::uint8_t>& bytes) const {
    const fs::path p = path / name;
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return p;
  }
};

std::vector<std::uint8_t> read_file(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("a WebP's EXIF and XMP are carried, upright", "[meta][export]") {
  const auto src = with_metadata(webp_bytes(32, 24), 6);
  const auto carried = mv::meta::read_carried(src);
  auto exif = decode_exif(carried.exif);
  CHECK(exif["Exif.Image.Make"].toString() == "Canon");
  CHECK(exif["Exif.Photo.DateTimeOriginal"].toString() == "2024:05:01 14:03:22");
  CHECK(exif["Exif.Image.Orientation"].toInt64() == 1);
  CHECK(has(exif, "Exif.GPSInfo.GPSLatitude"));
  const std::string xmp(carried.xmp.begin(), carried.xmp.end());
  CHECK(xmp.find("Harbour at dusk") != std::string::npos);
}

TEST_CASE("a TIFF's own pixel structure is not carried", "[meta][export]") {
  const auto src = with_metadata(tiff_bytes(32, 24), 1);
  // The source really has strips: that is what must not reach the JPEG.
  {
    auto image = Exiv2::ImageFactory::open(src.data(), src.size());
    image->readMetadata();
    REQUIRE(has(image->exifData(), "Exif.Image.StripOffsets"));
  }
  const auto carried = mv::meta::read_carried(src);
  auto exif = decode_exif(carried.exif);
  CHECK(exif["Exif.Image.Model"].toString() == "Canon EOS R5");
  for (const char* key : {"Exif.Image.StripOffsets", "Exif.Image.StripByteCounts",
                          "Exif.Image.RowsPerStrip", "Exif.Image.ImageWidth",
                          "Exif.Image.BitsPerSample", "Exif.Image.Compression",
                          "Exif.Image.PhotometricInterpretation"}) {
    INFO(key);
    CHECK_FALSE(has(exif, key));
  }
}

TEST_CASE("nothing to carry is empty, never an error", "[meta][export]") {
  const auto bare = mv::meta::read_carried(webp_bytes(8, 8));
  CHECK(bare.exif.empty());
  CHECK(bare.xmp.empty());
  const std::vector<std::uint8_t> junk(64, 0x5A);
  const auto none = mv::meta::read_carried(junk);
  CHECK(none.exif.empty());
}

TEST_CASE("exporting a WebP writes its metadata into the JPEG", "[shell][export]") {
  temp_dir dir;
  const fs::path src = dir.write("IMG_0100.webp", with_metadata(webp_bytes(64, 48), 1));

  mv::edit::geometry g;
  g.crop = {0.25f, 0.25f, 0.5f, 0.5f};
  auto out = mv::shell::run_export(src.string(), g, {});
  REQUIRE(out);
  CHECK(fs::path(*out).filename() == "IMG_0100-edit.jpg");
  const auto jpeg = read_file(*out);
  const auto app1 = mv::codec::find_jpeg_exif(jpeg);
  REQUIRE(app1);
  const std::vector<std::uint8_t> tiff(jpeg.begin() + static_cast<std::ptrdiff_t>(app1->offset),
                                       jpeg.begin() + static_cast<std::ptrdiff_t>(app1->offset + app1->size));
  auto exif = decode_exif(tiff);
  CHECK(exif["Exif.Image.Make"].toString() == "Canon");
  CHECK(exif["Exif.Image.Orientation"].toInt64() == 1);
  CHECK(mv::codec::find_jpeg_xmp(jpeg));
  auto layout = mv::edit::read_layout(jpeg);
  REQUIRE(layout);
  CHECK(layout->width == 32);
  CHECK(layout->height == 24);

  // The policy applies to carried metadata as it does to a JPEG's own.
  mv::edit::export_options opt;
  opt.policy = mv::edit::metadata_policy::minus_gps;
  auto no_gps = mv::shell::run_export(src.string(), g, opt);
  REQUIRE(no_gps);
  const auto jpeg2 = read_file(*no_gps);
  const auto app1b = mv::codec::find_jpeg_exif(jpeg2);
  REQUIRE(app1b);
  CHECK_FALSE(mv::codec::exif_has_gps(std::span<const std::uint8_t>(
      jpeg2.data() + app1b->offset, app1b->size)));
  opt.policy = mv::edit::metadata_policy::none;
  auto none = mv::shell::run_export(src.string(), g, opt);
  REQUIRE(none);
  CHECK_FALSE(mv::codec::find_jpeg_exif(read_file(*none)));
}

TEST_CASE("a HEIC's EXIF is carried, turned upright", "[meta][export][heif]") {
  // libheif shows this frame through its irot; the export is written upright,
  // so the Orientation 6 it carries in EXIF must arrive as 1.
  const fs::path here = fs::path(__FILE__).parent_path() / "data" / "heif" / "iphone_like.heic";
  if (!fs::exists(here)) SKIP("tests/data/heif/iphone_like.heic not present");
  const auto bytes = read_file(here);
  {
    auto image = Exiv2::ImageFactory::open(bytes.data(), bytes.size());
    image->readMetadata();
    REQUIRE(image->exifData()["Exif.Image.Orientation"].toInt64() == 6);
  }
  const auto carried = mv::meta::read_carried(bytes);
  auto exif = decode_exif(carried.exif);
  CHECK(exif["Exif.Image.Orientation"].toInt64() == 1);
}
