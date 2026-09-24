// SPDX-License-Identifier: GPL-2.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "io/dir.h"

namespace fs = std::filesystem;
using namespace mv::io;

namespace {

struct temp_tree {
  fs::path root;
  temp_tree() {
    root = fs::temp_directory_path() / ("mv_dir_tree_" + std::to_string(std::random_device{}()));
    fs::create_directories(root);
  }
  ~temp_tree() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  void dir(const char* rel) const { fs::create_directories(root / rel); }
  void file(const char* rel) const {
    fs::create_directories((root / rel).parent_path());
    std::ofstream(root / rel) << "x";
  }
  [[nodiscard]] std::string str() const { return root.string(); }
};

}  // namespace

TEST_CASE("less_natural: digit runs compare by value", "[dir_tree]") {
  CHECK(less_natural("Trip 2", "Trip 10"));
  CHECK_FALSE(less_natural("Trip 10", "Trip 2"));
  CHECK(less_natural("2019", "2020"));
  CHECK(less_natural("06", "10"));
  CHECK(less_natural("a", "B"));
  CHECK_FALSE(less_natural("same", "same"));
  // Total order even when values tie: neither may be "less" both ways.
  CHECK(less_natural("01", "1") != less_natural("1", "01"));
}

TEST_CASE("is_housekeeping_dir: NAS and OS folders are not albums", "[dir_tree]") {
  CHECK(is_housekeeping_dir("@eaDir"));
  CHECK(is_housekeeping_dir("#recycle"));
  CHECK(is_housekeeping_dir("$RECYCLE.BIN"));
  CHECK(is_housekeeping_dir(".Trashes"));
  CHECK(is_housekeeping_dir("__MACOSX"));
  CHECK_FALSE(is_housekeeping_dir("2024"));
  CHECK_FALSE(is_housekeeping_dir("Holiday"));
}

TEST_CASE("list_subfolders: natural order, housekeeping dropped, files ignored", "[dir_tree]") {
  temp_tree t;
  t.dir("Trip 10");
  t.dir("Trip 2");
  t.dir("2019");
  t.dir("@eaDir");
  t.dir(".hidden");
  t.file("loose.jpg");

  auto subs = list_subfolders(t.str());
  REQUIRE(subs);
  REQUIRE(subs.value().size() == 3);
  CHECK(subs.value()[0].name_utf8 == "2019");
  CHECK(subs.value()[1].name_utf8 == "Trip 2");
  CHECK(subs.value()[2].name_utf8 == "Trip 10");
  CHECK(fs::path(subs.value()[2].path_utf8) == t.root / "Trip 10");
}

TEST_CASE("list_subfolders: a missing folder is an error, not empty", "[dir_tree]") {
  CHECK_FALSE(list_subfolders("/definitely/not/here"));
}

TEST_CASE("summarize_dir: own media is the cover", "[dir_tree]") {
  temp_tree t;
  t.file("b.jpg");
  t.file("a.png");
  t.file("notes.txt");
  t.dir("sub");

  auto s = summarize_dir(t.str());
  REQUIRE(s);
  CHECK(s.value().media_count == 2);
  CHECK(s.value().subdir_count == 1);
  REQUIRE(s.value().has_cover);
  CHECK(s.value().cover.name_utf8 == "a.png");
}

TEST_CASE("summarize_dir: a year folder borrows a cover from its months", "[dir_tree]") {
  temp_tree t;
  t.dir("01");
  t.file("02/x.jpg");
  t.file("03/y.jpg");

  auto s = summarize_dir(t.str());
  REQUIRE(s);
  CHECK(s.value().media_count == 0);
  CHECK(s.value().subdir_count == 3);
  REQUIRE(s.value().has_cover);
  CHECK(s.value().cover.name_utf8 == "x.jpg");
}

TEST_CASE("summarize_dir: depth and visit budgets bound the walk", "[dir_tree]") {
  temp_tree t;
  t.file("a/b/c/d/e/deep.jpg");

  auto shallow = summarize_dir(t.str(), /*max_depth=*/2);
  REQUIRE(shallow);
  CHECK_FALSE(shallow.value().has_cover);

  auto deep = summarize_dir(t.str(), /*max_depth=*/8);
  REQUIRE(deep);
  CHECK(deep.value().has_cover);

  temp_tree wide;
  for (int i = 0; i < 10; ++i) wide.dir(("d" + std::to_string(100 + i)).c_str());
  wide.file("zzz/last.jpg");
  auto capped = summarize_dir(wide.str(), 3, /*max_visits=*/4);
  REQUIRE(capped);
  CHECK_FALSE(capped.value().has_cover);
}

TEST_CASE("summarize_dir: empty folder", "[dir_tree]") {
  temp_tree t;
  auto s = summarize_dir(t.str());
  REQUIRE(s);
  CHECK(s.value().media_count == 0);
  CHECK(s.value().subdir_count == 0);
  CHECK_FALSE(s.value().has_cover);
}
