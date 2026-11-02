// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// image::thumb_store: the JPEG-512 cache's SQLite index (image/thumb_store.cpp).
// Issue #168: statements prepared once, no stat or write under the lock, and a
// reader's failed read is the miss. #612: the sweep, stale stamps and Clear.
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

#include <sqlite3.h>

#include "core/job_system.h"
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

// ---- #612: the self-tidying cache (sweep, store's stale stamps, Clear) -----------------

namespace {

std::atomic<mv::generation> g_view_gen{1};

mv::job_context background_ctx() {
  return mv::job_context(1, mv::background_generation, &g_view_gen, 0);
}

// A row as an older build wrote it: another spec, its own file name.
void insert_old_spec_row(const fs::path& dir, const std::string& path, const std::string& file,
                         const char* spec = "jpg512.3") {
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open((dir / "thumbs.sqlite").string().c_str(), &db) == SQLITE_OK);
  const std::string sql = "INSERT INTO thumbs(path,mtime,size,spec,file) VALUES('" + path +
                          "',1,2,'" + spec + "','" + file + "');";
  CHECK(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(db);
}

int row_count(const fs::path& dir) {
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open((dir / "thumbs.sqlite").string().c_str(), &db) == SQLITE_OK);
  sqlite3_stmt* q = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM thumbs;", -1, &q, nullptr) == SQLITE_OK);
  int n = -1;
  if (sqlite3_step(q) == SQLITE_ROW) n = sqlite3_column_int(q, 0);
  sqlite3_finalize(q);
  sqlite3_close(db);
  return n;
}

void write_file(const fs::path& p, std::size_t bytes) {
  std::FILE* f = std::fopen(p.string().c_str(), "wb");
  REQUIRE(f != nullptr);
  const std::vector<std::uint8_t> data(bytes, 0xAB);
  std::fwrite(data.data(), 1, data.size(), f);
  std::fclose(f);
}

void age(const fs::path& p, std::chrono::seconds by) {
  fs::last_write_time(p, fs::file_time_type::clock::now() - by);
}

mv::result<mv::image::thumb_sweep_stats> sweep_all(mv::image::thumb_store& store,
                                                    std::uint64_t cap = 0,
                                                    const mv::image::sweep_pacer& pacer = {}) {
  mv::image::thumb_sweep st;
  for (int guard = 0; guard < 10000; ++guard) {
    auto r = store.sweep(st, cap, background_ctx(), pacer);
    if (!r) return mv::err(r.error());
    if (r.value()) return st.stats;
  }
  return mv::err(mv::status::internal);
}

}  // namespace

TEST_CASE("thumb store: cache file names", "[image][thumb]") {
  CHECK(mv::image::is_thumb_file_name("0123456789abcdef.jpg"));
  CHECK_FALSE(mv::image::is_thumb_file_name("0123456789ABCDEF.jpg"));
  CHECK_FALSE(mv::image::is_thumb_file_name("thumbs.sqlite"));
  CHECK_FALSE(mv::image::is_thumb_file_name("meta_cache.db"));
  CHECK_FALSE(mv::image::is_thumb_file_name("0123456789abcdef.jpeg"));
  CHECK_FALSE(mv::image::is_thumb_file_name("0123456789abcde.jpg"));
}

TEST_CASE("thumb store: a sweep drops an old spec's rows and files, keeps the current ones",
          "[image][thumb]") {
  temp_cache tc;
  mv::image::thumb_store store;
  REQUIRE(store.open(tc.dir.string()));
  auto kept = store.store(key_n(1), kJpeg);
  REQUIRE(kept);
  insert_old_spec_row(tc.dir, "/photos/IMG_1.JPG", "00000000000000aa.jpg");
  write_file(tc.dir / "00000000000000aa.jpg", 100);
  insert_old_spec_row(tc.dir, "/photos/IMG_2.JPG", "00000000000000bb.jpg", "jpg512.1");
  write_file(tc.dir / "00000000000000bb.jpg", 50);
  REQUIRE(row_count(tc.dir) == 3);

  auto stats = sweep_all(store);
  REQUIRE(stats);
  CHECK(stats.value().rows_removed == 2);
  CHECK(stats.value().files_removed == 2);
  CHECK(stats.value().bytes_removed == 150);
  CHECK(stats.value().files_kept == 1);
  CHECK(stats.value().bytes_kept == kJpeg.size());
  CHECK(row_count(tc.dir) == 1);
  CHECK_FALSE(fs::exists(tc.dir / "00000000000000aa.jpg"));
  CHECK_FALSE(fs::exists(tc.dir / "00000000000000bb.jpg"));
  CHECK(fs::exists(kept.value()));
  auto hit = store.lookup(key_n(1));
  REQUIRE(hit);
  CHECK(hit.value() == kept.value());
  bool known = false;
  CHECK(store.total_bytes(&known) == kJpeg.size());
  CHECK(known);
}

TEST_CASE("thumb store: a sweep drops rows whose JPEG is gone and old unnamed JPEGs",
          "[image][thumb]") {
  temp_cache tc;
  mv::image::thumb_store store;
  REQUIRE(store.open(tc.dir.string()));
  auto gone = store.store(key_n(1), kJpeg);
  REQUIRE(gone);
  REQUIRE(store.store(key_n(2), kJpeg));
  fs::remove(gone.value());
  // No row names these. The old one goes; the young one may be a store()
  // between its file and its row, and stays.
  write_file(tc.dir / "1111111111111111.jpg", 10);
  age(tc.dir / "1111111111111111.jpg", std::chrono::hours(2));
  write_file(tc.dir / "2222222222222222.jpg", 10);
  // Not the cache's: never touched, however old.
  write_file(tc.dir / "meta_cache.db", 10);
  age(tc.dir / "meta_cache.db", std::chrono::hours(2));
  write_file(tc.dir / "notes.txt", 10);
  age(tc.dir / "notes.txt", std::chrono::hours(2));

  auto stats = sweep_all(store);
  REQUIRE(stats);
  CHECK(stats.value().rows_removed == 1);
  CHECK(stats.value().files_removed == 1);
  CHECK(stats.value().files_kept == 1);
  CHECK(row_count(tc.dir) == 1);
  CHECK_FALSE(fs::exists(tc.dir / "1111111111111111.jpg"));
  CHECK(fs::exists(tc.dir / "2222222222222222.jpg"));
  CHECK(fs::exists(tc.dir / "meta_cache.db"));
  CHECK(fs::exists(tc.dir / "notes.txt"));
  CHECK(fs::exists(tc.dir / "thumbs.sqlite"));
}

TEST_CASE("thumb store: storing a new stamp drops the path's old stamps and their files",
          "[image][thumb]") {
  temp_cache tc;
  mv::image::thumb_store store;
  REQUIRE(store.open(tc.dir.string()));
  const auto before = key_n(5);
  auto old_file = store.store(before, kJpeg);
  REQUIRE(old_file);
  auto other_path = store.store(key_n(6), kJpeg);
  REQUIRE(other_path);
  // A clip moment of the same file is its own path: untouched.
  const auto moment = mv::image::moment_thumb_key(before.path, 1500, before.mtime_unix, before.size);
  auto moment_file = store.store(moment, kJpeg);
  REQUIRE(moment_file);

  // The file was edited: a new stamp, a miss, a store.
  const mv::image::thumb_key after{before.path, before.mtime_unix + 60, before.size + 7};
  auto miss = store.lookup(after);
  REQUIRE(miss);
  CHECK(miss.value().empty());
  auto new_file = store.store(after, kJpeg);
  REQUIRE(new_file);
  CHECK(new_file.value() != old_file.value());
  CHECK_FALSE(fs::exists(old_file.value()));
  auto old_row = store.lookup_unverified(before);
  REQUIRE(old_row);
  CHECK(old_row.value().empty());
  CHECK(fs::exists(new_file.value()));
  CHECK(fs::exists(other_path.value()));
  CHECK(fs::exists(moment_file.value()));
  CHECK(row_count(tc.dir) == 3);
}

TEST_CASE("thumb store: a cap drops the oldest-written JPEGs; no cap drops none",
          "[image][thumb]") {
  temp_cache tc;
  mv::image::thumb_store store;
  REQUIRE(store.open(tc.dir.string()));
  const std::vector<std::uint8_t> big(1000, 0x5A);
  std::vector<std::string> files;
  for (int i = 0; i < 5; ++i) {
    auto stored = store.store(key_n(i), big);
    REQUIRE(stored);
    files.push_back(stored.value());
    age(stored.value(), std::chrono::hours(10 - i));  // 0 is the oldest
  }
  auto uncapped = sweep_all(store, 0);
  REQUIRE(uncapped);
  CHECK(uncapped.value().files_kept == 5);
  CHECK(uncapped.value().rows_removed == 0);

  auto capped = sweep_all(store, 2500);
  REQUIRE(capped);
  CHECK(capped.value().bytes_kept <= 2500);
  CHECK(capped.value().files_kept == 2);
  CHECK(capped.value().rows_removed == 3);
  for (int i = 0; i < 3; ++i) CHECK_FALSE(fs::exists(files[i]));
  for (int i = 3; i < 5; ++i) CHECK(fs::exists(files[i]));
  CHECK(row_count(tc.dir) == 2);
}

TEST_CASE("thumb store: a sweep that steps aside resumes where it was; stop ends it",
          "[image][thumb]") {
  temp_cache tc;
  mv::image::thumb_store store;
  REQUIRE(store.open(tc.dir.string()));
  for (int i = 0; i < 600; ++i) REQUIRE(store.store(key_n(i), kJpeg));
  for (int i = 0; i < 300; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "%016x.jpg", 0x100000 + i);
    insert_old_spec_row(tc.dir, "/old/" + std::to_string(i), name);
  }

  int asks = 0;
  int asides = 0;
  const mv::image::sweep_pacer every_third = [&] {
    ++asks;
    if (asks % 3 == 0) {
      ++asides;
      return mv::image::sweep_pace::step_aside;
    }
    return mv::image::sweep_pace::go;
  };
  auto stats = sweep_all(store, 0, every_third);
  REQUIRE(stats);
  CHECK(asides > 0);
  CHECK(stats.value().rows_removed == 300);
  CHECK(stats.value().files_kept == 600);
  CHECK(row_count(tc.dir) == 600);

  // stop_sweep (the host before its pool shuts down) ends a sweep at its next step.
  mv::image::thumb_sweep st;
  const mv::image::sweep_pacer stop_then_go = [&] {
    store.stop_sweep();
    return mv::image::sweep_pace::go;
  };
  auto stopped = store.sweep(st, 0, background_ctx(), stop_then_go);
  REQUIRE_FALSE(stopped);
  CHECK(stopped.error() == mv::status::cancelled);
}

TEST_CASE("thumb store: Clear empties the cache and leaves everything else",
          "[image][thumb]") {
  temp_cache tc;
  mv::image::thumb_store store;
  REQUIRE(store.open(tc.dir.string()));
  std::vector<std::string> files;
  for (int i = 0; i < 20; ++i) {
    auto stored = store.store(key_n(i), kJpeg);
    REQUIRE(stored);
    files.push_back(stored.value());
  }
  write_file(tc.dir / "meta_cache.db", 64);
  write_file(tc.dir / "notes.txt", 8);
  REQUIRE(store.clear());
  CHECK(row_count(tc.dir) == 0);
  for (const auto& f : files) CHECK_FALSE(fs::exists(f));
  CHECK(fs::exists(tc.dir / "meta_cache.db"));
  CHECK(fs::exists(tc.dir / "notes.txt"));
  CHECK(fs::exists(tc.dir / "thumbs.sqlite"));
  bool known = false;
  CHECK(store.total_bytes(&known) == 0);
  CHECK(known);

  // The next visit regenerates: a miss, then a store that is hit again.
  auto miss = store.lookup(key_n(3));
  REQUIRE(miss);
  CHECK(miss.value().empty());
  auto again = store.store(key_n(3), kJpeg);
  REQUIRE(again);
  auto hit = store.lookup(key_n(3));
  REQUIRE(hit);
  CHECK(hit.value() == again.value());
}

TEST_CASE("thumb store: a queued sweep runs on the pool and reports once", "[image][thumb]") {
  temp_cache tc;
  mv::image::thumb_store store;
  REQUIRE(store.open(tc.dir.string()));
  for (int i = 0; i < 50; ++i) REQUIRE(store.store(key_n(i), kJpeg));
  insert_old_spec_row(tc.dir, "/old/a", "000000000000abcd.jpg");
  write_file(tc.dir / "000000000000abcd.jpg", 10);

  mv::job_system jobs;
  REQUIRE(jobs.start(2) == mv::status::ok);
  std::atomic<int> calls{0};
  std::atomic<std::uint64_t> removed{0};
  std::atomic<int> busy_asks{0};
  REQUIRE(store.submit_sweep(
      jobs, 0,
      // Busy for the first few asks (the viewer panning): the pass waits.
      [&] { return busy_asks.fetch_add(1) < 3; },
      [&](mv::result<mv::image::thumb_sweep_stats> r) {
        if (r) removed = r.value().rows_removed;
        ++calls;
      }));
  CHECK_FALSE(store.submit_sweep(jobs, 0, {}));  // one at a time
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (calls.load() == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  jobs.shutdown();
  CHECK(calls.load() == 1);
  CHECK(removed.load() == 1);
  CHECK(busy_asks.load() >= 3);
  CHECK_FALSE(store.sweep_running());
  CHECK_FALSE(fs::exists(tc.dir / "000000000000abcd.jpg"));
}

// #612's verify, the core half: a 10,000-file cache after a spec bump. The
// sweep reclaims every old-spec file; the current spec's lookups stay hits.
// Not part of the normal run: mv_tests "[.perf-bench]".
TEST_CASE("thumb store sweep after a spec bump", "[.perf-bench]") {
  temp_cache tc;
  mv::image::thumb_store store;
  REQUIRE(store.open(tc.dir.string()));
  constexpr int kOld = 10000;
  constexpr int kCurrent = 1000;
  const std::vector<std::uint8_t> jpeg(20000, 0x5A);  // a 512-px thumb's order of size
  {
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open((tc.dir / "thumbs.sqlite").string().c_str(), &db) == SQLITE_OK);
    sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
    for (int i = 0; i < kOld; ++i) {
      char name[32];
      std::snprintf(name, sizeof(name), "%016x.jpg", 0x200000 + i);
      write_file(tc.dir / name, jpeg.size());
      const std::string sql = "INSERT INTO thumbs(path,mtime,size,spec,file) VALUES('/p/" +
                              std::to_string(i) + "',1,2,'jpg512.3','" + name + "');";
      sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    }
    sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
    sqlite3_close(db);
  }
  for (int i = 0; i < kCurrent; ++i) REQUIRE(store.store(key_n(i), jpeg));
  const auto t0 = std::chrono::steady_clock::now();
  auto stats = sweep_all(store);
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  REQUIRE(stats);
  std::printf("thumb_store sweep: %d old-spec rows + %d current: %.1f ms, removed %llu rows, "
              "%llu files, %.1f MB; kept %llu files\n",
              kOld, kCurrent, ms, static_cast<unsigned long long>(stats.value().rows_removed),
              static_cast<unsigned long long>(stats.value().files_removed),
              static_cast<double>(stats.value().bytes_removed) / (1024.0 * 1024.0),
              static_cast<unsigned long long>(stats.value().files_kept));
  CHECK(stats.value().rows_removed == kOld);
  CHECK(stats.value().files_kept == kCurrent);
  for (int i = 0; i < kCurrent; ++i) {
    auto hit = store.lookup(key_n(i));
    REQUIRE(hit);
    CHECK_FALSE(hit.value().empty());
  }
}
