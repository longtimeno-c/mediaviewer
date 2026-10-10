// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// io/duplicate.h: File > Duplicate (issue #289, docs/design/16 "Marks, copy, move").
#include "catch_compat.h"

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "import_fixture.h"
#include "io/duplicate.h"
#include "io/file_port.h"

using namespace mv::test;
using mv::io::duplicate_name;
using mv::io::duplicate_style;

TEST_CASE("duplicate names follow Finder and Explorer", "[io][duplicate]") {
  REQUIRE(duplicate_name("IMG_0001.JPG", 1, duplicate_style::finder) == "IMG_0001 copy.JPG");
  REQUIRE(duplicate_name("IMG_0001.JPG", 2, duplicate_style::finder) == "IMG_0001 copy 2.JPG");
  REQUIRE(duplicate_name("IMG_0001.JPG", 1, duplicate_style::explorer) == "IMG_0001 - Copy.JPG");
  REQUIRE(duplicate_name("IMG_0001.JPG", 2, duplicate_style::explorer) == "IMG_0001 - Copy (2).JPG");
  REQUIRE(duplicate_name("archive.tar.gz", 1, duplicate_style::finder) == "archive.tar copy.gz");
  REQUIRE(duplicate_name("README", 3, duplicate_style::finder) == "README copy 3");
  REQUIRE(duplicate_name(".profile", 1, duplicate_style::explorer) == ".profile - Copy");
  REQUIRE(duplicate_name("\xE5\x86\x99\xE7\x9C\x9F.heic", 1, duplicate_style::finder) ==
          "\xE5\x86\x99\xE7\x9C\x9F copy.heic");
#if defined(_WIN32)
  REQUIRE(mv::io::native_duplicate_style() == duplicate_style::explorer);
#else
  REQUIRE(mv::io::native_duplicate_style() == duplicate_style::finder);
#endif
}

TEST_CASE("a group takes the first number free for every member", "[io][duplicate]") {
  std::set<std::string, std::less<>> taken;
  const auto exists = [&taken](std::string_view name) { return taken.count(name) != 0; };
  const std::vector<std::string> pair{"IMG_1.JPG", "IMG_1.CR2"};
  REQUIRE(mv::io::first_free_duplicate(pair, duplicate_style::finder, exists) == 1);
  // Only the RAW's copy name is taken: both move on, so they still pair.
  taken.insert("IMG_1 copy.CR2");
  REQUIRE(mv::io::first_free_duplicate(pair, duplicate_style::finder, exists) == 2);
  taken.insert("IMG_1 copy 2.JPG");
  REQUIRE(mv::io::first_free_duplicate(pair, duplicate_style::finder, exists) == 3);
  REQUIRE(mv::io::first_free_duplicate(pair, duplicate_style::finder, exists, 5) == 5);
  REQUIRE(mv::io::first_free_duplicate(pair, duplicate_style::finder, exists, 1, 2) == 0);
  REQUIRE(mv::io::first_free_duplicate(std::vector<std::string>{}, duplicate_style::finder, exists) == 0);
}

TEST_CASE("a duplicate lands beside the original and never overwrites", "[io][duplicate]") {
  scratch_dir s("dup");
  const auto bytes = pattern(300 * 1024 + 5, 7);
  write_bytes(s / "IMG_0001.JPG", bytes);
  set_mtime(s / "IMG_0001.JPG", 1790000000);
  const std::vector<std::string> one{utf8(s / "IMG_0001.JPG")};

  auto first = mv::io::duplicate_files(one, duplicate_style::finder);
  REQUIRE(first);
  REQUIRE(first->size() == 1);
  REQUIRE(first->front() == utf8(s / "IMG_0001 copy.JPG"));
  REQUIRE(read_bytes(s / "IMG_0001 copy.JPG") == bytes);
  auto st = mv::io::stat_path(first->front());
  REQUIRE(st);
  REQUIRE(st->mtime_unix == 1790000000);

  auto second = mv::io::duplicate_files(one, duplicate_style::finder);
  REQUIRE(second);
  REQUIRE(second->front() == utf8(s / "IMG_0001 copy 2.JPG"));
  // The original and the first copy are untouched (rule 5).
  REQUIRE(read_bytes(s / "IMG_0001.JPG") == bytes);
  REQUIRE(read_bytes(s / "IMG_0001 copy.JPG") == bytes);

  auto win = mv::io::duplicate_files(one, duplicate_style::explorer);
  REQUIRE(win);
  REQUIRE(win->front() == utf8(s / "IMG_0001 - Copy.JPG"));
  for (const std::string& rel : list_tree(s.root())) REQUIRE(rel.find(".mvtmp") == std::string::npos);
}

TEST_CASE("a pair and its sidecar are duplicated under one number", "[io][duplicate]") {
  scratch_dir s("dup_pair");
  write_bytes(s / "IMG_0002.JPG", pattern(4096, 1));
  write_bytes(s / "IMG_0002.CR2", pattern(8192, 2));
  write_text(s / "IMG_0002.xmp", "<x:xmpmeta/>");
  // A stray copy of the RAW alone: the JPEG must not take "copy" without it.
  write_bytes(s / "IMG_0002 copy.CR2", pattern(16, 3));

  const auto group = mv::io::duplicate_group(utf8(s / "IMG_0002.JPG"), utf8(s / "IMG_0002.CR2"));
  REQUIRE(group.size() == 3);
  REQUIRE(group[2] == utf8(s / "IMG_0002.xmp"));

  auto r = mv::io::duplicate_files(group, duplicate_style::finder);
  REQUIRE(r);
  REQUIRE(r->size() == 3);
  REQUIRE((*r)[0] == utf8(s / "IMG_0002 copy 2.JPG"));
  REQUIRE((*r)[1] == utf8(s / "IMG_0002 copy 2.CR2"));
  REQUIRE((*r)[2] == utf8(s / "IMG_0002 copy 2.xmp"));
  REQUIRE(read_bytes(s / "IMG_0002 copy 2.CR2") == pattern(8192, 2));
  REQUIRE(read_bytes(s / "IMG_0002 copy.CR2") == pattern(16, 3));
}

TEST_CASE("an item with no pair or sidecar is a group of one", "[io][duplicate]") {
  scratch_dir s("dup_alone");
  write_bytes(s / "clip.mov", pattern(1024, 4));
  const auto group = mv::io::duplicate_group(utf8(s / "clip.mov"), {});
  REQUIRE(group.size() == 1);
  REQUIRE(mv::io::duplicate_group({}, {}).empty());
}

TEST_CASE("a missing source duplicates nothing", "[io][duplicate]") {
  scratch_dir s("dup_missing");
  write_bytes(s / "a.jpg", pattern(64, 5));
  const std::vector<std::string> group{utf8(s / "a.jpg"), utf8(s / "gone.cr2")};
  REQUIRE_FALSE(mv::io::duplicate_files(group, duplicate_style::finder));
  REQUIRE(list_tree(s.root()).size() == 1);
  REQUIRE_FALSE(mv::io::duplicate_files(std::vector<std::string>{}, duplicate_style::finder));
}
