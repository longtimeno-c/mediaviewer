// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #529: the speed and time left under a transfer's progress (eta.h).
#include "catch_compat.h"
#if !defined(MV_CATCH2_AMALGAMATED)
#include <catch2/catch_approx.hpp>
#endif

#include <chrono>
#include <cstdint>

#include "addons/driftwire/eta.h"

namespace sp = mv::driftwire;
using Catch::Approx;

namespace {
using clock_t_ = sp::transfer_meter::clock;
constexpr std::uint64_t kMB = 1000 * 1000;

// `rate` bytes a second for `seconds`, a chunk every 250 ms, from `bytes`.
std::uint64_t feed(sp::transfer_meter& m, clock_t_::time_point& t, std::uint64_t bytes, double rate, double seconds) {
  for (double s = 0; s < seconds; s += 0.25) {
    t += std::chrono::milliseconds(250);
    bytes += static_cast<std::uint64_t>(rate * 0.25);
    m.add(t, bytes);
  }
  return bytes;
}
}  // namespace

TEST_CASE("driftwire eta: a steady transfer settles on its rate and counts down", "[driftwire]") {
  sp::transfer_meter m;
  auto t = clock_t_::now();
  m.add(t, 0);
  CHECK(m.rate() == 0);
  CHECK(m.seconds_left(400 * kMB) < 0);
  std::uint64_t b = feed(m, t, 0, 10.0 * kMB, 2.0);
  CHECK_FALSE(m.ready());  // not before 3 s
  b = feed(m, t, b, 10.0 * kMB, 4.0);
  REQUIRE(m.ready());
  CHECK(m.rate() == Approx(10.0 * kMB).epsilon(0.02));
  double last = m.seconds_left(400 * kMB);
  CHECK(last == Approx((400.0 * kMB - static_cast<double>(b)) / (10.0 * kMB)).epsilon(0.05));
  for (int i = 0; i < 40; ++i) {
    b = feed(m, t, b, 10.0 * kMB, 0.25);
    const double now_left = m.seconds_left(400 * kMB);
    CHECK(now_left < last);  // down, never up, at a steady rate
    last = now_left;
  }
}

TEST_CASE("driftwire eta: a change of rate is followed, without the old one lingering", "[driftwire]") {
  sp::transfer_meter m;
  auto t = clock_t_::now();
  m.add(t, 0);
  std::uint64_t b = feed(m, t, 0, 10.0 * kMB, 10.0);
  b = feed(m, t, b, 2.0 * kMB, 15.0);
  CHECK(m.rate() == Approx(2.0 * kMB).epsilon(0.10));
  // A slow trickle: the time left grows with it.
  const double before = m.seconds_left(b + 100 * kMB);
  b = feed(m, t, b, 0.5 * kMB, 4.0);
  CHECK(m.seconds_left(b + 100 * kMB) > before);
}

TEST_CASE("driftwire eta: little moved is no estimate, and a new transfer starts over", "[driftwire]") {
  sp::transfer_meter m;
  auto t = clock_t_::now();
  m.add(t, 0);
  feed(m, t, 0, 100 * 1000, 4.0);  // 400 KB in 4 s: under the half megabyte an estimate waits for
  CHECK_FALSE(m.ready());
  sp::transfer_meter n;
  n.add(t, 0);
  std::uint64_t b = feed(n, t, 0, 5.0 * kMB, 6.0);
  REQUIRE(n.ready());
  t += std::chrono::seconds(1);
  n.add(t, 1000);  // fewer bytes: another transfer
  CHECK_FALSE(n.ready());
  b = feed(n, t, 1000, 5.0 * kMB, 0.5);
  CHECK_FALSE(n.ready());
  n.restart();
  CHECK(n.rate() == 0);
  (void)b;
}

TEST_CASE("driftwire eta: the time left reads in rounded words", "[driftwire]") {
  CHECK(sp::time_left_text(-1).empty());
  CHECK(sp::time_left_text(4) == "a few seconds left");
  CHECK(sp::time_left_text(23) == "about 25 seconds left");
  CHECK(sp::time_left_text(70) == "about a minute left");
  CHECK(sp::time_left_text(4 * 60 + 10) == "about 4 min left");
  CHECK(sp::time_left_text(59 * 60) == "about 59 min left");
  CHECK(sp::time_left_text(3600 + 22 * 60) == "about 1 h 20 min left");
  CHECK(sp::time_left_text(2 * 3600 + 60) == "about 2 h left");
}
