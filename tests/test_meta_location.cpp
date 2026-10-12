// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #352 (owner, 2026-10-12): the Metadata pane's Location editor. The parser both
// hosts call (meta::parse_location), and the position written through PR 12's
// checked writer and sidecar rule: EXIF GPS in a plain JPEG, XMP exif:GPS* in a
// sidecar for everything else, a RAW original never touched (rule 5).
#include <catch2/catch_test_macros.hpp>

#include <exiv2/exiv2.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "corpus.h"
#include "fixtures.h"
#include "io/content_hash.h"
#include "io/file.h"
#include "meta/meta.h"
#include "meta/write.h"

namespace {

namespace fs = std::filesystem;
using mv::meta::change;
using mv::meta::geo_point;
using mv::meta::location_input;
using mv::meta::parse_location;
using mv::meta::write_fields;
using mv::meta::write_target;

constexpr double kEps = 1e-6;  // degrees; ~0.1 m

bool near(double a, double b, double eps = kEps) { return std::fabs(a - b) < eps; }

location_input ok(const char* text) {
  const location_input r = parse_location(text);
  INFO(text << " -> " << r.error);
  REQUIRE(r.k == location_input::kind::point);
  return r;
}

void bad(const char* text) {
  const location_input r = parse_location(text);
  INFO(text);
  CHECK(r.k == location_input::kind::invalid);
  CHECK(std::string(r.error).size() > 0);
}

struct scratch_dir {
  fs::path dir;
  explicit scratch_dir(const std::string& name) {
    static int counter = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    dir = fs::temp_directory_path() /
          ("mv_meta_location_" + name + "_" + std::to_string(stamp) + "_" + std::to_string(counter++));
    fs::remove_all(dir);
    fs::create_directories(dir);
  }
  ~scratch_dir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  [[nodiscard]] std::string path(const std::string& file) const { return (dir / file).string(); }
  [[nodiscard]] std::string snapshots() const { return (dir / "snapshots").string(); }
  void write(const std::string& file, const std::vector<std::uint8_t>& bytes) const {
    std::ofstream f(dir / file, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  [[nodiscard]] std::vector<std::uint8_t> read(const std::string& file) const {
    auto b = mv::io::read_all((dir / file).string());
    return b ? *b : std::vector<std::uint8_t>{};
  }
  [[nodiscard]] std::string text(const std::string& file) const {
    const auto b = read(file);
    return {b.begin(), b.end()};
  }
};

std::vector<std::uint8_t> bytes_of(Exiv2::Image& image) {
  Exiv2::BasicIo& io = image.io();
  io.open();
  Exiv2::DataBuf buf = io.read(io.size());
  return {buf.c_data(), buf.c_data() + buf.size()};
}

std::vector<std::uint8_t> flat_jpeg() {
  std::vector<std::uint8_t> rgb(64 * 48 * 3, 128);
  return fixtures::jpeg_rgb(64, 48, rgb.data());
}

// A camera JPEG; with `gps`, a fix at the Eiffel Tower 35 m up, and the XMP
// copy some editors keep beside it.
std::vector<std::uint8_t> camera_jpeg(bool gps, bool xmp_gps = false) {
  const auto base = flat_jpeg();
  auto image = Exiv2::ImageFactory::open(base.data(), base.size());
  Exiv2::ExifData exif;
  exif["Exif.Image.Make"] = "ACME";
  exif["Exif.Image.Model"] = "Test Camera";
  exif["Exif.Photo.DateTimeOriginal"] = "2024:05:01 14:03:22";
  if (gps) {
    exif["Exif.GPSInfo.GPSVersionID"].setValue("2 2 0 0");
    exif["Exif.GPSInfo.GPSLatitudeRef"] = "N";
    exif["Exif.GPSInfo.GPSLatitude"].setValue("48/1 51/1 3024/100");
    exif["Exif.GPSInfo.GPSLongitudeRef"] = "E";
    exif["Exif.GPSInfo.GPSLongitude"].setValue("2/1 17/1 4020/100");
    exif["Exif.GPSInfo.GPSAltitudeRef"].setValue("0");
    exif["Exif.GPSInfo.GPSAltitude"].setValue("35/1");
    exif["Exif.GPSInfo.GPSImgDirection"].setValue("1234/10");
  }
  image->setExifData(exif);
  Exiv2::XmpData xmp;
  xmp["Xmp.dc.creator"] = "Test Author";
  if (xmp_gps) {
    xmp["Xmp.exif.GPSLatitude"] = "48,51.504N";
    xmp["Xmp.exif.GPSLongitude"] = "2,17.67E";
  }
  image->setXmpData(xmp);
  image->writeMetadata();
  return bytes_of(*image);
}

// Everything that is not APPn / COM: the picture itself.
std::vector<std::uint8_t> picture_bytes(const std::vector<std::uint8_t>& j) {
  std::vector<std::uint8_t> out(j.begin(), j.begin() + 2);
  std::size_t i = 2;
  while (i + 4 <= j.size() && j[i] == 0xFF) {
    const std::uint8_t m = j[i + 1];
    if (m == 0xDA) {
      out.insert(out.end(), j.begin() + static_cast<std::ptrdiff_t>(i), j.end());
      return out;
    }
    const std::size_t len = (static_cast<std::size_t>(j[i + 2]) << 8) | j[i + 3];
    const bool meta = (m >= 0xE1 && m <= 0xED && m != 0xE2) || m == 0xFE;
    if (!meta) {
      out.insert(out.end(), j.begin() + static_cast<std::ptrdiff_t>(i),
                 j.begin() + static_cast<std::ptrdiff_t>(i + 2 + len));
    }
    i += 2 + len;
  }
  return out;
}

Exiv2::ExifData exif_of(const std::vector<std::uint8_t>& jpeg) {
  auto image = Exiv2::ImageFactory::open(jpeg.data(), jpeg.size());
  image->readMetadata();
  return image->exifData();
}
Exiv2::XmpData xmp_of(const std::vector<std::uint8_t>& jpeg) {
  auto image = Exiv2::ImageFactory::open(jpeg.data(), jpeg.size());
  image->readMetadata();
  return image->xmpData();
}
std::string exif_text(const Exiv2::ExifData& e, const char* key) {
  const auto it = e.findKey(Exiv2::ExifKey(key));
  return it == e.end() ? std::string{} : it->toString();
}
std::string xmp_text(const Exiv2::XmpData& x, const char* key) {
  const auto it = x.findKey(Exiv2::XmpKey(key));
  return it == x.end() ? std::string{} : it->toString();
}
std::size_t gps_rows(const Exiv2::ExifData& e) {
  std::size_t n = 0;
  for (const auto& d : e) n += d.groupName() == "GPSInfo" ? 1 : 0;
  return n;
}

write_fields set_location(double lat, double lon, std::optional<double> alt = std::nullopt) {
  write_fields f;
  geo_point p;
  p.lat = lat;
  p.lon = lon;
  p.altitude = alt;
  f.location = change<geo_point>::to(p);
  return f;
}
write_fields clear_location() {
  write_fields f;
  f.location = change<geo_point>::remove();
  return f;
}

std::string hash_of(const std::vector<std::uint8_t>& bytes) {
  mv::io::hasher h;
  h.update(bytes);
  return h.finish().hex();
}

}  // namespace

// ---- The parser ---------------------------------------------------------------

TEST_CASE("location text: decimal degrees", "[meta][location][parse]") {
  auto r = ok("38.7979, -9.3907");
  CHECK(near(r.point.lat, 38.7979));
  CHECK(near(r.point.lon, -9.3907));
  CHECK_FALSE(r.point.altitude);

  r = ok("38.7979 -9.3907");  // no comma: one space between two numbers
  CHECK(near(r.point.lat, 38.7979));
  CHECK(near(r.point.lon, -9.3907));
  r = ok("  +38.7979 ,   -9.3907  ");
  CHECK(near(r.point.lat, 38.7979));
  r = ok("38.7979;-9.3907");
  CHECK(near(r.point.lon, -9.3907));
  r = ok("-33.8688, 151.2093");
  CHECK(near(r.point.lat, -33.8688));
  CHECK(near(r.point.lon, 151.2093));
  r = ok("0, 0");
  CHECK(r.point.lat == 0.0);
  CHECK(r.point.lon == 0.0);
  CHECK_FALSE(std::signbit(ok("-0, -0.0").point.lat));  // never "-0"
  r = ok(".5, -.25");
  CHECK(near(r.point.lat, 0.5));
  CHECK(near(r.point.lon, -0.25));
  r = ok("38, -9");
  CHECK(near(r.point.lat, 38.0));
  r = ok("38.79790123456, -9.39070987654");
  CHECK(near(r.point.lat, 38.79790123456, 1e-10));
  r = ok("38.7979\xE2\x80\x89, \xE2\x88\x92" "9.3907");  // U+2212 minus
  CHECK(near(r.point.lon, -9.3907));
}

TEST_CASE("location text: hemisphere letters, either side, any order", "[meta][location][parse]") {
  auto r = ok("38.7979 N, 9.3907 W");
  CHECK(near(r.point.lat, 38.7979));
  CHECK(near(r.point.lon, -9.3907));
  r = ok("38.7979N 9.3907W");
  CHECK(near(r.point.lon, -9.3907));
  r = ok("N 38.7979, W 9.3907");
  CHECK(near(r.point.lat, 38.7979));
  CHECK(near(r.point.lon, -9.3907));
  r = ok("N38.7979 W9.3907");
  CHECK(near(r.point.lon, -9.3907));
  r = ok("9.3907 W, 38.7979 N");  // longitude first: the letters say so
  CHECK(near(r.point.lat, 38.7979));
  CHECK(near(r.point.lon, -9.3907));
  r = ok("33.8688 s, 151.2093 e");  // lower case
  CHECK(near(r.point.lat, -33.8688));
  CHECK(near(r.point.lon, 151.2093));
  r = ok("38.7979N, -9.3907");  // one half lettered, the other signed
  CHECK(near(r.point.lon, -9.3907));
}

TEST_CASE("location text: degrees, minutes and seconds", "[meta][location][parse]") {
  const double lat = 38.0 + 47.0 / 60.0 + 52.0 / 3600.0;
  const double lon = -(9.0 + 23.0 / 60.0 + 27.0 / 3600.0);
  auto r = ok("38\xC2\xB0" "47\xE2\x80\xB2" "52\xE2\x80\xB3 N, 9\xC2\xB0" "23\xE2\x80\xB2" "27\xE2\x80\xB3 W");
  CHECK(near(r.point.lat, lat));
  CHECK(near(r.point.lon, lon));
  r = ok("38 47 52 N 9 23 27 W");
  CHECK(near(r.point.lat, lat));
  CHECK(near(r.point.lon, lon));
  r = ok("38 47 52 N, 9 23 27 W");
  CHECK(near(r.point.lon, lon));
  r = ok("38\xC2\xB0" "47'52\" N, 9\xC2\xB0" "23'27\" W");  // ASCII ' and "
  CHECK(near(r.point.lat, lat));
  r = ok("38\xC2\xB0 47' 52'' N, 9\xC2\xB0 23' 27'' W");  // '' for seconds
  CHECK(near(r.point.lon, lon));
  r = ok("38\xC2\xBA" "47\xE2\x80\x99" "52\xE2\x80\x9D N, 9\xC2\xBA" "23\xE2\x80\x99" "27\xE2\x80\x9D W");  // º ’ ”
  CHECK(near(r.point.lat, lat));
  r = ok("N 38 47 52, W 9 23 27");
  CHECK(near(r.point.lon, lon));
  r = ok("38\xC2\xB0" "47.8667' N, 9\xC2\xB0" "23.45' W");  // degrees and decimal minutes
  CHECK(near(r.point.lat, 38.0 + 47.8667 / 60.0));
  CHECK(near(r.point.lon, -(9.0 + 23.45 / 60.0)));
  r = ok("-38 47 52, 9 23 27.5");
  CHECK(near(r.point.lat, -lat));
  CHECK(near(r.point.lon, 9.0 + 23.0 / 60.0 + 27.5 / 3600.0));
  r = ok("38\xC2\xB0 N, 9\xC2\xB0 W");
  CHECK(near(r.point.lat, 38.0));
  CHECK(near(r.point.lon, -9.0));
}

TEST_CASE("location text: ranges are checked", "[meta][location][parse]") {
  CHECK(ok("90, 180").point.lat == 90.0);
  CHECK(ok("-90, -180").point.lon == -180.0);
  CHECK(ok("90 0 0 S, 180 0 0 E").point.lat == -90.0);
  bad("90.0001, 0");
  bad("-91, 0");
  bad("0, 180.5");
  bad("0, -181");
  bad("91 N, 0 E");
  bad("0 N, 181 W");
  bad("89 60 0 N, 0 E");   // minutes under 60
  bad("89 59 60 N, 0 E");  // seconds under 60
  bad("38 61, 9 0");
  CHECK(std::string(parse_location("91, 0").error).find("Latitude") != std::string::npos);
  CHECK(std::string(parse_location("0, 181").error).find("Longitude") != std::string::npos);
  CHECK(std::string(parse_location("38 61 N, 9 W").error).find("60") != std::string::npos);
}

TEST_CASE("location text: blank removes, garbage is refused", "[meta][location][parse]") {
  CHECK(parse_location("").k == location_input::kind::blank);
  CHECK(parse_location("   ").k == location_input::kind::blank);
  CHECK(parse_location(" \t ").k == location_input::kind::blank);
  bad("38.7979");                  // one number
  bad("38.7979,");                 // one number and a comma
  bad(", -9.3907");
  bad("38.7979, -9.3907, 12");     // two commas
  bad("38.7979 -9.3907 12");       // three numbers, no separator
  bad("abc");
  bad("Lisbon");
  bad("38.79.79, -9.39");
  bad("38,7979, -9,3907");         // decimal commas
  bad("38.7979 N, 9.3907 N");      // two latitudes
  bad("38.7979 E, 9.3907 W");      // two longitudes
  bad("-38.7979 S, 9.3907 W");     // a sign and a letter
  bad("N 38.7979 S, 9 W");
  bad("38.5 47, 9 0");             // a number after a fraction
  bad("38 47 52 10, 9 0");         // four numbers
  bad("38' 47\xC2\xB0, 9 0");      // units out of place
  bad("38\xC2\xB0" "47'52\" 9\xC2\xB0" "23'27\"");  // DMS pair with no separator or letters
  bad("1e5, 0");
  bad("38.7979, -9.3907 \xF0\x9F\x93\x8D");  // an emoji pin
  bad("\xFF\xFE");
  bad(std::string(300, '1').c_str());
  bad("--38, 9");
  bad("+, 9");
  bad(".,.");
}

TEST_CASE("location text: the editor's starting text reads back to the point", "[meta][location][parse]") {
  geo_point p;
  p.lat = 38.7979;
  p.lon = -9.3907;
  CHECK(mv::meta::format_location_input(p) == "38.7979, -9.3907");
  for (const auto& [lat, lon] : std::vector<std::pair<double, double>>{
           {48.858370, 2.294481}, {-33.8688, 151.2093}, {0, 0}, {90, -180}, {-0.0000001, 0.0000001}}) {
    p.lat = lat;
    p.lon = lon;
    const std::string text = mv::meta::format_location_input(p);
    INFO(text);
    const auto r = parse_location(text);
    REQUIRE(r.k == location_input::kind::point);
    CHECK(near(r.point.lat, lat, 1e-6));
    CHECK(near(r.point.lon, lon, 1e-6));
    CHECK(text.find("-0,") == std::string::npos);
  }
}

// ---- What the row may do --------------------------------------------------------

TEST_CASE("location rights follow where the write lands", "[meta][location]") {
  mv::meta::metadata m;
  m.writes_in_file = true;
  auto r = mv::meta::location_rights_of(m);
  CHECK(r.can_set);
  CHECK_FALSE(r.can_clear);  // nothing to remove
  m.s.where = geo_point{1, 2, std::nullopt};
  m.where_in_file = true;
  r = mv::meta::location_rights_of(m);
  CHECK(r.can_clear);
  CHECK_FALSE(r.in_original);
  m.writes_in_file = false;  // a RAW / HEIC / clip with its own fix
  r = mv::meta::location_rights_of(m);
  CHECK(r.can_set);
  CHECK_FALSE(r.can_clear);
  CHECK(r.in_original);
  m.where_in_file = false;  // only the sidecar says where
  r = mv::meta::location_rights_of(m);
  CHECK(r.can_clear);
  CHECK_FALSE(r.in_original);
}

// ---- Writing --------------------------------------------------------------------

TEST_CASE("a plain JPEG takes a location in its EXIF GPS; the picture is untouched", "[meta][location][write]") {
  scratch_dir d("jpeg_new");
  mv::meta::reset_snapshot_session();
  const auto original = camera_jpeg(false);
  d.write("a.jpg", original);
  REQUIRE(*mv::meta::write_target_for(d.path("a.jpg")) == write_target::in_file);

  auto r = mv::meta::write(d.path("a.jpg"), set_location(38.7979, -9.3907), d.snapshots());
  REQUIRE(r);
  CHECK(r->target == write_target::in_file);
  CHECK_FALSE(fs::exists(d.dir / "a.xmp"));
  const auto now = d.read("a.jpg");
  CHECK(picture_bytes(now) == picture_bytes(original));

  const auto e = exif_of(now);
  CHECK(exif_text(e, "Exif.GPSInfo.GPSVersionID") == "2 3 0 0");
  CHECK(exif_text(e, "Exif.GPSInfo.GPSLatitudeRef") == "N");
  CHECK(exif_text(e, "Exif.GPSInfo.GPSLongitudeRef") == "W");
  CHECK(exif_text(e, "Exif.GPSInfo.GPSLatitude") == "38/1 47/1 524400/10000");
  CHECK(exif_text(e, "Exif.GPSInfo.GPSLongitude") == "9/1 23/1 265200/10000");
  CHECK(exif_text(e, "Exif.GPSInfo.GPSAltitude").empty());  // none given, none had
  CHECK(exif_text(e, "Exif.Image.Model") == "Test Camera");  // the rest carries over

  auto m = mv::meta::read(d.path("a.jpg"));
  REQUIRE(m);
  REQUIRE(m->s.where);
  CHECK(near(m->s.where->lat, 38.7979));
  CHECK(near(m->s.where->lon, -9.3907));
  CHECK(m->s.gps == "38.79790\xC2\xB0 N, 9.39070\xC2\xB0 W");
  CHECK(m->where_in_file);
  // The Summary row and All tags come from the same read.
  bool row = false;
  for (const auto& f : mv::meta::summary_rows(*m)) row |= f.label == "Location" && f.value == m->s.gps;
  CHECK(row);
  bool tag = false;
  for (const auto& p : m->properties) tag |= p.raw_tag == "Exif.GPSInfo.GPSLatitude";
  CHECK(tag);

  // Revert puts the file back byte for byte (the whole-metadata snapshot).
  REQUIRE(mv::meta::revert(d.path("a.jpg"), d.snapshots()));
  CHECK(d.read("a.jpg") == original);
}

TEST_CASE("editing a JPEG's location keeps its altitude and its other GPS tags", "[meta][location][write]") {
  scratch_dir d("jpeg_edit");
  mv::meta::reset_snapshot_session();
  const auto original = camera_jpeg(true, /*xmp_gps=*/true);
  d.write("a.jpg", original);

  REQUIRE(mv::meta::write(d.path("a.jpg"), set_location(-33.8688, 151.2093), d.snapshots()));
  const auto now = d.read("a.jpg");
  CHECK(picture_bytes(now) == picture_bytes(original));
  const auto e = exif_of(now);
  CHECK(exif_text(e, "Exif.GPSInfo.GPSVersionID") == "2 2 0 0");  // present: left as it was
  CHECK(exif_text(e, "Exif.GPSInfo.GPSLatitudeRef") == "S");
  CHECK(exif_text(e, "Exif.GPSInfo.GPSLongitudeRef") == "E");
  CHECK(exif_text(e, "Exif.GPSInfo.GPSAltitude") == "35/1");
  CHECK(exif_text(e, "Exif.GPSInfo.GPSImgDirection") == "1234/10");
  // The JPEG's own XMP copy follows, so no reader sees two places.
  const auto x = xmp_of(now);
  CHECK(xmp_text(x, "Xmp.exif.GPSLatitude") == "33,52.128000S");
  CHECK(xmp_text(x, "Xmp.exif.GPSLongitude") == "151,12.558000E");
  CHECK(xmp_text(x, "Xmp.dc.creator") == "Test Author");

  auto m = mv::meta::read(d.path("a.jpg"));
  REQUIRE(m);
  REQUIRE(m->s.where);
  CHECK(near(m->s.where->lat, -33.8688));
  CHECK(near(m->s.where->lon, 151.2093));
  REQUIRE(m->s.where->altitude);
  CHECK(near(*m->s.where->altitude, 35.0));

  // An altitude given is written, below sea level included.
  REQUIRE(mv::meta::write(d.path("a.jpg"), set_location(31.5, 35.5, -430.5), d.snapshots()));
  m = mv::meta::read(d.path("a.jpg"));
  REQUIRE(m);
  REQUIRE(m->s.where);
  REQUIRE(m->s.where->altitude);
  CHECK(near(*m->s.where->altitude, -430.5));
  CHECK(exif_text(exif_of(d.read("a.jpg")), "Exif.GPSInfo.GPSAltitudeRef") == "1");
}

TEST_CASE("removing a JPEG's location drops every GPS row, EXIF and XMP", "[meta][location][write]") {
  scratch_dir d("jpeg_clear");
  mv::meta::reset_snapshot_session();
  const auto original = camera_jpeg(true, /*xmp_gps=*/true);
  d.write("a.jpg", original);
  REQUIRE(gps_rows(exif_of(original)) >= 7);

  auto r = mv::meta::write(d.path("a.jpg"), clear_location(), d.snapshots());
  REQUIRE(r);
  CHECK(r->target == write_target::in_file);
  const auto now = d.read("a.jpg");
  CHECK(picture_bytes(now) == picture_bytes(original));
  CHECK(gps_rows(exif_of(now)) == 0);
  CHECK(xmp_text(xmp_of(now), "Xmp.exif.GPSLatitude").empty());
  CHECK(xmp_text(xmp_of(now), "Xmp.dc.creator") == "Test Author");
  CHECK(exif_text(exif_of(now), "Exif.Image.Model") == "Test Camera");
  auto m = mv::meta::read(d.path("a.jpg"));
  REQUIRE(m);
  CHECK_FALSE(m->s.where);
  CHECK(m->s.gps.empty());
  for (const auto& p : m->properties) {
    INFO(p.raw_tag);
    CHECK(p.raw_tag.rfind("Exif.GPSInfo.", 0) != 0);
  }
}

TEST_CASE("a sidecar beside a JPEG takes the same location as the file", "[meta][location][write]") {
  scratch_dir d("jpeg_sidecar");
  mv::meta::reset_snapshot_session();
  d.write("a.jpg", camera_jpeg(true));
  const std::string theirs =
      "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
      "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
      "<rdf:Description rdf:about=\"\" xmlns:exif=\"http://ns.adobe.com/exif/1.0/\" "
      "xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\" xmp:Rating=\"2\" "
      "exif:GPSLatitude=\"10,0N\" exif:GPSLongitude=\"20,0E\"/>"
      "</rdf:RDF></x:xmpmeta>\n<?xpacket end=\"w\"?>";
  d.write("a.xmp", std::vector<std::uint8_t>(theirs.begin(), theirs.end()));
  // The sidecar wins on read, before and after.
  auto m = mv::meta::read(d.path("a.jpg"));
  REQUIRE(m);
  REQUIRE(m->s.where);
  CHECK(near(m->s.where->lat, 10.0));

  auto r = mv::meta::write(d.path("a.jpg"), set_location(38.7979, -9.3907), d.snapshots());
  REQUIRE(r);
  CHECK(r->target == write_target::in_file);
  CHECK(r->sidecar_touched);
  m = mv::meta::read(d.path("a.jpg"));
  REQUIRE(m);
  REQUIRE(m->s.where);
  CHECK(near(m->s.where->lat, 38.7979));
  CHECK(near(m->s.where->lon, -9.3907));
  CHECK(m->s.rating == 2);  // theirs, kept

  REQUIRE(mv::meta::write(d.path("a.jpg"), clear_location(), d.snapshots()));
  m = mv::meta::read(d.path("a.jpg"));
  REQUIRE(m);
  CHECK_FALSE(m->s.where);
  CHECK(d.text("a.xmp").find("GPSLatitude") == std::string::npos);
  CHECK(m->s.rating == 2);
}

TEST_CASE("a file that is never rewritten takes its location in the sidecar", "[meta][location][write]") {
  scratch_dir d("png");
  mv::meta::reset_snapshot_session();
  std::vector<std::uint8_t> rgba(16 * 16 * 4, 77);
  const auto png = fixtures::png_rgba(16, 16, rgba.data());
  d.write("a.png", png);

  auto m = mv::meta::read(d.path("a.png"));
  REQUIRE(m);
  CHECK_FALSE(mv::meta::location_rights_of(*m).can_clear);  // nothing there yet

  auto r = mv::meta::write(d.path("a.png"), set_location(38.7979, -9.3907, 120.0), d.snapshots());
  REQUIRE(r);
  CHECK(r->target == write_target::sidecar);
  CHECK(d.read("a.png") == png);  // rule 5
  const std::string packet = d.text("a.xmp");
  CHECK(packet.find("38,47.874000N") != std::string::npos);
  CHECK(packet.find("9,23.442000W") != std::string::npos);
  CHECK(packet.find("2.3.0.0") != std::string::npos);
  CHECK(packet.find("12000/100") != std::string::npos);

  m = mv::meta::read(d.path("a.png"));
  REQUIRE(m);
  REQUIRE(m->s.where);
  CHECK(near(m->s.where->lat, 38.7979));
  CHECK(near(m->s.where->lon, -9.3907));
  REQUIRE(m->s.where->altitude);
  CHECK(near(*m->s.where->altitude, 120.0));
  CHECK(m->s.gps == "38.79790\xC2\xB0 N, 9.39070\xC2\xB0 W");
  CHECK_FALSE(m->where_in_file);
  const auto rights = mv::meta::location_rights_of(*m);
  CHECK(rights.can_clear);  // only the sidecar holds it
  CHECK_FALSE(rights.in_original);

  // Lat / lon alone keep the sidecar's altitude.
  REQUIRE(mv::meta::write(d.path("a.png"), set_location(1.5, 2.5), d.snapshots()));
  m = mv::meta::read(d.path("a.png"));
  REQUIRE(m);
  REQUIRE(m->s.where);
  CHECK(near(m->s.where->lat, 1.5));
  REQUIRE(m->s.where->altitude);
  CHECK(near(*m->s.where->altitude, 120.0));

  // Removing it takes the sidecar away when nothing else is in it.
  REQUIRE(mv::meta::write(d.path("a.png"), clear_location(), d.snapshots()));
  CHECK_FALSE(fs::exists(d.dir / "a.xmp"));
  m = mv::meta::read(d.path("a.png"));
  REQUIRE(m);
  CHECK_FALSE(m->s.where);
  CHECK(d.read("a.png") == png);
}

TEST_CASE("a JPEG written through its sidecar keeps the camera's fix but shows the new one", "[meta][location][write]") {
  // An MPF table makes in-place editing unsafe, so this JPEG behaves as a RAW
  // or HEIC does: its own EXIF GPS stays, the sidecar's position goes over it.
  scratch_dir d("mpf");
  mv::meta::reset_snapshot_session();
  auto j = camera_jpeg(true);
  const std::vector<std::uint8_t> mpf = {0xFF, 0xE2, 0x00, 0x10, 'M', 'P', 'F', 0, 0, 0, 0, 0, 0, 0, 0, 0};
  j.insert(j.begin() + 2, mpf.begin(), mpf.end());
  d.write("a.jpg", j);
  REQUIRE(*mv::meta::write_target_for(d.path("a.jpg")) == write_target::sidecar);

  auto m = mv::meta::read(d.path("a.jpg"));
  REQUIRE(m);
  REQUIRE(m->where_in_file);
  auto rights = mv::meta::location_rights_of(*m);
  CHECK(rights.can_set);
  CHECK_FALSE(rights.can_clear);
  CHECK(rights.in_original);

  REQUIRE(mv::meta::write(d.path("a.jpg"), set_location(38.7979, -9.3907), d.snapshots()));
  CHECK(d.read("a.jpg") == j);
  m = mv::meta::read(d.path("a.jpg"));
  REQUIRE(m);
  REQUIRE(m->s.where);
  CHECK(near(m->s.where->lat, 38.7979));
  REQUIRE(m->s.where->altitude);
  CHECK(near(*m->s.where->altitude, 35.0));  // the camera's altitude, kept
  CHECK(mv::meta::location_rights_of(*m).in_original);

  // A remove through the sidecar alone would leave the camera's fix showing:
  // the writer never takes a JPEG's EXIF out through a sidecar fallback, and the
  // row offers no remove (location_rights). Removing the sidecar's own does work.
  REQUIRE(mv::meta::write(d.path("a.jpg"), clear_location(), d.snapshots()));
  CHECK(d.read("a.jpg") == j);
  m = mv::meta::read(d.path("a.jpg"));
  REQUIRE(m);
  REQUIRE(m->s.where);
  CHECK(near(m->s.where->lat, 48.0 + 51.0 / 60.0 + 30.24 / 3600.0));  // the camera's again
}

TEST_CASE("location values out of range are refused before anything is written", "[meta][location][write]") {
  scratch_dir d("refuse");
  mv::meta::reset_snapshot_session();
  const auto original = camera_jpeg(true);
  d.write("a.jpg", original);
  for (const write_fields& f : {set_location(91, 0), set_location(0, -181), set_location(NAN, 0),
                                set_location(0, INFINITY), set_location(0, 0, 1e6)}) {
    const auto r = mv::meta::write(d.path("a.jpg"), f, d.snapshots());
    REQUIRE_FALSE(r);
    CHECK(r.error() == mv::status::invalid_arg);
  }
  CHECK(d.read("a.jpg") == original);
  CHECK_FALSE(fs::exists(d.dir / "a.xmp"));
}

TEST_CASE("coordinates survive the EXIF and XMP encodings to within a few millimetres", "[meta][location][write]") {
  scratch_dir d("roundtrip");
  mv::meta::reset_snapshot_session();
  std::vector<std::uint8_t> rgba(16 * 16 * 4, 33);
  d.write("a.jpg", camera_jpeg(false));
  d.write("a.png", fixtures::png_rgba(16, 16, rgba.data()));
  const std::vector<std::pair<double, double>> points = {
      {38.797912, -9.390734}, {-33.868820, 151.209296}, {89.9999999, 179.9999999},
      {-89.9999999, -179.9999999}, {0.0000004, -0.0000004}, {51.477928, -0.001545}, {64.1466, -21.9426}};
  for (const auto& [lat, lon] : points) {
    INFO(lat << ", " << lon);
    for (const char* file : {"a.jpg", "a.png"}) {
      INFO(file);
      REQUIRE(mv::meta::write(d.path(file), set_location(lat, lon), d.snapshots()));
      const auto m = mv::meta::read(d.path(file));
      REQUIRE(m);
      REQUIRE(m->s.where);
      CHECK(near(m->s.where->lat, lat, 1e-7));  // ~1 cm
      CHECK(near(m->s.where->lon, lon, 1e-7));
    }
  }
}

TEST_CASE("a RAW by its magic bytes takes its location in the sidecar; the original never changes", "[meta][location][write][raw]") {
  // Corpus-free twin of the test below: an Olympus ORF header ("IIRO") probes
  // as RAW, so the write goes to IMG.xmp and the original is never opened for
  // writing (rule 5). Runs everywhere, with or without tools/testmedia.
  scratch_dir d("fakeraw");
  mv::meta::reset_snapshot_session();
  std::vector<std::uint8_t> raw(4096, 0x5A);
  raw[0] = 'I';
  raw[1] = 'I';
  raw[2] = 'R';
  raw[3] = 'O';
  d.write("IMG_0001.ORF", raw);
  const std::string before = hash_of(d.read("IMG_0001.ORF"));
  REQUIRE(*mv::meta::write_target_for(d.path("IMG_0001.ORF")) == write_target::sidecar);

  auto r = mv::meta::write(d.path("IMG_0001.ORF"), set_location(38.7979, -9.3907, 12.5), d.snapshots());
  REQUIRE(r);
  CHECK(r->target == write_target::sidecar);
  CHECK(hash_of(d.read("IMG_0001.ORF")) == before);
  const std::string packet = d.text("IMG_0001.xmp");
  CHECK(packet.find("38,47.874000N") != std::string::npos);
  CHECK(packet.find("9,23.442000W") != std::string::npos);

  auto m = mv::meta::read(d.path("IMG_0001.ORF"));
  REQUIRE(m);
  REQUIRE(m->s.where);
  CHECK(near(m->s.where->lat, 38.7979));
  CHECK(near(m->s.where->lon, -9.3907));
  REQUIRE(m->s.where->altitude);
  CHECK(near(*m->s.where->altitude, 12.5));
  CHECK(mv::meta::location_rights_of(*m).can_clear);  // only the sidecar holds it

  // Lat / lon alone keep the altitude; a removal drops it with the rest.
  REQUIRE(mv::meta::write(d.path("IMG_0001.ORF"), set_location(-33.86882, 151.209296), d.snapshots()));
  m = mv::meta::read(d.path("IMG_0001.ORF"));
  REQUIRE(m);
  REQUIRE(m->s.where);
  REQUIRE(m->s.where->altitude);
  CHECK(near(*m->s.where->altitude, 12.5));
  REQUIRE(mv::meta::write(d.path("IMG_0001.ORF"), clear_location(), d.snapshots()));
  CHECK_FALSE(fs::exists(d.dir / "IMG_0001.xmp"));
  m = mv::meta::read(d.path("IMG_0001.ORF"));
  REQUIRE(m);
  CHECK_FALSE(m->s.where);
  CHECK(hash_of(d.read("IMG_0001.ORF")) == before);
}

TEST_CASE("real RAW files: the location goes to the sidecar, the original's hash never moves", "[meta][location][write][raw]") {
  std::vector<std::string> files;
  for (const char* name : {"canon_eos7dmk2.cr2", "nikon_d7500.nef", "sony_ilce7rm3.arw", "canon_eosr6.cr3",
                           "pentax_k50.dng"}) {
    const std::string p = corpus::path_of((std::string("raw/") + name).c_str());
    if (!p.empty()) files.push_back(p);
  }
  if (files.empty()) {
    if (corpus::corpus_required()) FAIL("MV_REQUIRE_CORPUS is set and tools/testmedia/raw is empty");
    SKIP("tools/testmedia/raw not fetched. This test proved NOTHING.");
  }
  scratch_dir d("raw");
  mv::meta::reset_snapshot_session();
  for (const std::string& src : files) {
    const std::string name = fs::path(src).filename().string();
    INFO(name);
    fs::copy_file(src, d.dir / name);
    const std::string before = hash_of(d.read(name));

    auto m = mv::meta::read(d.path(name));
    REQUIRE(m);
    CHECK_FALSE(m->writes_in_file);
    const bool had = m->s.where.has_value();
    CHECK(mv::meta::location_rights_of(*m).can_clear == false);  // nothing, or the camera's own

    auto r = mv::meta::write(d.path(name), set_location(38.7979, -9.3907), d.snapshots());
    REQUIRE(r);
    CHECK(r->target == write_target::sidecar);
    CHECK(hash_of(d.read(name)) == before);
    m = mv::meta::read(d.path(name));
    REQUIRE(m);
    REQUIRE(m->s.where);
    CHECK(near(m->s.where->lat, 38.7979));
    CHECK(near(m->s.where->lon, -9.3907));
    CHECK(m->where_in_file == had);

    REQUIRE(mv::meta::write(d.path(name), clear_location(), d.snapshots()));
    CHECK(hash_of(d.read(name)) == before);
    m = mv::meta::read(d.path(name));
    REQUIRE(m);
    CHECK(m->s.where.has_value() == had);
    CHECK(hash_of(d.read(name)) == before);
  }
}
