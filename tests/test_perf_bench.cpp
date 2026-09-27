// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Headless timing of the paths a person waits on: first pixel (preview
// decode), the full decode behind it, the colour stage, and listing and
// sorting a camera-dump folder. Not part of the normal run:
//
//     mv_tests "[.perf-bench]"
//
// MV_BENCH_JSON=path writes the medians as JSON (tools/perf/regenerate.py
// collects it). MV_BENCH_DIR=folder adds every still in that folder to the
// decode rows. The on-screen numbers (present pacing, a key to a presented
// frame) come from the lab's soaks, not from here.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "catch_compat.h"
#include "corpus.h"
#include "fixtures.h"

#include "codec/decode.h"
#include "image/colour.h"
#include "image/pipeline.h"
#include "io/dir.h"
#include "io/file.h"
#include "io/sort_order.h"

namespace {

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

template <class F>
double median_ms(int reps, F&& f) {
  std::vector<double> t;
  t.reserve(static_cast<std::size_t>(reps));
  for (int i = 0; i < reps; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    t.push_back(ms_since(t0));
  }
  std::sort(t.begin(), t.end());
  return t[t.size() / 2];
}

struct row {
  std::string group;
  std::string name;
  double ms = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

void emit(const std::vector<row>& rows) {
  for (const row& r : rows) {
    std::printf("[perf] %-10s %-28s %9.2f ms  %ux%u\n", r.group.c_str(), r.name.c_str(), r.ms,
                r.width, r.height);
  }
  const char* path = std::getenv("MV_BENCH_JSON");
  if (!path || !*path) return;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << "{\n  \"schema\": 1,\n  \"rows\": [\n";
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const row& r = rows[i];
    char line[512];
    std::snprintf(line, sizeof line,
                  "    {\"group\": \"%s\", \"name\": \"%s\", \"ms\": %.3f, \"width\": %u, "
                  "\"height\": %u}%s\n",
                  r.group.c_str(), r.name.c_str(), r.ms, r.width, r.height,
                  i + 1 < rows.size() ? "," : "");
    out << line;
  }
  out << "  ]\n}\n";
}

std::vector<std::filesystem::path> bench_files() {
  std::vector<std::filesystem::path> files;
  const auto media = corpus::media_dir();
  if (!media.empty()) {
    for (const char* sub : {"raw", "heif"}) {
      std::error_code ec;
      for (const auto& e : std::filesystem::directory_iterator(media / sub, ec)) {
        if (e.is_regular_file()) files.push_back(e.path());
      }
    }
  }
  if (const char* extra = std::getenv("MV_BENCH_DIR"); extra && *extra) {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(extra, ec)) {
      if (e.is_regular_file()) files.push_back(e.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

}  // namespace

TEST_CASE("perf: decode, colour and folder timings", "[.perf-bench]") {
  std::vector<row> rows;

  // A 24 MP camera JPEG, tagged Adobe RGB so the colour stage does real work,
  // and the same pixels untagged (sRGB: identity transform).
  {
    constexpr std::uint32_t w = 6000, h = 4000;
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w) * h * 3);
    std::uint32_t seed = 12345;
    for (std::size_t i = 0; i < rgb.size(); ++i) {
      // Smooth gradient plus noise: compresses like a photo, not like a flat fill.
      seed = seed * 1664525u + 1013904223u;
      const std::size_t px = i / 3;
      rgb[i] = static_cast<std::uint8_t>((px % w) * 200 / w + (px / w) * 40 / h + ((seed >> 24) & 15));
    }
    const auto tagged = fixtures::jpeg_rgb(w, h, rgb.data(), fixtures::adobe_rgb_icc());
    const auto plain = fixtures::jpeg_rgb(w, h, rgb.data(), {});
    std::vector<std::uint8_t>().swap(rgb);

    rows.push_back({"jpeg", "decode 24MP (codec)", median_ms(5, [&] {
                      auto r = mv::codec::decode(plain);
                      REQUIRE(r);
                    }),
                    w, h});
    rows.push_back({"jpeg", "full 24MP sRGB", median_ms(5, [&] {
                      auto r = mv::image::decode_bytes(plain);
                      REQUIRE(r);
                    }),
                    w, h});
    rows.push_back({"jpeg", "full 24MP Adobe RGB", median_ms(5, [&] {
                      auto r = mv::image::decode_bytes(tagged);
                      REQUIRE(r);
                    }),
                    w, h});
    rows.push_back({"jpeg", "preview 24MP Adobe RGB", median_ms(5, [&] {
                      auto r = mv::image::decode_preview(tagged);
                      REQUIRE(r);
                    }),
                    w, h});
  }

  // Real camera files: first pixel and the full decode behind it.
  for (const auto& path : bench_files()) {
    auto bytes = mv::io::read_all(path.string());
    if (!bytes) continue;
    const std::string name = path.filename().string();
    std::uint32_t pw = 0, ph = 0, fw = 0, fh = 0;
    (void)mv::image::decode_preview(bytes.value());  // warm the code and the allocator
    const double preview = median_ms(5, [&] {
      auto r = mv::image::decode_preview(bytes.value());
      if (r) {
        pw = r->width;
        ph = r->height;
      }
    });
    const double full = median_ms(3, [&] {
      auto r = mv::image::decode_bytes(bytes.value());
      if (r) {
        fw = r->width;
        fh = r->height;
      }
    });
    if (pw) rows.push_back({"preview", name, preview, pw, ph});
    if (fw) rows.push_back({"full", name, full, fw, fh});
  }

  // A camera dump: 3,000 stills (RAW+JPEG pairs) and 40 sub-folders.
  {
    const auto dir = std::filesystem::temp_directory_path() / "mv_perf_bench_folder";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    for (int i = 0; i < 1500; ++i) {
      char n[64];
      std::snprintf(n, sizeof n, "DSC%05d", i);
      std::ofstream(dir / (std::string(n) + ".JPG")) << 'x';
      std::ofstream(dir / (std::string(n) + ".ARW")) << 'x';
    }
    for (int i = 0; i < 40; ++i) std::filesystem::create_directories(dir / ("day " + std::to_string(i)));
    const std::string d = dir.string();

    std::size_t n_files = 0, n_dirs = 0;
    rows.push_back({"folder", "list 3000 stills", median_ms(9, [&] {
                      auto r = mv::io::list_still_files(d);
                      REQUIRE(r);
                      n_files = r->size();
                    }),
                    static_cast<std::uint32_t>(n_files), 0});
    rows.push_back({"folder", "scan sub-folders", median_ms(9, [&] {
                      auto r = mv::io::scan_subdirs(d);
                      REQUIRE(r);
                      n_dirs = r->size();
                    }),
                    static_cast<std::uint32_t>(n_dirs), 0});
    CHECK(n_files == 3000);
    CHECK(n_dirs == 40);

    auto entries = mv::io::list_still_files(d);
    REQUIRE(entries);
    const auto lookup = [](const mv::io::dir_entry& e) -> std::optional<std::int64_t> {
      // Stand-in for meta_store: a hashed key per entry, as the host builds.
      return static_cast<std::int64_t>(std::hash<std::string>{}(e.name_utf8) % 100000);
    };
    rows.push_back({"folder", "sort 3000 by date taken", median_ms(9, [&] {
                      auto copy = entries.value();
                      mv::io::sort_entries(copy, {mv::io::sort_key::date_taken, false}, lookup);
                    }),
                    3000, 0});
    std::filesystem::remove_all(dir, ec);
  }

  emit(rows);
}
