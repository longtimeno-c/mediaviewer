// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #299: the host's read of the AI pack's resume.json (addon/ai_resume.h).
#include "catch_compat.h"

#include "addon/ai_resume.h"

#include <filesystem>
#include <fstream>
#include <string>

using mv::addon::parse_ai_resume;
using mv::addon::resume_question;
using mv::addon::unfinished_root;

TEST_CASE("resume.json names the unfinished roots for the launch notice", "[addon][ai-resume]") {
  const auto roots = parse_ai_resume(
      R"({"version":1,"roots":[{"id":3,"path":"photos:library","kind":"photos","done":120,"total":9000,"held":true},)"
      R"({"id":5,"path":"/Users/me/Pictures/Holiday 2026/","kind":"folder","done":0,"total":40,"held":false}]})");
  REQUIRE(roots.size() == 2);
  CHECK(roots[0].id == 3);
  CHECK(roots[0].photos);
  CHECK(roots[0].done == 120);
  CHECK(roots[0].total == 9000);
  CHECK_FALSE(roots[1].photos);
  CHECK(mv::addon::unfinished_name(roots[0]) == "Photos");
  CHECK(mv::addon::unfinished_name(roots[1]) == "Holiday 2026");
  CHECK(mv::addon::unfinished_name(unfinished_root{.id = 1, .path = "C:\\Media\\Cards"}) == "Cards");

  CHECK(resume_question({}).empty());
  CHECK(resume_question({roots[0]}) == "Resume indexing Photos?");
  CHECK(resume_question(roots) == "Resume indexing Photos and Holiday 2026?");
  auto four = roots;
  four.push_back(unfinished_root{.id = 6, .path = "/a/b"});
  four.push_back(unfinished_root{.id = 7, .path = "/a/c"});
  CHECK(resume_question(four) == "Resume indexing Photos, Holiday 2026 and 2 more?");
}

TEST_CASE("resume.json that is missing, empty or not version 1 asks nothing", "[addon][ai-resume]") {
  CHECK(parse_ai_resume("").empty());
  CHECK(parse_ai_resume("{").empty());
  CHECK(parse_ai_resume(R"({"version":1,"roots":[]})").empty());
  CHECK(parse_ai_resume(R"({"version":2,"roots":[{"id":1,"path":"/a","kind":"folder"}]})").empty());
  // A row without an id or a path is skipped, the rest kept.
  const auto some = parse_ai_resume(
      R"({"version":1,"roots":[{"path":"/a"},{"id":2,"path":""},{"id":4,"path":"/b","kind":"folder"}]})");
  REQUIRE(some.size() == 1);
  CHECK(some[0].id == 4);

  const auto dir = std::filesystem::temp_directory_path() / "mv_ai_resume_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  CHECK(mv::addon::read_ai_resume(dir.string()).empty());  // none written yet
  {
    std::ofstream out(dir / "resume.json", std::ios::binary);
    out << R"({"version":1,"roots":[{"id":9,"path":"/x/Cards","kind":"folder","done":1,"total":2,"held":true}]})";
  }
  const auto read = mv::addon::read_ai_resume(dir.string());
  REQUIRE(read.size() == 1);
  CHECK(read[0].id == 9);
  std::filesystem::remove_all(dir);
}
