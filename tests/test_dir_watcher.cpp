// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Issue #155: the folder watcher (ReadDirectoryChangesW on Windows, FSEvents on
// the Mac) is what relists a folder when a camera dump lands in it or a file is
// renamed beside the photo on screen. Portable: the same case on both hosts.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <system_error>
#include <thread>

#include "import_fixture.h"
#include "io/dir.h"

using namespace std::chrono_literals;

namespace {

void count_call(void* user) { static_cast<std::atomic<int>*>(user)->fetch_add(1); }

// Waits up to `limit` for the count to move past `from`.
bool changes_after(const std::atomic<int>& count, int from, std::chrono::milliseconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    if (count.load() > from) return true;
    std::this_thread::sleep_for(10ms);
  }
  return count.load() > from;
}

}  // namespace

TEST_CASE("the folder watcher reports a created and a renamed file, and nothing after stop",
          "[io][watcher]") {
  mv::test::scratch_dir dir("dir_watch");
  std::atomic<int> calls{0};
  mv::io::directory_watcher watcher;
  REQUIRE(watcher.start(mv::test::utf8(dir.root()), &count_call, &calls));

  // Created: a file landing in the folder (a camera dump, an export).
  int before = calls.load();
  mv::test::write_text(dir / "IMG_0001.JPG", "new");
  CHECK(changes_after(calls, before, 2s));

  // Renamed beside the photo on screen. Let the create's events drain first, so
  // the rename is what moves the count.
  std::this_thread::sleep_for(400ms);
  before = calls.load();
  std::error_code ec;
  std::filesystem::rename(dir / "IMG_0001.JPG", dir / "IMG_0002.JPG", ec);
  REQUIRE_FALSE(ec);
  CHECK(changes_after(calls, before, 2s));

  // stop() is a barrier: no callback runs after it returns, even for a change
  // made straight afterwards (the ABI stops the watcher, then frees the user).
  watcher.stop();
  before = calls.load();
  mv::test::write_text(dir / "IMG_0003.JPG", "after stop");
  std::filesystem::remove(dir / "IMG_0002.JPG", ec);
  std::this_thread::sleep_for(750ms);
  CHECK(calls.load() == before);

  // A second stop is harmless, and the watcher can watch again.
  watcher.stop();
  REQUIRE(watcher.start(mv::test::utf8(dir.root()), &count_call, &calls));
  before = calls.load();
  mv::test::write_text(dir / "IMG_0004.JPG", "again");
  CHECK(changes_after(calls, before, 2s));
  watcher.stop();
}

TEST_CASE("the folder watcher refuses an empty path or no callback", "[io][watcher]") {
  mv::test::scratch_dir dir("dir_watch_args");
  std::atomic<int> calls{0};
  mv::io::directory_watcher watcher;
  CHECK_FALSE(watcher.start("", &count_call, &calls));
  CHECK_FALSE(watcher.start(mv::test::utf8(dir.root()), nullptr, &calls));
  watcher.stop();
  CHECK(calls.load() == 0);
}
