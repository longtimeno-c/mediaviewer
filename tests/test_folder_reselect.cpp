// SPDX-License-Identifier: GPL-2.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "abi/folder_reselect.h"

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
