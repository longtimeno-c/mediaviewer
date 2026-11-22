// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// shell/browse_history.h: Back and Forward through folders, with the selection
// each folder had when it was left.
#include <catch2/catch_test_macros.hpp>

#include "shell/browse_history.h"

using mv::shell::browse_history;
using mv::shell::browse_place;

TEST_CASE("browse_history: back returns the folder left, with its selection", "[browse_history]") {
  browse_history h;
  CHECK_FALSE(h.can_back());
  h.visit({"/p", "/p/a.jpg", "/p/2024"}, "/p/2024");
  REQUIRE(h.can_back());
  CHECK_FALSE(h.can_forward());

  const auto back = h.back({"/p/2024", "/p/2024/x.jpg", ""});
  REQUIRE(back);
  CHECK(back->dir == "/p");
  CHECK(back->item == "/p/a.jpg");
  CHECK(back->folder == "/p/2024");  // the tile that was opened stays selected
  CHECK_FALSE(h.can_back());
  REQUIRE(h.can_forward());

  const auto fwd = h.forward({"/p", "/p/a.jpg", "/p/2024"});
  REQUIRE(fwd);
  CHECK(fwd->dir == "/p/2024");
  CHECK(fwd->item == "/p/2024/x.jpg");
  CHECK(h.can_back());
  CHECK_FALSE(h.can_forward());
}

TEST_CASE("browse_history: a new folder after Back clears Forward", "[browse_history]") {
  browse_history h;
  h.visit({"/a", "", ""}, "/b");
  h.visit({"/b", "", ""}, "/c");
  REQUIRE(h.back({"/c", "", ""})->dir == "/b");
  REQUIRE(h.can_forward());
  h.visit({"/b", "", ""}, "/d");
  CHECK_FALSE(h.can_forward());
  CHECK(h.back({"/d", "", ""})->dir == "/b");
  CHECK(h.back({"/b", "", ""})->dir == "/a");
  CHECK_FALSE(h.back({"/a", "", ""}).has_value());
}

TEST_CASE("browse_history: nothing open, the same folder, and trailing separators are no move",
          "[browse_history]") {
  browse_history h;
  h.visit({"", "", ""}, "/a");
  CHECK_FALSE(h.can_back());
  h.visit({"/a", "", ""}, "/a/");
  CHECK_FALSE(h.can_back());
  // An empty stack hands nothing back and records nothing.
  CHECK_FALSE(h.back({"/a", "", ""}).has_value());
  CHECK_FALSE(h.can_forward());
}

TEST_CASE("browse_history: walking back and forth by hand stays one entry per folder", "[browse_history]") {
  browse_history h;
  h.visit({"/a", "", ""}, "/b");
  h.visit({"/b", "", ""}, "/a");
  h.visit({"/a", "1", ""}, "/b");
  // /a, /b, /a would be three; the repeated top folder is kept once, newest selection.
  CHECK(h.back_depth() == 3);
  h.visit({"/b", "", ""}, "/b/c");
  CHECK(h.back_depth() == 4);
  h.clear();
  h.visit({"/x", "first", ""}, "/y");
  h.visit({"/x", "second", ""}, "/z");  // the same folder pushed twice in a row
  CHECK(h.back_depth() == 1);
  CHECK(h.back({"/z", "", ""})->item == "second");
}

TEST_CASE("browse_history: the oldest entries drop off past the cap", "[browse_history]") {
  browse_history h;
  for (std::size_t i = 0; i < browse_history::kMaxDepth + 10; ++i) {
    h.visit({"/f" + std::to_string(i), "", ""}, "/f" + std::to_string(i + 1));
  }
  CHECK(h.back_depth() == browse_history::kMaxDepth);
  std::string last;
  std::string here = "/f" + std::to_string(browse_history::kMaxDepth + 10);
  while (auto p = h.back({here, "", ""})) {
    last = p->dir;
    here = p->dir;
  }
  CHECK(last == "/f10");
}

TEST_CASE("browse_history: forget drops a folder that went, and everything inside it", "[browse_history]") {
  browse_history h;
  h.visit({"/a", "", ""}, "/a/b");
  h.visit({"/a/b", "", ""}, "/a/b/c");
  h.visit({"/a/b/c", "", ""}, "/z");
  h.forget("/a/b");
  CHECK(h.back_depth() == 1);
  CHECK(h.back({"/z", "", ""})->dir == "/a");
}

TEST_CASE("browse_history: Windows paths step back the same way", "[browse_history]") {
  browse_history h;
  h.visit({"C:\\Photos", "", "C:\\Photos\\2024"}, "C:\\Photos\\2024");
  h.visit({"C:\\Photos\\2024", "", ""}, "C:\\Photos\\2024\\06");
  h.forget("C:\\Photos\\2024");
  REQUIRE(h.back_depth() == 1);
  CHECK(h.back({"C:\\", "", ""})->folder == "C:\\Photos\\2024");
}
