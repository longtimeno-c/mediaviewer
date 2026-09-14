// SPDX-License-Identifier: GPL-2.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <string_view>

#include "io/collision_name.h"

using mv::io::collision_name;
using mv::io::unique_name;

TEST_CASE("collision names insert (n) before the last extension", "[io][copy]") {
  REQUIRE(collision_name("IMG_0001.JPG", 2) == "IMG_0001 (2).JPG");
  REQUIRE(collision_name("IMG_0001.JPG", 12) == "IMG_0001 (12).JPG");
  REQUIRE(collision_name("archive.tar.gz", 2) == "archive.tar (2).gz");
  REQUIRE(collision_name("README", 2) == "README (2)");
  REQUIRE(collision_name(".profile", 2) == ".profile (2)");
  REQUIRE(collision_name("trailing.", 2) == "trailing (2).");
  // UTF-8 names are bytes to this function; nothing is split mid-character
  // because '.' never occurs inside a multi-byte sequence.
  REQUIRE(collision_name("\xE5\x86\x99\xE7\x9C\x9F.heic", 3) == "\xE5\x86\x99\xE7\x9C\x9F (3).heic");
}

TEST_CASE("unique_name never returns a taken name", "[io][copy]") {
  std::set<std::string, std::less<>> taken;
  const auto exists = [&taken](std::string_view name) { return taken.count(name) != 0; };

  REQUIRE(unique_name("a.jpg", exists) == "a.jpg");
  taken.insert("a.jpg");
  REQUIRE(unique_name("a.jpg", exists) == "a (2).jpg");
  taken.insert("a (2).jpg");
  taken.insert("a (3).jpg");
  REQUIRE(unique_name("a.jpg", exists) == "a (4).jpg");

  // Exhausted: empty, never an overwrite.
  REQUIRE(unique_name("a.jpg", exists, 3).empty());
  REQUIRE(unique_name("", exists).empty());
}
