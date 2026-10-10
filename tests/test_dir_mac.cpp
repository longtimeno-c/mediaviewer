// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// io/dir_mac list_still_files (issue #167): one getattrlistbulk pass gives each
// file's size and mtime; only a symlink is stat'ed, and through to its target,
// because the thumbnail cache keys on path + size + mtime.
#include "catch_compat.h"

#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "io/dir.h"

namespace fs = std::filesystem;

namespace {

struct temp_dir {
  fs::path root;
  temp_dir() {
    root = fs::temp_directory_path() / ("mv_dir_mac_" + std::to_string(std::random_device{}()));
    fs::create_directories(root);
  }
  ~temp_dir() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  [[nodiscard]] std::string path(const char* name) const { return (root / name).string(); }
  void file(const char* name, std::size_t bytes) const {
    std::ofstream(root / name, std::ios::binary) << std::string(bytes, 'x');
  }
};

void set_mtime(const std::string& path, std::int64_t unix_seconds) {
  const timeval tv[2] = {{static_cast<time_t>(unix_seconds), 0}, {static_cast<time_t>(unix_seconds), 0}};
  REQUIRE(::utimes(path.c_str(), tv) == 0);
}

}  // namespace

TEST_CASE("list_still_files (mac): media only, size and mtime from the listing", "[dir]") {
  temp_dir t;
  t.file("b.JPG", 5);
  t.file("a.png", 3);
  t.file("notes.txt", 1);
  t.file(".hidden.jpg", 1);
  t.file("._a.png", 1);
  t.file("Thumbs.db", 1);
  t.file("flagged.jpg", 1);
  REQUIRE(::chflags(t.path("flagged.jpg").c_str(), UF_HIDDEN) == 0);
  fs::create_directories(t.root / "folder.jpg");
  set_mtime(t.path("a.png"), 1'600'000'000);

  auto listed = mv::io::list_still_files(t.root.string());
  REQUIRE(listed);
  REQUIRE(listed->size() == 2);
  CHECK(listed.value()[0].name_utf8 == "a.png");
  CHECK(listed.value()[0].path_utf8 == t.path("a.png"));
  CHECK(listed.value()[0].size == 3);
  CHECK(listed.value()[0].mtime_unix == 1'600'000'000);
  CHECK(listed.value()[1].name_utf8 == "b.JPG");
  CHECK(listed.value()[1].size == 5);
}

TEST_CASE("list_still_files (mac): a symlink lists its target's size and mtime", "[dir]") {
  temp_dir t;
  t.file("target.bin", 7);
  set_mtime(t.path("target.bin"), 1'500'000'000);
  fs::create_directories(t.root / "sub");
  REQUIRE(::symlink(t.path("target.bin").c_str(), t.path("link.jpg").c_str()) == 0);
  REQUIRE(::symlink(t.path("missing.bin").c_str(), t.path("dangling.jpg").c_str()) == 0);
  REQUIRE(::symlink(t.path("sub").c_str(), t.path("dirlink.jpg").c_str()) == 0);

  auto listed = mv::io::list_still_files(t.root.string());
  REQUIRE(listed);
  REQUIRE(listed->size() == 1);
  CHECK(listed.value()[0].name_utf8 == "link.jpg");
  CHECK(listed.value()[0].size == 7);
  CHECK(listed.value()[0].mtime_unix == 1'500'000'000);
}

TEST_CASE("list_still_files (mac): more entries than one bulk buffer", "[dir]") {
  temp_dir t;
  // 64 KiB holds a few hundred entries; 1500 forces several getattrlistbulk calls.
  for (int i = 0; i < 1500; ++i) {
    const std::string name = "IMG_" + std::to_string(10000 + i) + ".jpg";
    t.file(name.c_str(), 1);
  }
  auto listed = mv::io::list_still_files(t.root.string());
  REQUIRE(listed);
  REQUIRE(listed->size() == 1500);
  CHECK(listed.value().front().name_utf8 == "IMG_10000.jpg");
  CHECK(listed.value().back().name_utf8 == "IMG_11499.jpg");
}

TEST_CASE("list_still_files (mac): a missing folder is an error", "[dir]") {
  temp_dir t;
  CHECK_FALSE(mv::io::list_still_files(t.path("nope")));
}
