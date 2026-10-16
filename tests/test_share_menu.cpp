// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #444: the one share command's file set and its wire argument
// (shell/share_menu.h), shared by both hosts.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "shell/commands.h"
#include "shell/key_router.h"
#include "shell/share_menu.h"

using namespace mv::shell;

TEST_CASE("a share request survives the island's float", "[shell][share]") {
  for (share_target t : {share_target::menu, share_target::sharepoint, share_target::system}) {
    for (share_from f : {share_from::key, share_from::bar, share_from::gallery, share_from::context}) {
      for (std::int32_t clicked : {-1, 0, 1, 499, 65535, kShareClickedLimit - 1}) {
        const share_request r{t, f, clicked};
        const share_request back = unpack_share_request(pack_share_request(r));
        CHECK(back.target == t);
        CHECK(back.from == f);
        CHECK(back.clicked == clicked);
      }
    }
  }
  // 0 is the key's request: the menu, on the selection.
  const share_request key = unpack_share_request(0.0f);
  CHECK(key.target == share_target::menu);
  CHECK(key.from == share_from::key);
  CHECK(key.clicked == -1);
  // An index that does not fit is dropped, never wrapped onto another stop.
  CHECK(unpack_share_request(pack_share_request({share_target::system, share_from::context, kShareClickedLimit}))
            .clicked == -1);
  // Garbage reads as the key's request.
  CHECK(unpack_share_request(-4.0f).clicked == -1);
  CHECK(unpack_share_request(1e9f).target == share_target::menu);
  CHECK(unpack_share_request(3.0f).target == share_target::menu);  // no target 3
}

TEST_CASE("right-click shares the selection when the clicked file is in it", "[shell][share]") {
  const std::vector<std::string> three = {"/d/a.jpg", "/d/b.jpg", "/d/c.jpg"};
  // The icon, the bar and the key: the selection as it stands.
  CHECK(share_stops(three, "") == three);
  // Right-click a selected cell: the same three files.
  CHECK(share_stops(three, "/d/b.jpg") == three);
  // Right-click an unselected cell: that one file.
  CHECK(share_stops(three, "/d/z.jpg") == std::vector<std::string>{"/d/z.jpg"});
  // Nothing selected (an empty folder) and nothing clicked: nothing to share.
  CHECK(share_stops({}, "").empty());
  CHECK(share_stops({}, "/d/a.jpg") == std::vector<std::string>{"/d/a.jpg"});
}

TEST_CASE("pair halves follow their primary", "[shell][share]") {
  const std::map<std::string, std::string, std::less<>> pairs = {
      {"/d/a.jpg", "/d/a.cr3"},
      {"/d/live.heic", "/d/live.mov"},
  };
  CHECK(share_with_pairs({"/d/a.jpg", "/d/b.jpg", "/d/live.heic"}, pairs) ==
        std::vector<std::string>{"/d/a.jpg", "/d/a.cr3", "/d/b.jpg", "/d/live.heic", "/d/live.mov"});
  CHECK(share_with_pairs({"/d/b.jpg"}, {}) == std::vector<std::string>{"/d/b.jpg"});
}

TEST_CASE("the system sheet gets media only", "[shell][share]") {
  CHECK(share_is_sidecar("/d/a.xmp"));
  CHECK(share_is_sidecar("C:\\d\\A.XMP"));
  CHECK(share_is_sidecar("/d/IMG_0001.AAE"));
  CHECK(share_is_sidecar("/d/MVI_0001.THM"));
  CHECK_FALSE(share_is_sidecar("/d/a.jpg"));
  CHECK_FALSE(share_is_sidecar("/d/a.cr3"));
  CHECK_FALSE(share_is_sidecar("/d/.xmp"));        // a dot file, not an extension
  CHECK_FALSE(share_is_sidecar("/d.xmp/a"));       // the folder's dot does not count
  CHECK_FALSE(share_is_sidecar("/d/a.xmpx"));
  CHECK(share_system_files({"/d/a.jpg", "/d/a.xmp", "/d/a.cr3", "", "/d/a.jpg", "/d/b.mov"}) ==
        std::vector<std::string>{"/d/a.jpg", "/d/a.cr3", "/d/b.mov"});
}

TEST_CASE("one share command, on Ctrl+Shift+S in every viewing mode", "[shell][share][commands]") {
  key_router r;
  view_state still;
  still.item = item_kind::still;
  view_state clip;
  clip.item = item_kind::clip;
  view_state gallery = still;
  gallery.gallery_open = true;
  for (const view_state& s : {still, clip, gallery}) {
    const auto out = r.on_key({char_key('S'), mod_ctrl | mod_shift, false, false}, s);
    CHECK(out.handled);
    CHECK(out.command == command_id::share);
  }
  // The bar's, the icon's and the menus' items post this id; it is never renumbered.
  CHECK(static_cast<int>(command_id::share) == 149);
  const command_info* info = find_command(command_id::share);
  REQUIRE(info != nullptr);
  CHECK(std::string(info->name) == "Share…");
  CHECK_FALSE(info->keyless);
}

TEST_CASE("a 500-file share set is gathered in well under a frame", "[shell][share][.perf-bench]") {
  // Issue #444: opening the menu with a 500-item selection must not stall.
  // The UI thread only picks the stops; the pairs walk is the pool's.
  std::vector<std::string> selection;
  std::map<std::string, std::string, std::less<>> pairs;
  for (int i = 0; i < 20000; ++i) {
    const std::string p = "/Volumes/Card/DCIM/100CANON/IMG_" + std::to_string(10000 + i) + ".JPG";
    if (i % 40 == 0) selection.push_back(p);
    if (i % 2 == 0) pairs.emplace(p, p.substr(0, p.size() - 3) + "CR3");
  }
  REQUIRE(selection.size() == 500);
  const auto t0 = std::chrono::steady_clock::now();
  const auto stops = share_stops(selection, selection[250]);
  const auto files = share_with_pairs(stops, pairs);
  const auto media = share_system_files(files);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK(media.size() == 1000);
  WARN("500 stops -> " << media.size() << " files in " << ms << " ms");
  CHECK(ms < 16.0);
}
