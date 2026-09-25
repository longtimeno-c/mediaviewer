// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "shell/marks.h"

using mv::shell::mark_set;

TEST_CASE("marks toggle by path and survive a re-sort", "[shell][marks]") {
  mark_set marks;
  REQUIRE(marks.toggle("C:\\dump\\a.jpg"));
  REQUIRE(marks.toggle("C:\\dump\\c.jpg"));
  REQUIRE(marks.contains("C:\\dump\\a.jpg"));
  REQUIRE_FALSE(marks.contains("C:\\dump\\b.jpg"));
  REQUIRE_FALSE(marks.toggle("C:\\dump\\a.jpg"));  // toggled back off
  REQUIRE(marks.size() == 1);
  REQUIRE_FALSE(marks.toggle(""));  // nothing selected is not a mark
  REQUIRE(marks.size() == 1);
}

TEST_CASE("F7 / F8 / Delete act on the marks, else the current item", "[shell][marks]") {
  mark_set marks;
  REQUIRE(marks.targets("C:\\dump\\cur.jpg") == std::vector<std::string>{"C:\\dump\\cur.jpg"});
  REQUIRE(marks.targets("").empty());

  const std::vector<std::string> folder = {"C:\\dump\\b.jpg", "C:\\dump\\a.jpg", ""};
  marks.mark_all(folder);
  REQUIRE(marks.size() == 2);
  const auto t = marks.targets("C:\\dump\\cur.jpg");
  REQUIRE(t.size() == 2);  // the current item is not added to marked targets
  REQUIRE(t[0] == "C:\\dump\\a.jpg");

  marks.erase("C:\\dump\\a.jpg");  // it succeeded: its mark clears, the other stays
  REQUIRE(marks.targets({}) == std::vector<std::string>{"C:\\dump\\b.jpg"});
  marks.clear();
  REQUIRE(marks.empty());
}
