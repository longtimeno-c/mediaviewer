// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #606: a GoPro or DJI clip's GPS route from its telemetry (meta/telemetry.h),
// on GPMF payloads built byte for byte from the published format and on clips
// this test writes (tests/telemetry_fixture.h, tests/cue_fixture.h).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cue_fixture.h"
#include "meta/internal.h"
#include "meta/telemetry.h"
#include "telemetry_fixture.h"

namespace {

namespace fs = std::filesystem;
namespace meta = mv::meta;
namespace gp = mv::test::gpmf;
using Catch::Approx;

struct temp_file {
  fs::path path;
  explicit temp_file(const std::string& name) : path(fs::temp_directory_path() / ("mv_telemetry_test_" + name)) {}
  ~temp_file() {
    std::error_code ec;
    fs::remove(path, ec);
  }
};

std::vector<meta::geo_point> read_payload(const gp::bytes& b, std::size_t* found = nullptr) {
  meta::route_builder r(meta::route_limits{.max_points = 100000, .min_step_m = 0});
  const std::size_t n = meta::read_gpmf(b, r);
  if (found) *found = n;
  return r.take();
}

// A drive from Lisbon (Praça do Comércio) toward Sintra, one fix every ~5.6 m.
std::vector<gp::fix> drive(std::size_t n, std::size_t from = 0) {
  std::vector<gp::fix> out;
  for (std::size_t i = from; i < from + n; ++i) {
    const double t = static_cast<double>(i) / 2000.0;
    out.push_back({38.7075 + 0.09 * t, -9.1365 - 0.25 * t, 20.0 + static_cast<double>(i % 7)});
  }
  return out;
}

// The distance from p to the segment ab, in metres (flat over a few km).
double to_segment_m(const meta::geo_point& p, const meta::geo_point& a, const meta::geo_point& b) {
  const double k = std::cos(p.lat * 3.14159265358979 / 180.0) * 111320.0;
  const double ax = (a.lon - p.lon) * k, ay = (a.lat - p.lat) * 110574.0;
  const double bx = (b.lon - p.lon) * k, by = (b.lat - p.lat) * 110574.0;
  const double dx = bx - ax, dy = by - ay;
  const double len2 = dx * dx + dy * dy;
  double t = len2 > 0 ? -(ax * dx + ay * dy) / len2 : 0;
  t = std::fmax(0.0, std::fmin(1.0, t));
  return std::hypot(ax + t * dx, ay + t * dy);
}

double off_route_m(const meta::geo_point& p, const std::vector<meta::geo_point>& route) {
  double best = 1e18;
  for (std::size_t i = 0; i + 1 < route.size(); ++i) best = std::fmin(best, to_segment_m(p, route[i], route[i + 1]));
  if (route.size() == 1) best = meta::distance_m(p, route[0]);
  return best;
}

}  // namespace

TEST_CASE("telemetry: a GPS5 stream's fixes, over its SCAL", "[meta][telemetry]") {
  const auto fixes = drive(18);
  std::size_t found = 0;
  const auto pts = read_payload(gp::payload(gp::gps5_strm(fixes)), &found);
  CHECK(found == 18);
  REQUIRE(pts.size() == 18);
  for (std::size_t i = 0; i < pts.size(); ++i) {
    CHECK(pts[i].lat == Approx(fixes[i].lat).margin(1e-7));
    CHECK(pts[i].lon == Approx(fixes[i].lon).margin(1e-7));
    REQUIRE(pts[i].altitude);
    CHECK(*pts[i].altitude == Approx(fixes[i].alt).margin(1e-3));
  }
  // The accelerometer stream beside it is not a position.
  CHECK(read_payload(gp::payload(gp::accl_strm())).empty());
}

TEST_CASE("telemetry: no lock, poor precision and no SCAL give no fix", "[meta][telemetry]") {
  CHECK(read_payload(gp::payload(gp::gps5_strm(drive(5), 0))).empty());        // GPSF 0: no lock
  CHECK(read_payload(gp::payload(gp::gps5_strm(drive(5), 2))).size() == 5);    // 2D lock is a fix
  CHECK(read_payload(gp::payload(gp::gps5_strm(drive(5), 3, 999))).empty());   // GPSP 9.99: too poor
  CHECK(read_payload(gp::payload(gp::gps5_strm(drive(5), 3, 500))).size() == 5);
  CHECK(read_payload(gp::payload(gp::gps5_strm(drive(5), 3, 120, false))).empty());  // no SCAL
  // 0, 0 is what a receiver reports before lock.
  CHECK(read_payload(gp::payload(gp::gps5_strm({{0, 0, 0}}))).empty());
}

TEST_CASE("telemetry: a GPS9 stream (HERO11 on) reads fix and DOP per sample", "[meta][telemetry]") {
  std::vector<gp::fix9> f = {{38.80, -9.38, 210, 1.1, 3}, {38.81, -9.39, 211, 1.1, 0},   // no lock
                             {38.82, -9.40, 212, 7.5, 3}, {38.83, -9.41, 213, 2.0, 2}};  // DOP 7.5: dropped
  const auto pts = read_payload(gp::payload(gp::gps9_strm(f)));
  REQUIRE(pts.size() == 2);
  CHECK(pts[0].lat == Approx(38.80).margin(1e-7));
  CHECK(pts[0].lon == Approx(-9.38).margin(1e-7));
  CHECK(*pts[0].altitude == Approx(210).margin(1e-3));
  CHECK(pts[1].lat == Approx(38.83).margin(1e-7));
}

TEST_CASE("telemetry: a damaged payload reads what precedes the damage and nothing past it",
          "[meta][telemetry]") {
  const gp::bytes good = gp::payload(gp::gps5_strm(drive(10)));
  for (std::size_t cut = 0; cut < good.size(); cut += 3) {
    const gp::bytes part(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut));
    CHECK(read_payload(part).size() <= 10);
  }
  gp::bytes flipped = good;
  for (std::size_t i = 0; i < flipped.size(); i += 7) flipped[i] ^= 0x5A;
  (void)read_payload(flipped);  // anything, without reading out of bounds (ASan, the fuzzer)
  // A nest that claims more than it holds stops the walk.
  gp::bytes liar = good;
  liar[6] = 0xFF;
  liar[7] = 0xFF;
  CHECK(read_payload(liar).empty());
  // Deep nesting is capped, not recursed into.
  gp::bytes deep = gp::gps5_strm(drive(1));
  for (int i = 0; i < 12; ++i) deep = gp::nest("DEVC", {deep});
  CHECK(read_payload(deep).empty());
}

TEST_CASE("telemetry: the route drops standing-still repeats and thins a long one", "[meta][telemetry]") {
  meta::route_builder r(meta::route_limits{.max_points = 100, .min_step_m = 1.0});
  meta::geo_point p{38.7, -9.1, std::nullopt};
  for (int i = 0; i < 50; ++i) r.add(p);  // 18 Hz on a tripod
  CHECK(r.points().size() == 1);
  CHECK(r.accepted() == 50);
  for (int i = 1; i <= 1000; ++i) r.add({38.7 + i * 1e-4, -9.1, std::nullopt});  // ~11 m apart
  CHECK(r.thinned());
  CHECK(r.points().size() <= 100);
  CHECK(r.points().size() >= 50);
  CHECK(r.points().front().lat == Approx(38.7));
  r.add({91, 0, std::nullopt});
  r.add({std::nan(""), 0, std::nullopt});
  CHECK(r.accepted() == 1050);
  CHECK(meta::distance_m({0, 0, std::nullopt}, {0, 1, std::nullopt}) == Approx(111195).epsilon(0.001));

  std::vector<meta::geo_point> line;
  for (int i = 0; i < 1000; ++i) line.push_back({static_cast<double>(i) / 100, 0, std::nullopt});
  const auto s = meta::sample_route(line, 64);
  REQUIRE(s.size() == 64);
  CHECK(s.front().lat == line.front().lat);
  CHECK(s.back().lat == line.back().lat);
  CHECK(meta::sample_route(line, 0).empty());
  CHECK(meta::sample_route(std::span(line).first(10), 64).size() == 10);
}

TEST_CASE("telemetry: DJI subtitle text, both shapes", "[meta][telemetry]") {
  const auto a = meta::parse_dji_text(
      "FrameCnt: 1, DiffTime: 33ms 2021-08-27 13:13:32.384 [iso : 100] [shutter : 1/1000.0] [fnum : 280] "
      "[ev : 0] [ct : 5500] [color_md : default] [focal_len : 240] [latitude: 38.797912] [longitude: -9.390722] "
      "[rel_alt: 1.200 abs_alt: 85.912]");
  REQUIRE(a);
  CHECK(a->lat == Approx(38.797912));
  CHECK(a->lon == Approx(-9.390722));
  REQUIRE(a->altitude);
  CHECK(*a->altitude == Approx(85.912));
  // DJI's own spelling, and a space before the colon.
  const auto b = meta::parse_dji_text("[latitude : 22.570766] [longtitude : 113.869911] [altitude: 105.0]");
  REQUIRE(b);
  CHECK(b->lat == Approx(22.570766));
  CHECK(b->lon == Approx(113.869911));
  CHECK(*b->altitude == Approx(105.0));
  // Phantom 4 / Mavic Pro: GPS(lon, lat, alt), longitude first.
  const auto c = meta::parse_dji_text("HOME(149.0251,-20.2532) 2017.08.05 14:11:51 GPS(149.0251,-20.2533,16) BAROMETER:1.9");
  REQUIRE(c);
  CHECK(c->lat == Approx(-20.2533));
  CHECK(c->lon == Approx(149.0251));
  const auto d = meta::parse_dji_text("F/2.8, SS 320, ISO 100, EV 0, GPS (8.6553, 49.8806, 19), D 23.27m, H 15.70m");
  REQUIRE(d);
  CHECK(d->lat == Approx(49.8806));
  CHECK(d->lon == Approx(8.6553));
  // Before lock, and text that is not telemetry.
  CHECK_FALSE(meta::parse_dji_text("[latitude: 0.000000] [longitude: 0.000000]"));
  CHECK_FALSE(meta::parse_dji_text("[latitude: 95.0] [longitude: 10.0]"));
  CHECK_FALSE(meta::parse_dji_text("I'll be back."));
  CHECK_FALSE(meta::parse_dji_text("The GPS (please) was wrong"));
  CHECK_FALSE(meta::parse_dji_text(""));
}

TEST_CASE("telemetry: a GoPro clip gets a place from its first fix, and the pane its route", "[meta][telemetry]") {
  temp_file f("gopro.mp4");
  // 30 packets of 18 fixes; the first two seconds without lock, as a camera
  // just switched on writes them.
  std::vector<gp::bytes> packets;
  std::vector<gp::fix> all;
  for (std::size_t i = 0; i < 30; ++i) {
    const auto fixes = drive(18, i * 18);
    if (i < 2) {
      packets.push_back(gp::payload(gp::gps5_strm(fixes, 0)));
      continue;
    }
    all.insert(all.end(), fixes.begin(), fixes.end());
    packets.push_back(gp::payload(gp::gps5_strm(fixes)));
  }
  REQUIRE(gp::make_mp4(f.path.string(), packets));

  // The tag pass: the head only, stopping at the first fix.
  meta::metadata head;
  REQUIRE(meta::detail::read_clip(f.path.string(), head));
  REQUIRE(head.s.where);
  CHECK(head.s.where->lat == Approx(all.front().lat).margin(1e-6));
  CHECK(head.s.where->lon == Approx(all.front().lon).margin(1e-6));
  CHECK_FALSE(head.s.gps.empty());
  CHECK(head.route.size() == 1);

  // The pane: every packet, the route within 20 m of the device's own fixes.
  meta::metadata full;
  REQUIRE(meta::detail::read_clip(f.path.string(), full, nullptr, meta::detail::telemetry_read::full));
  REQUIRE(full.route.size() > 100);
  CHECK(full.s.where->lat == Approx(all.front().lat).margin(1e-6));
  double worst = 0;
  for (const auto& x : all) worst = std::fmax(worst, off_route_m({x.lat, x.lon, std::nullopt}, full.route));
  CHECK(worst < 20.0);
  bool source_row = false;
  for (const auto& p : full.properties) source_row = source_row || (p.raw_tag == "Telemetry.Source" && p.value == "GoPro GPMF");
  CHECK(source_row);

  // The search side's coarse copy: 64 points, still on the route.
  const meta::detail::route_read coarse = meta::detail::clip_route(f.path.string(), 64);
  REQUIRE_FALSE(coarse.points.empty());
  CHECK(coarse.points.size() <= 64);
  CHECK(coarse.fixes == all.size());
}

TEST_CASE("telemetry: a clip whose GPS never locks has no place", "[meta][telemetry]") {
  temp_file f("gopro-nolock.mp4");
  std::vector<gp::bytes> packets;
  for (std::size_t i = 0; i < 4; ++i) packets.push_back(gp::payload(gp::gps5_strm(drive(18, i * 18), 0)));
  REQUIRE(gp::make_mp4(f.path.string(), packets));
  meta::metadata m;
  REQUIRE(meta::detail::read_clip(f.path.string(), m, nullptr, meta::detail::telemetry_read::full));
  CHECK_FALSE(m.s.where);
  CHECK(m.route.empty());
}

TEST_CASE("telemetry: a DJI clip's subtitle track places it; a film's subtitles do not", "[meta][telemetry]") {
  temp_file f("dji.mp4");
  std::vector<mv::test::cuefx::line> lines;
  lines.push_back({0, 33, "FrameCnt: 1 [latitude: 0.000000] [longitude: 0.000000]"});  // before lock
  for (int i = 1; i < 90; ++i) {
    char buf[200];
    std::snprintf(buf, sizeof buf, "FrameCnt: %d, DiffTime: 33ms [iso : 100] [latitude: %.6f] [longitude: %.6f] [rel_alt: 30.0 abs_alt: 120.5]",
                  i + 1, 38.79 + i * 1e-4, -9.39 - i * 1e-4);
    lines.push_back({i * 33, i * 33 + 33, buf});
  }
  REQUIRE(mv::test::cuefx::make(f.path.string(), "mp4", {}, lines));
  meta::metadata head;
  REQUIRE(meta::detail::read_clip(f.path.string(), head));
  REQUIRE(head.s.where);
  CHECK(head.s.where->lat == Approx(38.7901).margin(1e-6));
  CHECK(head.s.where->lon == Approx(-9.3901).margin(1e-6));
  meta::metadata full;
  REQUIRE(meta::detail::read_clip(f.path.string(), full, nullptr, meta::detail::telemetry_read::full));
  CHECK(full.route.size() == 89);

  temp_file film("film.mp4");
  REQUIRE(mv::test::cuefx::make(film.path.string(), "mp4", {}, {{0, 1000, "Where are we?"}, {1000, 2000, "Lisbon."}}));
  meta::metadata m;
  REQUIRE(meta::detail::read_clip(film.path.string(), m, nullptr, meta::detail::telemetry_read::full));
  CHECK_FALSE(m.s.where);
  CHECK(m.route.empty());
}

// The corpus of tools/fuzz/fuzz_meta_gpmf.cpp, from the builders above.
// Hidden: `MV_SEED_DIR=tests/data/seeds mv_tests "[.write-seeds]"`.
TEST_CASE("write the telemetry fuzz seeds", "[.write-seeds]") {
  const char* dir = std::getenv("MV_SEED_DIR");
  if (!dir) SKIP("set MV_SEED_DIR");
  const fs::path d = fs::path(dir) / "gpmf";
  fs::create_directories(d);
  const auto put = [&](const std::string& file, const gp::bytes& b) {
    std::ofstream f(d / file, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  };
  put("gps5.gpmf", gp::payload(gp::gps5_strm(drive(18))));
  put("gps5_nolock.gpmf", gp::payload(gp::gps5_strm(drive(4), 0)));
  put("gps9.gpmf", gp::payload(gp::gps9_strm({{38.80, -9.38, 210, 1.1, 3}, {38.81, -9.39, 211, 6.0, 3}})));
  const std::string dji = "FrameCnt: 2, DiffTime: 33ms [iso : 100] [latitude: 38.797912] [longtitude: -9.390722] "
                          "[rel_alt: 1.200 abs_alt: 85.912]";
  put("dji_brackets.txt", gp::bytes(dji.begin(), dji.end()));
  const std::string p4 = "HOME(149.0251,-20.2532) 2017.08.05 14:11:51\nGPS(149.0251,-20.2533,16) BAROMETER:1.9";
  put("dji_gps.txt", gp::bytes(p4.begin(), p4.end()));
}
