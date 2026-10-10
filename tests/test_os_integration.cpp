// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "shell/os_integration.h"

using namespace mv::shell;
using Catch::Approx;

TEST_CASE("print fits the still on the page, centred, keeping its shape", "[shell][os][print]") {
  // US Letter's printable area at 600 dpi, less a quarter inch all round.
  const print_rect page{150, 150, 4800, 6300};
  // A 3:2 landscape still on a portrait page: width-bound.
  print_rect r = place_print(6000, 4000, page, 600, 600, print_scale::fit);
  CHECK(r.w == Approx(4800));
  CHECK(r.h == Approx(3200));
  CHECK(r.x == Approx(150));
  CHECK(r.y == Approx(150 + (6300 - 3200) / 2.0));
  // A portrait still: height-bound, centred across.
  r = place_print(3000, 4500, page, 600, 600, print_scale::fit);
  CHECK(r.h == Approx(6300));
  CHECK(r.w == Approx(4200));
  CHECK(r.x == Approx(150 + 300));
  // A small still is scaled up to the page, as Preview's Scale to Fit does.
  r = place_print(48, 32, page, 600, 600, print_scale::fit);
  CHECK(r.w == Approx(4800));
  // Square pixels on a 600 x 300 dpi device stay square on paper.
  r = place_print(1000, 1000, print_rect{0, 0, 4800, 3150}, 600, 300, print_scale::fit);
  CHECK(r.w / 600 == Approx(r.h / 300));
  CHECK(r.w <= 4800 + 1e-9);
  CHECK(r.h <= 3150 + 1e-9);
}

TEST_CASE("print at actual size is one pixel per point, cut at the page", "[shell][os][print]") {
  const print_rect page{0, 0, 612, 792};  // Letter in points, the Mac's units
  print_rect r = place_print(144, 72, page, 72, 72, print_scale::actual_size);
  CHECK(r.w == Approx(144));
  CHECK(r.h == Approx(72));
  CHECK(r.x == Approx((612 - 144) / 2.0));
  CHECK(r.y == Approx((792 - 72) / 2.0));
  // At 600 dpi the same still is two inches by one.
  r = place_print(144, 72, print_rect{0, 0, 5100, 6600}, 600, 600, print_scale::actual_size);
  CHECK(r.w == Approx(1200));
  CHECK(r.h == Approx(600));
  // Larger than the page: centred, overhanging every edge; the device clips.
  r = place_print(6000, 4000, page, 72, 72, print_scale::actual_size);
  CHECK(r.w == Approx(6000));
  CHECK(r.x < 0);
  CHECK(r.x + r.w / 2 == Approx(306));
  // Nothing to place.
  r = place_print(0, 10, page, 72, 72, print_scale::fit);
  CHECK(r.w == 0);
  r = place_print(10, 10, print_rect{}, 72, 72, print_scale::fit);
  CHECK(r.w == 0);
}

TEST_CASE("print orientation follows the still", "[shell][os][print]") {
  CHECK(print_landscape(6000, 4000));
  CHECK_FALSE(print_landscape(4000, 6000));
  CHECK_FALSE(print_landscape(4000, 4000));
}

TEST_CASE("print pixels are composited over white", "[shell][os][print]") {
  std::vector<std::uint8_t> px = {
      10, 20, 30, 255,   // opaque: unchanged
      0, 0, 0, 0,        // clear: white paper
      200, 100, 0, 128,  // half: halfway to white
  };
  prepare_print_pixels(px, false);
  CHECK(px == std::vector<std::uint8_t>{10, 20, 30, 255, 255, 255, 255, 255, 227, 177, 127, 255});
  std::vector<std::uint8_t> dib = {1, 2, 3, 255};
  prepare_print_pixels(dib, true);
  CHECK(dib == std::vector<std::uint8_t>{3, 2, 1, 255});
}

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
