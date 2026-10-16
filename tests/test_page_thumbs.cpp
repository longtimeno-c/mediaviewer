// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #429, the page sidebar's core: several pages of a document from one
// open (codec::decode_pages), their thumbnails in the disk cache
// (image::make_page_thumbs) and the bookkeeping both hosts share
// (image::page_thumb_book). Hand-built PDFs (pdf_fixture.h), so no corpus.
#include "catch_compat.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "codec/decode.h"
#include "image/page_thumbs.h"
#include "image/thumb.h"
#include "pdf_fixture.h"

using mv::status;
namespace fs = std::filesystem;
namespace pdffx = mv::test::pdffx;

namespace {

struct temp_dir {
  fs::path dir;
  temp_dir() {
    dir = fs::temp_directory_path() / ("mv_page_thumbs_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
  }
  ~temp_dir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  temp_dir(const temp_dir&) = delete;
  temp_dir& operator=(const temp_dir&) = delete;
};

// Page n is a 100 x 140 pt page whose top half is a colour of its own.
std::vector<std::uint8_t> numbered_pages(int count) {
  static const char* const kFill[] = {"1 0 0 rg", "0 1 0 rg", "0 0 1 rg"};
  std::vector<pdffx::page> pages;
  for (int i = 0; i < count; ++i) {
    pages.push_back({100, 140, 0, std::string(kFill[i % 3]) + " 0 70 100 70 re f"});
  }
  return pdffx::make(pages);
}

struct collected {
  std::vector<mv::codec::raster> pages;
  std::size_t stop_after = SIZE_MAX;
};

bool collect(void* user, mv::codec::raster&& page) {
  auto& c = *static_cast<collected*>(user);
  c.pages.push_back(std::move(page));
  return c.pages.size() < c.stop_after;
}

struct landed {
  std::vector<std::uint32_t> pages;
  std::vector<std::string> paths;
};

bool on_landed(void* user, std::uint32_t page, std::string_view path) {
  auto& l = *static_cast<landed*>(user);
  l.pages.push_back(page);
  l.paths.emplace_back(path);
  return true;
}

}  // namespace

TEST_CASE("decode_pages renders the pages asked for, in that order, from one open",
          "[page-sidebar][pdf]") {
  const auto bytes = numbered_pages(3);
  collected got;
  const std::vector<std::uint32_t> want = {2, 0, 9};  // 9 is past the end: skipped
  auto count = mv::codec::decode_pages(bytes, want, 256, nullptr, &got, collect);
  REQUIRE(count);
  CHECK(count.value() == 3);
  REQUIRE(got.pages.size() == 2);
  CHECK(got.pages[0].page == 2);
  CHECK(got.pages[1].page == 0);
  for (const auto& p : got.pages) {
    CHECK(p.page_count == 3);
    CHECK(p.height == 256);  // the long edge of a portrait page
    // The same pixels a single-page decode at that size gives.
    auto one = mv::codec::decode_pdf(bytes, p.page, nullptr, 256);
    REQUIRE(one);
    CHECK(one->width == p.width);
    CHECK(one->rgba == p.rgba);
  }
}

TEST_CASE("decode_pages stops when the sink says so", "[page-sidebar][pdf]") {
  const auto bytes = numbered_pages(4);
  collected got;
  got.stop_after = 1;
  const std::vector<std::uint32_t> want = {0, 1, 2, 3};
  auto count = mv::codec::decode_pages(bytes, want, 64, nullptr, &got, collect);
  REQUIRE(count);
  CHECK(got.pages.size() == 1);
}

TEST_CASE("decode_pages takes documents only", "[page-sidebar]") {
  const std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0};
  collected got;
  const std::vector<std::uint32_t> want = {0};
  auto r = mv::codec::decode_pages(png, want, 64, nullptr, &got, collect);
  REQUIRE_FALSE(r);
  CHECK(r.error() == status::unsupported_format);
}

TEST_CASE("make_page_thumbs renders the misses once and finds them after",
          "[page-sidebar][pdf]") {
  temp_dir tmp;
  const fs::path file = tmp.dir / "doc.pdf";
  const auto bytes = numbered_pages(5);
  {
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  mv::image::thumb_store store;
  fs::create_directories(tmp.dir / "cache");
  REQUIRE(store.open((tmp.dir / "cache").string()));
  const mv::image::thumb_key key{file.string(), 1700000000, bytes.size()};

  landed first;
  std::uint32_t count = 0;
  const std::vector<std::uint32_t> pages = {1, 3, 7};
  CHECK(mv::image::make_page_thumbs(store, key, pages, nullptr, &first, on_landed, &count) ==
        status::ok);
  CHECK(count == 5);  // the render learned it
  REQUIRE(first.pages == std::vector<std::uint32_t>{1, 3});
  for (const auto& p : first.paths) CHECK(fs::exists(p));
  // Its own row beside the file's, under the file's stamp.
  auto row = store.lookup(mv::image::page_thumb_key(key.path, 3, key.mtime_unix, key.size));
  REQUIRE(row);
  CHECK(row.value() == first.paths[1]);
  CHECK(mv::image::page_thumb_key("/a.pdf", 12, 1, 2).path == "/a.pdf#page=12");

  // Asked again for the pages that exist: every one is a hit, nothing is opened.
  landed second;
  const std::vector<std::uint32_t> again = {1, 3};
  CHECK(mv::image::make_page_thumbs(store, key, again, nullptr, &second, on_landed, &count) ==
        status::ok);
  CHECK(count == 0);
  CHECK(second.pages == first.pages);
  CHECK(second.paths == first.paths);

  // A JPEG small enough for the sidebar, not the folder thumbnail's size.
  auto decoded = mv::codec::decode([&] {
    std::ifstream in(first.paths[0], std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }());
  REQUIRE(decoded);
  CHECK(std::max(decoded->width, decoded->height) == mv::image::kPageThumbLongEdge);
}

TEST_CASE("page_thumb_book: a stop's serial, the window, flight and landings", "[page-sidebar]") {
  mv::image::page_thumb_book book;
  auto a = book.want("/docs/a.pdf", 0, 4);
  CHECK(a.pages == std::vector<std::uint32_t>{0, 1, 2, 3});
  const std::uint32_t serial = a.serial;

  // In flight: not asked for twice.
  auto again = book.want("/docs/a.pdf", 2, 4);
  CHECK(again.serial == serial);
  CHECK(again.pages == std::vector<std::uint32_t>{4, 5});

  // Landed: kept, and never asked for again.
  CHECK(book.landed(serial, 1, "/cache/1.jpg"));
  CHECK(book.path(1) == "/cache/1.jpg");
  CHECK(book.path(0).empty());
  book.finished(serial, std::vector<std::uint32_t>{0, 1, 2, 3});
  auto retry = book.want("/docs/a.pdf", 0, 2);
  CHECK(retry.pages == std::vector<std::uint32_t>{0});  // 1 landed; 0 is asked again

  // A fling: the window moved far on; a queued job keeps only what is near it.
  (void)book.want("/docs/a.pdf", 100, 5);
  const std::vector<std::uint32_t> queued = {4, 5, 95, 101};
  CHECK(book.still_wanted(serial, queued) == std::vector<std::uint32_t>{95, 101});
  // Pages it dropped left flight: the window coming back asks for them.
  auto back = book.want("/docs/a.pdf", 4, 2);
  CHECK(back.pages == std::vector<std::uint32_t>{4, 5});

  // Another document: a new serial, and nothing of the last one is kept or accepted.
  auto b = book.want("/docs/b.pdf", 0, 2);
  CHECK(b.serial != serial);
  CHECK(b.pages == std::vector<std::uint32_t>{0, 1});
  CHECK(book.path(1).empty());
  CHECK_FALSE(book.landed(serial, 7, "/cache/old.jpg"));
  CHECK(book.still_wanted(serial, std::vector<std::uint32_t>{0}).empty());

  // A zero-length request only names the stop (the hosts' first call).
  const std::uint32_t before = book.serial();
  CHECK(book.want("/docs/b.pdf", 0, 0).serial == before);
  CHECK(book.want("/docs/c.pdf", 0, 0).serial != before);
  book.clear();
  CHECK(book.serial() != before);
}

// The verify line's numbers (issue #429) on a real document: `MV_BENCH_DOC` is a
// PDF, DOCX or PPTX of a few hundred pages. Run with
//   mv_tests "[.perf-bench][page-sidebar]"
// Medians of 5, cold cache for the window (a fresh one per run), warm for the rest.
TEST_CASE("perf: page sidebar windows and a jump to a far page", "[.perf-bench][page-sidebar]") {
  const char* doc = std::getenv("MV_BENCH_DOC");
  if (!doc || !fs::exists(doc)) SKIP("MV_BENCH_DOC names no file");
  using clock = std::chrono::steady_clock;
  const auto ms = [](clock::time_point a, clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };
  const auto median = [](std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
  };
  std::ifstream in(doc, std::ios::binary);
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  collected probe;
  const std::vector<std::uint32_t> first_page = {0};
  auto count = mv::codec::decode_pages(bytes, first_page, 64, nullptr, &probe, collect);
  REQUIRE(count);
  const std::uint32_t pages = count.value();
  const std::uint32_t far = pages > 250 ? 249 : pages - 1;  // "page 250"

  std::vector<double> window_cold, window_far_cold, first_pixel, full_page;
  for (int run = 0; run < 5; ++run) {
    temp_dir tmp;
    mv::image::thumb_store store;
    REQUIRE(store.open(tmp.dir.string()));
    const mv::image::thumb_key key{doc, 1700000000 + run, bytes.size()};
    // What the strip shows on opening (rows 0-5 and two below).
    std::vector<std::uint32_t> window;
    for (std::uint32_t p = 0; p < std::min<std::uint32_t>(8, pages); ++p) window.push_back(p);
    landed got;
    auto t0 = clock::now();
    CHECK(mv::image::make_page_thumbs(store, key, window, nullptr, &got, on_landed) == status::ok);
    window_cold.push_back(ms(t0, clock::now()));
    // A fling to the end lands there: the window around the far page.
    std::vector<std::uint32_t> far_window;
    for (std::uint32_t p = far > 4 ? far - 4 : 0; p < std::min(pages, far + 4); ++p) far_window.push_back(p);
    landed got_far;
    t0 = clock::now();
    CHECK(mv::image::make_page_thumbs(store, key, far_window, nullptr, &got_far, on_landed) == status::ok);
    window_far_cold.push_back(ms(t0, clock::now()));
    // The click: the cached thumbnail up first (lookup, read, decode), then the page.
    t0 = clock::now();
    auto row = store.lookup_unverified(mv::image::page_thumb_key(key.path, far, key.mtime_unix, key.size));
    REQUIRE(row);
    std::ifstream jf(row.value(), std::ios::binary);
    const std::vector<std::uint8_t> jpeg((std::istreambuf_iterator<char>(jf)), std::istreambuf_iterator<char>());
    auto thumb = mv::codec::decode(jpeg);
    REQUIRE(thumb);
    first_pixel.push_back(ms(t0, clock::now()));
    t0 = clock::now();
    auto page = mv::codec::decode(bytes, nullptr, 4, far);
    REQUIRE(page);
    full_page.push_back(ms(t0, clock::now()));
  }
  std::printf("{\"doc\":\"%s\",\"pages\":%u,\"window8_cold_ms\":%.1f,\"window8_far_cold_ms\":%.1f,"
              "\"jump_first_pixel_ms\":%.2f,\"jump_full_page_ms\":%.1f}\n",
              fs::path(doc).filename().string().c_str(), pages, median(window_cold),
              median(window_far_cold), median(first_pixel), median(full_page));
}
