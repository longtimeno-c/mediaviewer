// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// image::thumb_store: the JPEG-512 cache's SQLite index (image/thumb_store.cpp).
// Issue #168: statements prepared once, no stat or write under the lock, and a
// reader's failed read is the miss.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "image/thumb.h"

namespace fs = std::filesystem;

namespace {

struct temp_cache {
  fs::path dir;
  temp_cache() {
    dir = fs::temp_directory_path() / ("mv_thumb_store_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
  }
  ~temp_cache() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  temp_cache(const temp_cache&) = delete;
  temp_cache& operator=(const temp_cache&) = delete;
};

const std::vector<std::uint8_t> kJpeg = {0xFF, 0xD8, 0xFF, 0xD9};

mv::image::thumb_key key_n(int n) {
  return mv::image::thumb_key{"/photos/IMG_" + std::to_string(n) + ".JPG", 1700000000 + n,
                              static_cast<std::uint64_t>(1000 + n)};
}

}  // namespace

TEST_CASE("thumb store: miss, store, hit", "[image][thumb]") {
  temp_cache tc;
  mv::image::thumb_store store;
  REQUIRE(store.open(tc.dir.string()));
  const auto key = key_n(1);

  auto miss = store.lookup(key);
  REQUIRE(miss);
  CHECK(miss.value().empty());

  auto stored = store.store(key, kJpeg);
  REQUIRE(stored);
  CHECK(fs::exists(stored.value()));

  auto hit = store.lookup(key);
  REQUIRE(hit);
  CHECK(hit.value() == stored.value());
  auto unverified = store.lookup_unverified(key);
  REQUIRE(unverified);
  CHECK(unverified.value() == stored.value());

  // Another stamp of the same path is another row.
  auto other = store.lookup(mv::image::thumb_key{key.path, key.mtime_unix + 1, key.size});
  REQUIRE(other);
  CHECK(other.value().empty());
}

TEST_CASE("thumb store: a row whose file has gone is a miss and is dropped", "[image][thumb]") {
  temp_cache tc;
  mv::image::thumb_store store;
  REQUIRE(store.open(tc.dir.string()));
  const auto key = key_n(2);
  auto stored = store.store(key, kJpeg);
  REQUIRE(stored);
  fs::remove(stored.value());

  // The unverified path still names it: its reader's failed read is the check.
  auto unverified = store.lookup_unverified(key);
  REQUIRE(unverified);
  CHECK(unverified.value() == stored.value());

  auto verified = store.lookup(key);
  REQUIRE(verified);
  CHECK(verified.value().empty());
  // ...and the verified miss dropped the row.
  auto after = store.lookup_unverified(key);
  REQUIRE(after);
  CHECK(after.value().empty());
}

TEST_CASE("thumb store: forget drops the row", "[image][thumb]") {
  temp_cache tc;
  mv::image::thumb_store store;
  REQUIRE(store.open(tc.dir.string()));
  const auto key = key_n(3);
  REQUIRE(store.store(key, kJpeg));
  store.forget(key);
  auto after = store.lookup_unverified(key);
  REQUIRE(after);
  CHECK(after.value().empty());
}

TEST_CASE("thumb store: rows survive a close and reopen; reopening the same dir is a no-op",
          "[image][thumb]") {
  temp_cache tc;
  const auto key = key_n(4);
  std::string path;
  {
    mv::image::thumb_store store;
    REQUIRE(store.open(tc.dir.string()));
    auto stored = store.store(key, kJpeg);
    REQUIRE(stored);
    path = stored.value();
    REQUIRE(store.open(tc.dir.string()));  // same dir: keeps the connection
    auto hit = store.lookup(key);
    REQUIRE(hit);
    CHECK(hit.value() == path);
    store.close();
    CHECK(!store.is_open());
    CHECK(!store.lookup(key));  // closed is an error, not a miss
  }
  mv::image::thumb_store again;
  REQUIRE(again.open(tc.dir.string()));
  auto hit = again.lookup(key);
  REQUIRE(hit);
  CHECK(hit.value() == path);
}

TEST_CASE("thumb store: lookups and stores from many threads", "[image][thumb]") {
  temp_cache tc;
  mv::image::thumb_store store;
  REQUIRE(store.open(tc.dir.string()));
  constexpr int kThreads = 4;
  constexpr int kPerThread = 50;
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        const auto key = key_n(t * kPerThread + i);
        if (!store.store(key, kJpeg)) ++failures;
        auto hit = store.lookup(key);
        if (!hit || hit.value().empty()) ++failures;
        auto miss = store.lookup(key_n(100000 + t * kPerThread + i));
        if (!miss || !miss.value().empty()) ++failures;
      }
    });
  }
  for (auto& th : threads) th.join();
  CHECK(failures.load() == 0);
}

// Issue #168's cost: the lookups a sweep makes over a cached folder, four pool
// threads at once. Not part of the normal run: mv_tests "[.perf-bench]".
TEST_CASE("thumb store lookup throughput", "[.perf-bench]") {
  temp_cache tc;
  mv::image::thumb_store store;
  REQUIRE(store.open(tc.dir.string()));
  constexpr int kItems = 2000;
  for (int i = 0; i < kItems; ++i) REQUIRE(store.store(key_n(i), kJpeg));

  constexpr int kThreads = 4;
  constexpr int kRounds = 15;
  auto run = [&](auto&& look) {
    std::vector<double> ms;
    for (int r = 0; r < kRounds; ++r) {
      const auto t0 = std::chrono::steady_clock::now();
      std::vector<std::thread> threads;
      for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
          for (int i = t; i < kItems; i += kThreads) (void)look(key_n(i));
        });
      }
      for (auto& th : threads) th.join();
      ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                       .count());
    }
    std::sort(ms.begin(), ms.end());
    return ms[ms.size() / 2];
  };
  const double verified = run([&](const mv::image::thumb_key& k) { return store.lookup(k); });
  const double unverified =
      run([&](const mv::image::thumb_key& k) { return store.lookup_unverified(k); });
  std::printf("thumb_store: %d hits on %d threads, median of %d: lookup %.2f ms, "
              "lookup_unverified %.2f ms\n",
              kItems, kThreads, kRounds, verified, unverified);
  CHECK(verified > 0.0);
}
