// SPDX-License-Identifier: GPL-2.0-or-later
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
