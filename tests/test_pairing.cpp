// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <vector>

#include "io/pairing.h"

using mv::io::dir_entry;
using mv::io::pair_kind;
using mv::io::pair_listing;

namespace {

dir_entry e(const char* name, const char* path = nullptr) {
  dir_entry d;
  d.name_utf8 = name;
  d.path_utf8 = path ? path : name;
  d.size = 1;
  return d;
}

}  // namespace

TEST_CASE("RAW+JPEG of the same stem is one stop, JPEG primary", "[io][pairing]") {
  auto listed = pair_listing({e("DSC_0123.NEF"), e("DSC_0123.JPG")});
  REQUIRE(listed.size() == 1);
  REQUIRE(listed[0].kind == pair_kind::raw_jpeg);
  REQUIRE(listed[0].primary.name_utf8 == "DSC_0123.JPG");
  REQUIRE(listed[0].secondary.name_utf8 == "DSC_0123.NEF");
}

TEST_CASE("Live Photo HEIC+MOV is one stop, still primary", "[io][pairing]") {
  auto listed = pair_listing({e("IMG_1234.MOV"), e("IMG_1234.HEIC")});
  REQUIRE(listed.size() == 1);
  REQUIRE(listed[0].kind == pair_kind::live_photo);
  REQUIRE(listed[0].primary.name_utf8 == "IMG_1234.HEIC");
  REQUIRE(listed[0].secondary.name_utf8 == "IMG_1234.MOV");
}

TEST_CASE("unpaired RAW stays a stop", "[io][pairing]") {
  auto listed = pair_listing({e("DSC_0001.ARW"), e("other.jpg")});
  REQUIRE(listed.size() == 2);
  REQUIRE(listed[0].kind == pair_kind::none);
  REQUIRE(listed[0].primary.name_utf8 == "DSC_0001.ARW");
  REQUIRE(listed[0].secondary.path_utf8.empty());
}

TEST_CASE("three files of one stem stay separate", "[io][pairing]") {
  auto listed = pair_listing({e("DSC_1.NEF"), e("DSC_1.JPG"), e("DSC_1.MOV")});
  REQUIRE(listed.size() == 3);
  for (const auto& it : listed) {
    REQUIRE(it.kind == pair_kind::none);
    REQUIRE(it.secondary.path_utf8.empty());
  }
}

TEST_CASE("case-insensitive stem match", "[io][pairing]") {
  auto listed = pair_listing({e("img_9.heic"), e("IMG_9.mov")});
  REQUIRE(listed.size() == 1);
  REQUIRE(listed[0].kind == pair_kind::live_photo);
}

TEST_CASE("iPhone Most Compatible JPG+MOV is a Live Photo; MP4 never pairs", "[io][pairing]") {
  auto live = pair_listing({e("IMG_0007.JPG"), e("IMG_0007.MOV")});
  REQUIRE(live.size() == 1);
  REQUIRE(live[0].kind == pair_kind::live_photo);
  REQUIRE(live[0].primary.name_utf8 == "IMG_0007.JPG");
  REQUIRE(live[0].secondary.name_utf8 == "IMG_0007.MOV");

  auto mp4 = pair_listing({e("IMG_0008.JPG"), e("IMG_0008.MP4")});
  REQUIRE(mp4.size() == 2);
  REQUIRE(mp4[0].kind == pair_kind::none);
}

TEST_CASE("RAW+HEIC pairs like RAW+JPEG; two RAWs or two stills do not", "[io][pairing]") {
  auto heic = pair_listing({e("DSC_2.HIF"), e("DSC_2.ARW")});
  REQUIRE(heic.size() == 1);
  REQUIRE(heic[0].kind == pair_kind::raw_jpeg);
  REQUIRE(heic[0].primary.name_utf8 == "DSC_2.HIF");

  REQUIRE(pair_listing({e("x.CR2"), e("x.DNG")}).size() == 2);
  REQUIRE(pair_listing({e("x.JPG"), e("x.HEIC")}).size() == 2);
  REQUIRE(pair_listing({e("x.PNG"), e("x.NEF")}).size() == 2);
}

TEST_CASE("a pair takes its first file's place; nothing else moves", "[io][pairing]") {
  // Name order as list_still_files returns it.
  auto listed = pair_listing({e("a.jpg"), e("DSC_1 (2).JPG"), e("DSC_1.JPG"), e("DSC_1.NEF"),
                              e("DSC_1_x.jpg"), e("IMG_5.HEIC"), e("m.png"), e("IMG_5.MOV")});
  REQUIRE(listed.size() == 6);
  REQUIRE(listed[0].primary.name_utf8 == "a.jpg");
  REQUIRE(listed[1].primary.name_utf8 == "DSC_1 (2).JPG");
  REQUIRE(listed[2].primary.name_utf8 == "DSC_1.JPG");
  REQUIRE(listed[2].kind == pair_kind::raw_jpeg);
  REQUIRE(listed[3].primary.name_utf8 == "DSC_1_x.jpg");
  // Not adjacent in the listing (another sort key), still one stop, at the still.
  REQUIRE(listed[4].primary.name_utf8 == "IMG_5.HEIC");
  REQUIRE(listed[4].kind == pair_kind::live_photo);
  REQUIRE(listed[5].primary.name_utf8 == "m.png");
}

TEST_CASE("non-ASCII stems: identical bytes pair, case-only differences stay two", "[io][pairing]") {
  // "Été" (UTF-8). Cameras write DCF ASCII names; the fold is ASCII only, so
  // the non-ASCII case never hides a file.
  auto same = pair_listing({e("\xC3\x89t\xC3\xA9.JPG"), e("\xC3\x89t\xC3\xA9.nef")});
  REQUIRE(same.size() == 1);
  REQUIRE(same[0].kind == pair_kind::raw_jpeg);
  auto folded = pair_listing({e("\xC3\x89T\xC3\x89.JPG"), e("\xC3\xA9t\xC3\xA9.NEF")});
  REQUIRE(folded.size() == 2);
}

TEST_CASE("pairing a 4000-file dump is scan-fast and exact", "[io][pairing][perf]") {
  std::vector<dir_entry> entries;
  constexpr int stems = 2000;
  entries.reserve(stems * 2);
  for (int i = 0; i < stems; ++i) {
    char jpg[32];
    char raw[32];
    std::snprintf(jpg, sizeof(jpg), "DSC_%04d.JPG", i);
    std::snprintf(raw, sizeof(raw), "DSC_%04d.NEF", i);
    entries.push_back(e(jpg));
    entries.push_back(e(raw));
  }
  const auto t0 = std::chrono::steady_clock::now();
  constexpr int runs = 10;
  std::size_t stops = 0;
  for (int r = 0; r < runs; ++r) stops = pair_listing(entries).size();
  const auto per_run_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() /
      runs;
  INFO("pair_listing 4000 entries: " << per_run_ms << " ms");
  REQUIRE(stops == stems);
  // The old O(n^2) scan took seconds here; the watcher relists on every change.
  // ASan on a hosted runner measured 131 ms (2026-09-24). Keep that budget far
  // below a quadratic regression, and keep the tight one everywhere else.
#ifdef MV_ASAN
  REQUIRE(per_run_ms < 400.0);
#else
  REQUIRE(per_run_ms < 50.0);
#endif
}
