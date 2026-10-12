// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "shell/listing_stamp.h"

using mv::shell::file_stamp;
using mv::shell::listing_stamps;

TEST_CASE("the listing's stamp is used when this app wrote nothing", "[shell][stamp]") {
  listing_stamps stamps;
  REQUIRE(stamps.resolve("C:\\dump\\a.jpg", {100, 10}) == file_stamp{100, 10});
  REQUIRE(stamps.size() == 0);
}

TEST_CASE("a write overrides the listing until it relists", "[shell][stamp]") {
  listing_stamps stamps;
  stamps.rewritten("C:\\dump\\a.jpg", {100, 10}, {120, 20});
  // The listing has not seen the write.
  REQUIRE(stamps.resolve("C:\\dump\\a.jpg", {100, 10}) == file_stamp{120, 20});
  REQUIRE(stamps.resolve("C:\\dump\\b.jpg", {100, 10}) == file_stamp{100, 10});
  // The watcher relisted: the listing is the word, the note is gone.
  REQUIRE(stamps.resolve("C:\\dump\\a.jpg", {120, 20}) == file_stamp{120, 20});
  REQUIRE(stamps.size() == 0);
  REQUIRE(stamps.resolve("C:\\dump\\a.jpg", {100, 10}) == file_stamp{100, 10});
}

TEST_CASE("writes that land before and after a relist chain", "[shell][stamp]") {
  listing_stamps stamps;
  stamps.rewritten("a.jpg", {100, 10}, {120, 20});  // a rating
  stamps.rewritten("a.jpg", {120, 20}, {130, 21});  // then a comment
  REQUIRE(stamps.resolve("a.jpg", {100, 10}) == file_stamp{130, 21});
  // A relist between the two writes saw the first one only.
  REQUIRE(stamps.resolve("a.jpg", {120, 20}) == file_stamp{130, 21});
  REQUIRE(stamps.resolve("a.jpg", {130, 21}) == file_stamp{130, 21});
  REQUIRE(stamps.size() == 0);
}

TEST_CASE("a change made elsewhere after a relist wins", "[shell][stamp]") {
  listing_stamps stamps;
  stamps.rewritten("a.jpg", {100, 10}, {120, 20});
  REQUIRE(stamps.resolve("a.jpg", {900, 99}) == file_stamp{900, 99});
  REQUIRE(stamps.size() == 0);
}

TEST_CASE("a sidecar write leaves the file's stamp alone", "[shell][stamp]") {
  listing_stamps stamps;
  stamps.rewritten("a.cr3", {100, 10}, {100, 10});
  stamps.rewritten("", {100, 10}, {120, 20});
  REQUIRE(stamps.size() == 0);
}
