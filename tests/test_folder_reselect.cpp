// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "abi/folder_reselect.h"

using mv::abi::relist_should_reopen;
using mv::abi::reselect;

namespace {

std::uint32_t pick(const std::vector<std::string>& items, bool changed, std::string_view previous,
                   std::uint32_t previous_index, std::string_view wanted = {}) {
  return reselect(items, [](const std::string& s) -> const std::string& { return s; }, changed,
                  previous, previous_index, wanted);
}

}  // namespace

TEST_CASE("a fresh open selects the requested file, else the first", "[abi][folder]") {
  const std::vector<std::string> items = {"a.jpg", "b.jpg", "c.jpg"};
  REQUIRE(pick(items, false, {}, 0, "b.jpg") == 1);
  REQUIRE(pick(items, false, {}, 0, "missing.jpg") == 0);
  REQUIRE(pick(items, false, {}, 0) == 0);
  REQUIRE(pick({}, false, {}, 0, "a.jpg") == 0);
}

TEST_CASE("a watcher refresh keeps the item the user is on", "[abi][folder]") {
  // A new file sorts in before the current one: the index moves, the item does not.
  const std::vector<std::string> added = {"a.jpg", "aa.jpg", "b.jpg", "c.jpg"};
  REQUIRE(pick(added, true, "b.jpg", 1, "a.jpg") == 2);
  // The originally opened file is not where a refresh goes back to.
  REQUIRE(pick(added, true, "c.jpg", 2, "a.jpg") == 3);
}

TEST_CASE("the current file removed: the next slides in, or the last at the end",
          "[abi][folder]") {
  const std::vector<std::string> after = {"a.jpg", "c.jpg", "d.jpg"};
  REQUIRE(pick(after, true, "b.jpg", 1) == 1);  // c.jpg took b.jpg's place
  const std::vector<std::string> shorter = {"a.jpg", "b.jpg"};
  REQUIRE(pick(shorter, true, "c.jpg", 2) == 1);  // was last: previous item
  REQUIRE(pick({}, true, "a.jpg", 0) == 0);       // folder emptied
  // Nothing was selected before a refresh: behave like an open.
  REQUIRE(pick(after, true, {}, 0, "c.jpg") == 1);
}

namespace {

struct stop {
  std::string primary;
  std::string secondary;
};

std::uint32_t pick_pair(const std::vector<stop>& items, bool changed, std::string_view previous,
                        std::string_view previous_secondary, std::uint32_t previous_index,
                        std::string_view wanted = {}) {
  return mv::abi::reselect_pair(
      items, [](const stop& s) -> const std::string& { return s.primary; },
      [](const stop& s) -> const std::string& { return s.secondary; }, changed, previous,
      previous_secondary, previous_index, wanted);
}

}  // namespace

TEST_CASE("either half of a pair selects its stop", "[abi][folder][pairing]") {
  const std::vector<stop> items = {{"a.jpg", ""}, {"DSC_1.JPG", "DSC_1.NEF"}, {"z.jpg", ""}};
  // Opening the RAW from Explorer lands on the JPEG+RAW stop.
  REQUIRE(pick_pair(items, false, {}, {}, 0, "DSC_1.NEF") == 1);
  REQUIRE(pick_pair(items, false, {}, {}, 0, "DSC_1.JPG") == 1);
  // A refresh keeps the stop when a file is added in front of it.
  const std::vector<stop> added = {{"a.jpg", ""}, {"b.jpg", ""}, {"DSC_1.JPG", "DSC_1.NEF"}};
  REQUIRE(pick_pair(added, true, "DSC_1.JPG", "DSC_1.NEF", 1) == 2);
}

TEST_CASE("a refresh that loses one half stays on the half that is left",
          "[abi][folder][pairing]") {
  // The JPEG went to the Recycle Bin on its own: the RAW is now a stop.
  const std::vector<stop> raw_left = {{"a.jpg", ""}, {"DSC_1.NEF", ""}, {"z.jpg", ""}};
  REQUIRE(pick_pair(raw_left, true, "DSC_1.JPG", "DSC_1.NEF", 1) == 1);
  // Two separate files became a pair: the user's file is now a secondary.
  const std::vector<stop> paired = {{"a.jpg", ""}, {"DSC_1.JPG", "DSC_1.NEF"}};
  REQUIRE(pick_pair(paired, true, "DSC_1.NEF", {}, 2) == 1);
}

// Issue #56: a watcher/poll relist that lands back on the file already open
// must not reopen it (a clip's fresh media_source starts paused-by-default
// and the gallery-hold autoplay gate waves it through, silently overriding a
// user's pause). Windows' apply_folder_list and the Mac host's
// -refreshFolderIfChanged both gate their reopen call on this.
TEST_CASE("a relist reopens only a first listing or a genuine file change",
          "[abi][folder]") {
  // Nothing was open yet (a folder's first listing, or the folder was just
  // emptied out): always worth an open, even if the path happens to match.
  REQUIRE(relist_should_reopen(false, false));
  REQUIRE(relist_should_reopen(false, true));
  // Something is already open and the relist still resolves to that same
  // file: a watcher tick that saw an unrelated change must not reopen it.
  REQUIRE_FALSE(relist_should_reopen(true, true));
  // Something is already open and the relist lands on a different file (the
  // old one was deleted/renamed out from under the selection): reopen.
  REQUIRE(relist_should_reopen(true, false));
}
