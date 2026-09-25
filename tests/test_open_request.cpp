// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "shell/open_request.h"

using namespace mv::shell;

TEST_CASE("open paths are normalised without losing long-path prefixes", "[shell][open]") {
  REQUIRE(normalize_open_path(L"  \"C:\\Photos\\2026\\\"  ") == L"C:\\Photos\\2026");
  REQUIRE(normalize_open_path(L"C:/Photos/IMG_0001.JPG") == L"C:\\Photos\\IMG_0001.JPG");
  REQUIRE(normalize_open_path(L"C:\\") == L"C:\\");
  REQUIRE(normalize_open_path(L"C:\\\\") == L"C:\\");
  REQUIRE(normalize_open_path(L"\\\\?\\C:\\very\\long\\path\\") == L"\\\\?\\C:\\very\\long\\path");
  REQUIRE(normalize_open_path(L"\\\\?\\C:\\") == L"\\\\?\\C:\\");
  REQUIRE(normalize_open_path(L"\\\\server\\share\\dump\\") == L"\\\\server\\share\\dump");
  REQUIRE(normalize_open_path(L"D:\\\u5199\u771F\\\u732B.heic") == L"D:\\\u5199\u771F\\\u732B.heic");
  REQUIRE(normalize_open_path(L"\"\"").empty());
  REQUIRE(normalize_open_path(L"").empty());
}

TEST_CASE("the first existing entry decides what opens", "[shell][open]") {
  SECTION("one folder opens that folder") {
    const std::vector<path_probe> drop = {{L"C:\\dump", true, true}};
    const auto r = resolve_open(drop);
    REQUIRE(r.kind == open_kind::folder);
    REQUIRE(r.path == L"C:\\dump");
  }
  SECTION("several files open their folder on the first") {
    const std::vector<path_probe> drop = {{L"C:\\dump\\b.jpg", true, false},
                                          {L"C:\\dump\\a.jpg", true, false}};
    const auto r = resolve_open(drop);
    REQUIRE(r.kind == open_kind::file);
    REQUIRE(r.path == L"C:\\dump\\b.jpg");
  }
  SECTION("a mixed drop opens the first file's folder") {
    const std::vector<path_probe> drop = {{L"D:\\other\\x.mp4", true, false},
                                          {L"C:\\dump", true, true}};
    REQUIRE(resolve_open(drop).path == L"D:\\other\\x.mp4");
  }
  SECTION("entries that do not exist are skipped") {
    const std::vector<path_probe> drop = {{L"C:\\gone.jpg", false, false},
                                          {L"C:\\dump", true, true}};
    const auto r = resolve_open(drop);
    REQUIRE(r.kind == open_kind::folder);
    REQUIRE(r.path == L"C:\\dump");
  }
  SECTION("nothing exists: missing, and nothing to open") {
    const std::vector<path_probe> drop = {{L"C:\\gone.jpg", false, false}};
    const auto r = resolve_open(drop);
    REQUIRE(r.kind == open_kind::missing);
    REQUIRE(r.path.empty());
  }
  SECTION("no arguments is not an error") {
    REQUIRE(resolve_open({}).kind == open_kind::none);
  }
}
