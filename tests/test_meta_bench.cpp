// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #290: what one meta::read costs, for the before / after numbers CLAUDE.md asks of
// a change on a worker path (docs/DEVELOPMENT.md "Performance suite"). Hidden; uses
// only the public read API so the same file builds against a base checkout:
//   mv_tests "[.perf-meta]"     (MV_BENCH_ROUNDS, default 40; MV_BENCH_CLIP=a.mp4 adds a clip)
#include <catch2/catch_test_macros.hpp>

#include <exiv2/exiv2.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "fixtures.h"
#include "meta/meta.h"
#if __has_include("meta/probe.h")
#include "meta/probe.h"
#define MV_HAVE_PROBE 1
#endif

namespace {

namespace fs = std::filesystem;

std::vector<std::uint8_t> noise(std::size_t n) {
  std::vector<std::uint8_t> v(n);
  std::uint32_t s = 12345;
  for (auto& b : v) {
    s = s * 1664525u + 1013904223u;
    b = static_cast<std::uint8_t>(s >> 24);
  }
  return v;
}

std::vector<std::uint8_t> camera_jpeg() {
  const auto rgb = noise(3000u * 2000u * 3u);
  auto jpeg = fixtures::jpeg_rgb(3000, 2000, rgb.data());
  auto image = Exiv2::ImageFactory::open(jpeg.data(), jpeg.size());
  Exiv2::ExifData exif;
  exif["Exif.Image.Make"] = "Canon";
  exif["Exif.Image.Model"] = "Canon EOS R5";
  exif["Exif.Image.Software"] = "Firmware Version 1.8.1";
  exif["Exif.Photo.DateTimeOriginal"] = "2024:05:01 14:03:22";
  exif["Exif.Photo.ExposureTime"] = Exiv2::URational(1, 250);
  exif["Exif.GPSInfo.GPSLatitudeRef"] = "N";
  exif["Exif.GPSInfo.GPSLatitude"] = "48/1 51/1 3012/100";
  exif["Exif.GPSInfo.GPSLongitudeRef"] = "E";
  exif["Exif.GPSInfo.GPSLongitude"] = "2/1 17/1 4012/100";
  image->setExifData(exif);
  Exiv2::XmpData xmp;
  xmp["Xmp.xmp.CreatorTool"] = "Adobe Lightroom";
  xmp["Xmp.dc.subject"] = "holiday";
  image->setXmpData(xmp);
  image->writeMetadata();
  Exiv2::BasicIo& io = image->io();
  io.open();
  Exiv2::DataBuf buf = io.read(io.size());
  return {buf.c_data(), buf.c_data() + buf.size()};
}

double median_us(const std::string& path, int rounds) {
  std::vector<double> t;
  for (int i = 0; i < rounds; ++i) {
    const auto a = std::chrono::steady_clock::now();
    auto m = mv::meta::read(path);
    const auto b = std::chrono::steady_clock::now();
    REQUIRE(m);
    t.push_back(std::chrono::duration<double, std::micro>(b - a).count());
  }
  std::sort(t.begin(), t.end());
  return t[t.size() / 2];
}

}  // namespace

TEST_CASE("perf: one metadata read per format", "[.perf-meta]") {
  const int rounds = std::getenv("MV_BENCH_ROUNDS") ? std::atoi(std::getenv("MV_BENCH_ROUNDS")) : 40;
  const fs::path dir = fs::temp_directory_path() / "mv_meta_bench";
  fs::create_directories(dir);
  const auto write = [&](const std::string& name, const std::vector<std::uint8_t>& b) {
    const fs::path p = dir / name;
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return p.string();
  };
  std::vector<std::pair<std::string, std::string>> files;
  files.emplace_back("camera JPEG 6 MP", write("camera.jpg", camera_jpeg()));
  {
    const auto rgba = noise(1500u * 1500u * 4u);
    files.emplace_back("PNG 2.2 MP", write("big.png", fixtures::png_rgba(1500, 1500, rgba.data())));
  }
  // Run from the repo root (as ctest does) for the HEIC and WebP seeds.
  const fs::path seeds = fs::current_path() / "tests" / "data" / "seeds";
  for (const char* rel : {"heic/8bit.heic", "webp/lossy.webp"}) {
    if (fs::exists(seeds / rel)) files.emplace_back(rel, (seeds / rel).string());
  }
  // A real clip, when given (the read model's clip path is libavformat's).
  if (const char* clip = std::getenv("MV_BENCH_CLIP"); clip && fs::exists(clip)) files.emplace_back("clip", clip);
  for (const auto& [label, path] : files) {
    (void)median_us(path, 3);  // warm the page cache, as the published runs are
    std::printf("[perf-meta] %-20s median %.1f us over %d reads\n", label.c_str(), median_us(path, rounds), rounds);
#ifdef MV_HAVE_PROBE
    // #345: the AI badge's provenance probe on the same file.
    std::vector<double> t;
    for (int i = 0; i < rounds; ++i) {
      const auto a = std::chrono::steady_clock::now();
      (void)mv::meta::probe_provenance(path);
      t.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a).count());
    }
    std::sort(t.begin(), t.end());
    std::printf("[perf-meta] %-20s probe median %.1f us\n", label.c_str(), t[t.size() / 2]);
#endif
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}
