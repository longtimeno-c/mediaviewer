// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <set>

#include "shell/slideshow.h"

using mv::shell::slideshow;

TEST_CASE("slideshow interval steps a clamped ladder", "[shell][slideshow]") {
  slideshow s;
  REQUIRE(s.interval_ms() == 4000);
  for (int i = 0; i < 20; ++i) s.faster();
  REQUIRE(s.interval_ms() == 1000);
  for (int i = 0; i < 20; ++i) s.slower();
  REQUIRE(s.interval_ms() == 60000);
  s.faster();
  REQUIRE(s.interval_ms() == 30000);
}

TEST_CASE("in folder order next walks forward and honours wrap", "[shell][slideshow]") {
  slideshow s;
  s.start(3, 0, 1);
  REQUIRE(s.next(0, true) == 1u);
  REQUIRE(s.next(1, true) == 2u);
  REQUIRE(s.next(2, true) == 0u);
  REQUIRE_FALSE(s.next(2, false).has_value());
  slideshow empty;
  empty.start(0, 0, 1);
  REQUIRE_FALSE(empty.next(0, true).has_value());
}

TEST_CASE("shuffle visits every item once per round, starting from the current",
          "[shell][slideshow]") {
  slideshow s;
  s.start(50, 17, 42);
  s.toggle_shuffle(17, 42);
  REQUIRE(s.shuffled());
  std::set<std::uint32_t> seen = {17};
  std::uint32_t cur = 17;
  for (int i = 0; i < 49; ++i) {
    const auto n = s.next(cur, true);
    REQUIRE(n.has_value());
    REQUIRE(*n < 50u);
    REQUIRE(seen.insert(*n).second);  // no repeat inside a round
    cur = *n;
  }
  REQUIRE(seen.size() == 50);
  REQUIRE_FALSE(s.next(cur, false).has_value());  // round over, no wrap
  REQUIRE(s.next(cur, true) == 17u);             // wrap goes round again

  // Same seed, same order: the shuffle is reproducible for a test.
  slideshow a;
  slideshow b;
  a.start(20, 0, 7);
  b.start(20, 0, 7);
  a.toggle_shuffle(0, 7);
  b.toggle_shuffle(0, 7);
  std::uint32_t ca = 0;
  std::uint32_t cb = 0;
  for (int i = 0; i < 19; ++i) {
    ca = *a.next(ca, true);
    cb = *b.next(cb, true);
    REQUIRE(ca == cb);
  }
  s.toggle_shuffle(cur, 1);
  REQUIRE_FALSE(s.shuffled());
  REQUIRE(s.next(3, true) == 4u);
}

TEST_CASE("a listing change under a running shuffle rebuilds the order", "[shell][slideshow]") {
  slideshow s;
  s.start(5, 0, 3);
  s.toggle_shuffle(0, 3);
  s.set_count(3, 1);
  std::set<std::uint32_t> seen = {1};
  std::uint32_t cur = 1;
  for (int i = 0; i < 2; ++i) {
    cur = *s.next(cur, false);
    REQUIRE(cur < 3u);
    REQUIRE(seen.insert(cur).second);
  }
}

TEST_CASE("an opening clip is not finished; paused and ended are", "[shell][slideshow]") {
  using m = slideshow::media;
  REQUIRE(slideshow::media_finished(m::none));
  REQUIRE_FALSE(slideshow::media_finished(m::opening));
  REQUIRE_FALSE(slideshow::media_finished(m::playing));
  REQUIRE(slideshow::media_finished(m::paused));
  REQUIRE(slideshow::media_finished(m::finished));
  slideshow s;
  s.start(3, 0, 1);
  for (int i = 0; i < 20; ++i) s.faster();  // 1 s: shorter than a 4K open
  REQUIRE_FALSE(s.should_advance(5000, slideshow::media_finished(m::opening)));
  REQUIRE(s.should_advance(5000, slideshow::media_finished(m::finished)));
}

TEST_CASE("advance waits for the later of the interval and the clip", "[shell][slideshow]") {
  slideshow s;
  REQUIRE_FALSE(s.should_advance(10000, true));  // not running
  s.start(10, 0, 1);
  REQUIRE_FALSE(s.should_advance(3999, true));   // still, interval not up
  REQUIRE(s.should_advance(4000, true));
  REQUIRE_FALSE(s.should_advance(9000, false));  // clip still playing past the interval
  REQUIRE(s.should_advance(9000, true));         // clip ended after the interval
  s.toggle_pause();
  REQUIRE_FALSE(s.should_advance(9000, true));   // paused never advances
  s.toggle_pause();
  s.toggle_blackout();
  REQUIRE(s.blackout());
  s.stop();
  REQUIRE_FALSE(s.active());
  REQUIRE_FALSE(s.blackout());
}
