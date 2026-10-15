// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A file's coordinates from a bounded read (meta/location.h): what the host
// table's location_of / locations_in serve a search provider (docs/design/25 PR 59).
// Fixtures are built in the test.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <exiv2/exiv2.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "fixtures.h"
#include "meta/location.h"

namespace {

namespace fs = std::filesystem;
using Catch::Approx;

struct temp_file {
  fs::path path;
  explicit temp_file(const std::string& name) { path = fs::temp_directory_path() / ("mv_location_test_" + name); }
  ~temp_file() {
    std::error_code ec;
    fs::remove(path, ec);
  }
  void write(const std::vector<std::uint8_t>& bytes) const {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
};

std::vector<std::uint8_t> bytes_of(Exiv2::Image& image) {
  Exiv2::BasicIo& io = image.io();
  io.open();
  Exiv2::DataBuf buf = io.read(io.size());
  return {buf.c_data(), buf.c_data() + buf.size()};
}

std::vector<std::uint8_t> jpeg_with(const Exiv2::ExifData& exif, const Exiv2::XmpData& xmp) {
  std::vector<std::uint8_t> rgb(32 * 24 * 3, 128);
  auto jpeg = fixtures::jpeg_rgb(32, 24, rgb.data());
  auto image = Exiv2::ImageFactory::open(jpeg.data(), jpeg.size());
  image->setExifData(exif);
  image->setXmpData(xmp);
  image->writeMetadata();
  return bytes_of(*image);
}

}  // namespace

TEST_CASE("coordinates come from EXIF GPS, then XMP, from a bounded read", "[meta][location]") {
  {
    // Sintra: 38° 47' 53.16" N, 9° 23' 24.72" W, 200 m.
    Exiv2::ExifData exif;
    exif["Exif.GPSInfo.GPSLatitudeRef"] = "N";
    exif["Exif.GPSInfo.GPSLatitude"] = "38/1 47/1 5316/100";
    exif["Exif.GPSInfo.GPSLongitudeRef"] = "W";
    exif["Exif.GPSInfo.GPSLongitude"] = "9/1 23/1 2472/100";
    exif["Exif.GPSInfo.GPSAltitude"] = "200/1";
    temp_file f("exif.jpg");
    f.write(jpeg_with(exif, {}));
    const auto fix = mv::meta::read_location(f.path.string());
    REQUIRE(fix);
    REQUIRE(fix->lat == Approx(38.7981).margin(1e-4));
    REQUIRE(fix->lon == Approx(-9.3902).margin(1e-4));
    REQUIRE(fix->altitude);
    REQUIRE(*fix->altitude == Approx(200.0));
  }
  {
    // A drone's XMP only.
    Exiv2::XmpData xmp;
    xmp["Xmp.exif.GPSLatitude"] = "41,8.7N";
    xmp["Xmp.exif.GPSLongitude"] = "8,36.6W";
    temp_file f("xmp.jpg");
    f.write(jpeg_with({}, xmp));
    const auto fix = mv::meta::read_location(f.path.string());
    REQUIRE(fix);
    REQUIRE(fix->lat == Approx(41.145).margin(1e-6));
    REQUIRE(fix->lon == Approx(-8.61).margin(1e-6));
    REQUIRE_FALSE(fix->altitude);
  }
  {
    temp_file f("none.jpg");
    f.write(jpeg_with({}, {}));
    REQUIRE_FALSE(mv::meta::read_location(f.path.string()));
  }
  REQUIRE_FALSE(mv::meta::read_location((fs::temp_directory_path() / "mv_location_missing.jpg").string()));
}

TEST_CASE("coordinate text parses both ways a file or a summary writes it", "[meta][location]") {
  using mv::meta::parse_coordinate_pair;
  using mv::meta::xmp_coordinate_degrees;
  REQUIRE(*xmp_coordinate_degrees("38,47,53.16N") == Approx(38.7981).margin(1e-6));
  REQUIRE(*xmp_coordinate_degrees("9,23.412W") == Approx(-9.3902).margin(1e-6));
  REQUIRE_FALSE(xmp_coordinate_degrees("38.7981"));
  REQUIRE_FALSE(xmp_coordinate_degrees("38,61N"));
  REQUIRE_FALSE(xmp_coordinate_degrees("N"));
  const auto pair = parse_coordinate_pair("48.85837\xC2\xB0 N, 2.29448\xC2\xB0 E");
  REQUIRE(pair);
  REQUIRE(pair->lat == Approx(48.85837));
  REQUIRE(pair->lon == Approx(2.29448));
  REQUIRE(parse_coordinate_pair("33.86882\xC2\xB0 S, 151.20929\xC2\xB0 W")->lon == Approx(-151.20929));
  REQUIRE_FALSE(parse_coordinate_pair("48.85837 N, 2.29448 E"));
  REQUIRE_FALSE(parse_coordinate_pair("95.0\xC2\xB0 N, 2.0\xC2\xB0 E"));
}
