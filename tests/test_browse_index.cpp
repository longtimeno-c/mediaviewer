// SPDX-License-Identifier: GPL-2.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "shell/browse_index.h"

using mv::shell::browse_index;

TEST_CASE("browse_index is empty with no items", "[shell][browse]") {
  browse_index idx;
  idx.reset(0);
  REQUIRE(idx.empty());
  REQUIRE(idx.count() == 0);
  REQUIRE(idx.current() == 0);
  REQUIRE(idx.next() == 0);
  REQUIRE(idx.prev() == 0);
  REQUIRE(idx.first() == 0);
  REQUIRE(idx.last() == 0);
  REQUIRE(idx.skip(10) == 0);
}

TEST_CASE("browse_index reset clamps an out-of-range index to the last item", "[shell][browse]") {
  browse_index idx;
  idx.reset(5, 99);
  REQUIRE(idx.current() == 4);
  idx.reset(5, 2);
  REQUIRE(idx.current() == 2);
}

TEST_CASE("browse_index next/prev wrap at the ends", "[shell][browse]") {
  browse_index idx;
  idx.reset(3, 0);
  REQUIRE(idx.next() == 1);
  REQUIRE(idx.next() == 2);
  REQUIRE(idx.next() == 0);  // wraps past the last item
  REQUIRE(idx.prev() == 2);  // wraps back past the first
  REQUIRE(idx.prev() == 1);
}

TEST_CASE("browse_index first/last", "[shell][browse]") {
  browse_index idx;
  idx.reset(10, 5);
  REQUIRE(idx.last() == 9);
  REQUIRE(idx.first() == 0);
}

TEST_CASE("browse_index skip wraps both directions by an arbitrary delta", "[shell][browse]") {
  browse_index idx;
  idx.reset(10, 0);
  REQUIRE(idx.skip(4) == 4);
  REQUIRE(idx.skip(10) == 4);   // a full lap lands back where it started
  REQUIRE(idx.skip(-1) == 3);
  REQUIRE(idx.skip(-10) == 3);  // a full lap backwards, same story
  REQUIRE(idx.skip(-5) == 8);   // wraps past the front
  idx.reset(1, 0);
  REQUIRE(idx.skip(1) == 0);    // a single item is its own wrap
  REQUIRE(idx.skip(-1) == 0);
}

TEST_CASE("browse_index a single item never moves", "[shell][browse]") {
  browse_index idx;
  idx.reset(1, 0);
  REQUIRE(idx.next() == 0);
  REQUIRE(idx.prev() == 0);
  REQUIRE(idx.first() == 0);
  REQUIRE(idx.last() == 0);
}
