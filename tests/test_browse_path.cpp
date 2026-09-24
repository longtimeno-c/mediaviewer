// SPDX-License-Identifier: GPL-2.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "shell/browse_path.h"

using mv::shell::browse_path;

TEST_CASE("browse_path: crumbs run from the root down to the current folder", "[browse_path]") {
  browse_path p;
  p.reset("/nas/photos");
  p.visit("/nas/photos/2024");
  p.visit("/nas/photos/2024/06");

  const auto crumbs = p.crumbs();
  REQUIRE(crumbs.size() == 3);
  CHECK(crumbs[0].name == "photos");
  CHECK(crumbs[0].path == "/nas/photos");
  CHECK(crumbs[1].name == "2024");
  CHECK(crumbs[2].name == "06");
  CHECK(crumbs[2].path == "/nas/photos/2024/06");
}

TEST_CASE("browse_path: going up past the root grows the trail upward", "[browse_path]") {
  browse_path p;
  p.reset("/nas/photos/2024");
  p.visit(p.parent());
  CHECK(p.current() == "/nas/photos");
  CHECK(p.root() == "/nas/photos");

  // Back down is still one click: the old root is now a level inside the trail.
  p.visit("/nas/photos/2024");
  const auto crumbs = p.crumbs();
  REQUIRE(crumbs.size() == 2);
  CHECK(crumbs[0].name == "photos");
  CHECK(crumbs[1].name == "2024");
}

TEST_CASE("browse_path: reset starts a new trail", "[browse_path]") {
  browse_path p;
  p.reset("/a/b");
  p.visit("/a/b/c");
  p.reset("/x/y");
  CHECK(p.crumbs().size() == 1);
  CHECK(p.current() == "/x/y");
}

TEST_CASE("browse_path: a sibling outside the root replaces it", "[browse_path]") {
  browse_path p;
  p.reset("/a/b");
  p.visit("/a/c");
  CHECK(p.root() == "/a/c");
  CHECK(p.crumbs().size() == 1);
}

TEST_CASE("browse_path: prefix match respects component boundaries", "[browse_path]") {
  CHECK(browse_path::within("/a/b/c", "/a/b"));
  CHECK(browse_path::within("/a/b", "/a/b"));
  CHECK_FALSE(browse_path::within("/a/bc", "/a/b"));
  CHECK_FALSE(browse_path::within("/a", "/a/b"));
  CHECK(browse_path::within("/x", "/"));
}

TEST_CASE("browse_path: parent stops at a volume root", "[browse_path]") {
  CHECK(browse_path::parent_of("/a/b") == "/a");
  CHECK(browse_path::parent_of("/a") == "/");
  CHECK(browse_path::parent_of("/").empty());
  CHECK(browse_path::parent_of("/a/b/") == "/a");
  CHECK(browse_path::parent_of("C:\\Photos\\2024") == "C:\\Photos");
  CHECK(browse_path::parent_of("C:\\Photos") == "C:\\");
  CHECK(browse_path::parent_of("C:\\").empty());
}

TEST_CASE("browse_path: Windows paths and trailing separators", "[browse_path]") {
  browse_path p;
  p.reset("D:\\Photos\\");
  p.visit("D:\\Photos\\2024\\");
  const auto crumbs = p.crumbs();
  REQUIRE(crumbs.size() == 2);
  CHECK(crumbs[0].name == "Photos");
  CHECK(crumbs[1].name == "2024");
  CHECK(crumbs[1].path == "D:\\Photos\\2024");
}

TEST_CASE("browse_path: empty state is safe", "[browse_path]") {
  browse_path p;
  CHECK(p.empty());
  CHECK(p.crumbs().empty());
  CHECK(p.parent().empty());
}

TEST_CASE("browse_path: a volume root is a single crumb", "[browse_path]") {
  browse_path p;
  p.reset("/");
  const auto crumbs = p.crumbs();
  REQUIRE(crumbs.size() == 1);
  CHECK(crumbs[0].name == "/");
}
