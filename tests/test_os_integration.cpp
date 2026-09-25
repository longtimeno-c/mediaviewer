// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "shell/os_integration.h"

using namespace mv::shell;

TEST_CASE("recent folders are most recent first, one entry per folder", "[shell][os]") {
  std::vector<std::string> list;
  list = push_recent_folder(list, "C:\\dump\\a");
  list = push_recent_folder(list, "C:\\dump\\b");
  REQUIRE(list == std::vector<std::string>{"C:\\dump\\b", "C:\\dump\\a"});

  SECTION("reopening moves it to the front under its new spelling") {
    list = push_recent_folder(list, "c:/DUMP/A/");
    REQUIRE(list == std::vector<std::string>{"c:/DUMP/A", "C:\\dump\\b"});
  }
  SECTION("the list is capped") {
    for (int i = 0; i < 20; ++i) list = push_recent_folder(list, "/Volumes/card/" + std::to_string(i));
    REQUIRE(list.size() == kMaxRecentFolders);
    REQUIRE(list.front() == "/Volumes/card/19");
    REQUIRE(list.back() == "/Volumes/card/10");
  }
  SECTION("an empty folder changes nothing") {
    REQUIRE(push_recent_folder(list, "") == list);
  }
  SECTION("roots keep their separator") {
    list = push_recent_folder(list, "D:\\");
    list = push_recent_folder(list, "/");
    REQUIRE(list[0] == "/");
    REQUIRE(list[1] == "D:\\");
  }
}

TEST_CASE("a recent folder is labelled by its last component", "[shell][os]") {
  REQUIRE(folder_display_name("C:\\Photos\\2026-09 Iceland") == "2026-09 Iceland");
  REQUIRE(folder_display_name("/Users/me/Pictures/dump/") == "dump");
  REQUIRE(folder_display_name("D:\\") == "D:\\");
  REQUIRE(folder_display_name("/") == "/");
  REQUIRE(folder_display_name("relative") == "relative");
}

TEST_CASE("recent folders that share a name are told apart by their parent", "[shell][os]") {
  const std::vector<std::string> dirs = {"/Volumes/CARD_A/DCIM", "D:\\Iceland", "/Volumes/CARD_B/DCIM/"};
  const auto labels = recent_folder_labels(dirs);
  REQUIRE(labels.size() == 3);
  CHECK(labels[0] == "DCIM \u2014 CARD_A");
  CHECK(labels[1] == "Iceland");
  CHECK(labels[2] == "DCIM \u2014 CARD_B");
  const std::vector<std::string> roots = {"/", "D:\\"};
  CHECK(recent_folder_labels(roots) == std::vector<std::string>{"/", "D:\\"});
}

TEST_CASE("copy path is one path per line, no trailing newline", "[shell][os]") {
  const std::vector<std::string> one = {"C:\\dump\\IMG_0001.JPG"};
  REQUIRE(paths_as_text(one, "\r\n") == "C:\\dump\\IMG_0001.JPG");
  const std::vector<std::string> two = {"/a/1.heic", "", "/a/2.cr3"};
  REQUIRE(paths_as_text(two, "\n") == "/a/1.heic\n/a/2.cr3");
  REQUIRE(paths_as_text({}, "\n").empty());
}

TEST_CASE("the handler's file list is names only", "[shell][os][shellext]") {
  CHECK(parse_shellext_file_list("MediaViewerThumbs.dll\r\nheif.dll\n\nraw_r.dll\n") ==
        std::vector<std::string>{"MediaViewerThumbs.dll", "heif.dll", "raw_r.dll"});
  CHECK(parse_shellext_file_list("").empty());
  // One bad line refuses the list: the copy never leaves its two folders.
  CHECK(parse_shellext_file_list("MediaViewerThumbs.dll\n..\\evil.dll\n").empty());
  CHECK(parse_shellext_file_list("C:evil.dll\n").empty());
  CHECK(parse_shellext_file_list("sub/evil.dll\n").empty());
  CHECK(parse_shellext_file_list("..\n").empty());
}

TEST_CASE("each version's handler gets its own folder; the rest are pruned", "[shell][os][shellext]") {
  const std::vector<std::string> existing = {"0.1.3", "0.1.4", "junk"};
  const auto plan = plan_shellext_install("0.1.4", existing);
  CHECK(plan.version_dir == "0.1.4");
  CHECK(plan.prune == std::vector<std::string>{"0.1.3", "junk"});
  CHECK(plan_shellext_install("1.0.0-beta+7", {}).version_dir == "1.0.0-beta+7");
  CHECK(plan_shellext_install("", existing).version_dir.empty());
  CHECK(plan_shellext_install("..", existing).version_dir.empty());
  CHECK(plan_shellext_install("0.1\\..\\x", existing).version_dir.empty());
}
