// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
  SECTION("a bare drive is its root, and one entry with it") {
    list = push_recent_folder(list, "D:\\");
    list = push_recent_folder(list, "D:");
    REQUIRE(list[0] == "D:\\");
    REQUIRE(std::count(list.begin(), list.end(), std::string("D:\\")) == 1);
  }
}

// Issue #617: pins lead every recent list and survive any number of newer opens.
TEST_CASE("a pinned folder stays first however many folders are opened", "[shell][os][pinned]") {
  std::vector<std::string> recent;
  std::vector<std::string> pinned;
  recent = push_recent_folder(recent, "\\\\nas\\photos");
  recent = push_recent_folder(recent, "D:\\2026");
  pinned = pin_folder(pinned, "\\\\nas\\photos");
  REQUIRE(pinned == std::vector<std::string>{"\\\\nas\\photos"});
  REQUIRE(is_pinned_folder(pinned, "\\\\NAS\\Photos\\"));
  REQUIRE_FALSE(is_pinned_folder(pinned, "D:\\2026"));

  // The verify: ten newer opens.
  for (int i = 0; i < 10; ++i) recent = push_recent_folder(recent, "E:\\card\\" + std::to_string(i));
  REQUIRE_FALSE(std::count(recent.begin(), recent.end(), std::string("\\\\nas\\photos")));
  const std::vector<std::string> shown = pinned_then_recent(pinned, recent);
  REQUIRE(shown.size() == kMaxRecentFolders);
  REQUIRE(shown.front() == "\\\\nas\\photos");
  REQUIRE(shown[1] == "E:\\card\\9");
  REQUIRE(shown.back() == "E:\\card\\1");  // the pin takes one recent's place

  welcome_recents card;
  fill_welcome_recents(shown, "", card, false, pinned.size());
  REQUIRE(card.count == welcome_recents::kMax);
  REQUIRE(card.pinned == 1);
  REQUIRE(std::string(card.label[0]) == "photos");
  welcome_recents with_cloud;
  fill_welcome_recents(shown, "", with_cloud, "OneDrive", "~\\OneDrive", pinned.size());
  REQUIRE(with_cloud.icloud);
  REQUIRE(with_cloud.pinned == 1);
  REQUIRE(std::string(with_cloud.label[1]) == "photos");
  welcome_recents none;
  fill_welcome_recents(shown, "", none);
  REQUIRE(none.pinned == 0);

  SECTION("opening a pinned folder moves it in the MRU list, not among the pins") {
    pinned = pin_folder(pinned, "D:\\2026");
    recent = push_recent_folder(recent, "D:\\2026");
    const auto again = pinned_then_recent(pinned, recent);
    REQUIRE(again[0] == "\\\\nas\\photos");
    REQUIRE(again[1] == "D:\\2026");
    REQUIRE(again[2] == "E:\\card\\9");
    REQUIRE(std::count(again.begin(), again.end(), std::string("D:\\2026")) == 1);
  }
  SECTION("unpinning restores MRU order") {
    pinned = remove_folder(pinned, "\\\\nas\\photos\\");
    REQUIRE(pinned.empty());
    REQUIRE(pinned_then_recent(pinned, recent) == recent);
  }
  SECTION("an unpinned folder still in the MRU list goes back to its MRU place") {
    recent = push_recent_folder(recent, "\\\\nas\\photos");
    recent = push_recent_folder(recent, "E:\\card\\5");
    REQUIRE(pinned_then_recent(pinned, recent)[0] == "\\\\nas\\photos");
    pinned = remove_folder(pinned, "\\\\nas\\photos");
    const auto mru = pinned_then_recent(pinned, recent);
    REQUIRE(mru == recent);
    REQUIRE(mru[0] == "E:\\card\\5");
    REQUIRE(mru[1] == "\\\\nas\\photos");
  }
}

TEST_CASE("pins are one entry per folder, in the order pinned, capped", "[shell][os][pinned]") {
  std::vector<std::string> pinned;
  pinned = pin_folder(pinned, "/Volumes/NAS/Photos");
  pinned = pin_folder(pinned, "/Users/me/Pictures/2026/");
  REQUIRE(pinned == std::vector<std::string>{"/Volumes/NAS/Photos", "/Users/me/Pictures/2026"});
  SECTION("pinning again changes nothing") {
    REQUIRE(pin_folder(pinned, "/volumes/nas/photos/") == pinned);
  }
  SECTION("an empty folder is not a pin") {
    REQUIRE(pin_folder(pinned, "") == pinned);
    REQUIRE_FALSE(is_pinned_folder(pinned, ""));
    REQUIRE(remove_folder(pinned, "") == pinned);
  }
  SECTION("a bare drive pins its root") {
    REQUIRE(pin_folder({}, "D:") == std::vector<std::string>{"D:\\"});
  }
  SECTION("a full list takes no more") {
    for (int i = 0; i < 10; ++i) pinned = pin_folder(pinned, "/p/" + std::to_string(i));
    REQUIRE(pinned.size() == kMaxPinnedFolders);
    REQUIRE(pinned.back() == "/p/2");
    REQUIRE(pin_folder(pinned, "/q") == pinned);
  }
  SECTION("the shown list never repeats a folder and stops at max") {
    const std::vector<std::string> recent = {"/Users/me/Pictures/2026", "/a", "/b", "/c"};
    const auto shown = pinned_then_recent(pinned, recent, 4);
    REQUIRE(shown == std::vector<std::string>{"/Volumes/NAS/Photos", "/Users/me/Pictures/2026", "/a", "/b"});
    REQUIRE(pinned_then_recent(pinned, recent, 1) == std::vector<std::string>{"/Volumes/NAS/Photos"});
    REQUIRE(pinned_then_recent({}, {}).empty());
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

TEST_CASE("the welcome card's recent rows name the folder and where it lives", "[shell][os]") {
  const std::vector<std::string> dirs = {"/Users/ana/Pictures/2026-09 Iceland/", "D:\\Iceland", "D:\\",
                                         "/Volumes/CARD_A/DCIM", "/Photos", "/Users/ana"};
  welcome_recents r;
  r.hover = 2;
  fill_welcome_recents(dirs, "/Users/ana/", r);
  REQUIRE(r.count == 6);
  CHECK(r.hover == -1);
  CHECK(std::string(r.label[0]) == "2026-09 Iceland");
  CHECK(std::string(r.where[0]) == "~/Pictures");
  CHECK(std::string(r.label[1]) == "Iceland");
  CHECK(std::string(r.where[1]) == "D:\\");
  CHECK(std::string(r.label[2]) == "D:\\");
  CHECK(std::string(r.where[2]).empty());
  CHECK(std::string(r.where[3]) == "/Volumes/CARD_A");
  CHECK(std::string(r.where[4]) == "/");
  CHECK(std::string(r.label[5]) == "ana");
  CHECK(std::string(r.where[5]) == "/Users");  // home's own parent is not under home

  SECTION("a folder beside home is not under it") {
    const std::vector<std::string> one = {"/Users/anabel/Photos"};
    fill_welcome_recents(one, "/Users/ana", r);
    CHECK(std::string(r.where[0]) == "/Users/anabel");
  }
  SECTION("a Windows home matches whatever its case and separators") {
    const std::vector<std::string> win = {"c:\\users\\Ana\\Pictures\\Iceland", "C:/Users/Ana/Desktop/x",
                                          "C:\\Users\\Anabel\\x"};
    fill_welcome_recents(win, "C:\\Users\\Ana\\", r);
    REQUIRE(r.count == 3);
    CHECK(std::string(r.where[0]) == "~\\Pictures");
    CHECK(std::string(r.where[1]) == "~/Desktop");
    CHECK(std::string(r.where[2]) == "C:\\Users\\Anabel");
  }
  SECTION("at most kMax rows") {
    std::vector<std::string> many;
    for (int i = 0; i < 10; ++i) many.push_back("/x/" + std::to_string(i));
    fill_welcome_recents(many, "", r);
    CHECK(r.count == welcome_recents::kMax);
  }
  SECTION("the iCloud Photos row comes first and the folders follow it") {
    std::vector<std::string> many;
    for (int i = 0; i < 10; ++i) many.push_back("/x/" + std::to_string(i));
    fill_welcome_recents(many, "", r, true);
    CHECK(r.icloud);
    CHECK(r.count == welcome_recents::kMax);  // still kMax rows in all
    CHECK(std::string(r.label[0]) == "iCloud Photos");
    CHECK(std::string(r.label[1]) == "0");
    CHECK(std::string(r.label[welcome_recents::kMax - 1]) == std::to_string(welcome_recents::kMax - 2));
    fill_welcome_recents(many, "", r);
    CHECK_FALSE(r.icloud);
    CHECK(std::string(r.label[0]) == "0");
    const std::vector<std::string> none;
    fill_welcome_recents(none, "", r, true);
    CHECK(r.count == 1);  // the library alone, before any folder was opened
  }
  SECTION("Windows: OneDrive leads, as iCloud Photos does on the Mac") {
    const std::vector<std::string> one{"C:\\Users\\Ana\\Pictures\\Iceland"};
    fill_welcome_recents(one, "C:\\Users\\Ana", r, "OneDrive", "~\\OneDrive");
    REQUIRE(r.count == 2);
    CHECK(r.icloud);
    CHECK(std::string(r.label[0]) == "OneDrive");
    CHECK(std::string(r.where[0]) == "~\\OneDrive");
    CHECK(std::string(r.label[1]) == "Iceland");
    fill_welcome_recents(one, "C:\\Users\\Ana", r, "", "");
    CHECK_FALSE(r.icloud);
    CHECK(r.count == 1);
  }
  SECTION("a long name is cut at a character boundary") {
    std::string name;
    for (int i = 0; i < 60; ++i) name += "\xC3\xA9";  // é, two bytes each
    const std::vector<std::string> one = {"/x/" + name};
    fill_welcome_recents(one, "", r);
    const std::string label = r.label[0];
    CHECK(label.size() == sizeof(r.label[0]) - 2);  // 94: 95 would split an é
    CHECK(label.size() % 2 == 0);
  }
}

TEST_CASE("the welcome card's rows are hit where they are drawn", "[shell][os]") {
  const float w = 1600.0f, h = 1000.0f, chrome = 80.0f, scale = 2.0f;
  const welcome_geometry plain = layout_welcome(w, h, chrome, scale, 0);
  REQUIRE(plain.fits);
  CHECK(plain.rows == 0);
  CHECK(welcome_row_at(plain, w * 0.5f, plain.hi_y - 1.0f) == -1);

  const welcome_geometry g = layout_welcome(w, h, chrome, scale, 3);
  REQUIRE(g.fits);
  REQUIRE(g.rows == 3);
  CHECK(g.hi_y - g.lo_y > plain.hi_y - plain.lo_y);
  const float cx = w * 0.5f;
  CHECK(welcome_row_at(g, cx, g.rows_top - 1.0f) == -1);
  CHECK(welcome_row_at(g, cx, g.rows_top + 1.0f) == 0);
  CHECK(welcome_row_at(g, cx, g.rows_top + g.row_h * 2.5f) == 2);
  CHECK(welcome_row_at(g, cx, g.rows_top + g.row_h * 3.0f) == -1);
  CHECK(welcome_row_at(g, g.row_x0 - 1.0f, g.rows_top + 1.0f) == -1);
  // The x at a row's right end removes; the rest of the row opens.
  CHECK(welcome_on_remove(g, g.row_x1 - 1.0f));
  CHECK(welcome_on_remove(g, g.row_x1 - (kWelcomeRemoveW - 1.0f) * scale));
  CHECK_FALSE(welcome_on_remove(g, g.row_x1 - (kWelcomeRemoveW + 1.0f) * scale));
  CHECK_FALSE(welcome_on_remove(g, cx));
  CHECK_FALSE(welcome_on_remove(g, g.row_x1));

  SECTION("the iCloud Photos library leads, above the folders' header") {
    const welcome_geometry lead = layout_welcome(w, h, chrome, scale, 3, 0.0f, true);
    REQUIRE(lead.fits);
    REQUIRE(lead.lead);
    REQUIRE(lead.rows == 3);
    CHECK(lead.rows_top < g.rows_top);  // above where the header used to start the rows
    CHECK(welcome_row_top(lead, 0) == lead.rows_top);
    CHECK(welcome_row_top(lead, 1) == lead.folders_top);
    CHECK(lead.folders_top >= lead.rows_top + lead.row_h + 20.0f * scale);  // room for the header
    CHECK(welcome_row_at(lead, cx, lead.rows_top + 1.0f) == 0);
    CHECK(welcome_row_at(lead, cx, lead.rows_top + lead.row_h + 1.0f) == -1);  // the header
    CHECK(welcome_row_at(lead, cx, lead.folders_top + 1.0f) == 1);
    CHECK(welcome_row_at(lead, cx, lead.folders_top + lead.row_h * 1.5f) == 2);
    CHECK(welcome_row_at(lead, cx, lead.folders_top + lead.row_h * 2.0f) == -1);
    CHECK(lead.hi_y >= lead.folders_top + 2.0f * lead.row_h);

    const welcome_geometry alone = layout_welcome(w, h, chrome, scale, 1, 0.0f, true);
    REQUIRE(alone.rows == 1);
    CHECK(welcome_row_at(alone, cx, alone.rows_top + 1.0f) == 0);
    CHECK(alone.hi_y < alone.folders_top);  // no header, no empty folder room
  }
  SECTION("a short window drops rows before it drops the card") {
    const welcome_geometry tight = layout_welcome(w, 700.0f, chrome, scale, 6);
    REQUIRE(tight.fits);
    CHECK(tight.rows < 6);
    CHECK(tight.hi_y - tight.lo_y <= 700.0f - chrome);
  }
  SECTION("too short for the card: nothing, and nothing to hit") {
    const welcome_geometry none = layout_welcome(w, 400.0f, chrome, scale, 6);
    CHECK_FALSE(none.fits);
    CHECK(welcome_row_at(none, cx, 300.0f) == -1);
  }
}
