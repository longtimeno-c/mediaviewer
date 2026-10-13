// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>

#include "canvas/camera.h"
#include "shell/gesture.h"

using mv::shell::pinch_to_wheel;
using mv::shell::swipe_detector;

TEST_CASE("a pinch is wheel units: one 15 % step is one notch", "[shell][gesture]") {
  pinch_to_wheel pinch;
  REQUIRE(pinch.feed(1.15) == 120);
  const auto back = pinch.feed(1.0 / 1.15);  // log(1/x) may round a hair short
  REQUIRE(back >= -120);
  REQUIRE(back <= -119);
  pinch.reset();
  REQUIRE(pinch.feed(1.0) == 0);
  REQUIRE(pinch.feed(0.0) == 0);
  REQUIRE(pinch.feed(-2.0) == 0);
  REQUIRE(pinch.feed(std::nan("")) == 0);
}

TEST_CASE("a slow pinch is not lost to truncation", "[shell][gesture]") {
  // 200 contact updates of 0.07 % each: every one is under a wheel unit, the
  // whole is a 15 % step.
  pinch_to_wheel pinch;
  const double step = std::pow(1.15, 1.0 / 200.0);
  std::int64_t total = 0;
  for (int i = 0; i < 200; ++i) {
    const auto units = pinch.feed(step);
    REQUIRE(std::abs(units) <= 1);
    total += units;
  }
  REQUIRE(total >= 119);
  REQUIRE(total <= 120);
  // Pinching back out returns to where it started.
  for (int i = 0; i < 200; ++i) total += pinch.feed(1.0 / step);
  REQUIRE(std::abs(total) <= 1);
}

TEST_CASE("a pinch drives the wheel's camera springs toward the fingers", "[shell][gesture][camera]") {
  // 4000x3000 in 1000x750: fit is 25 %. A 2x pinch about the top-left quarter
  // zooms the target there, and the springs settle onto it.
  mv::canvas::camera cam;
  cam.fit(4000, 3000, 1000, 750, true);
  pinch_to_wheel pinch;
  const auto units = pinch.feed(2.0);
  cam.wheel_toward(250, 187.5f, static_cast<float>(units) / 120.0f, 1000, 750, 4000, 3000);
  REQUIRE_FALSE(cam.fit_mode());
  REQUIRE(std::abs(cam.target_zoom() - 0.5f) < 0.01f);
  REQUIRE(cam.target_pan_x() < 2000.0f);
  REQUIRE(cam.target_pan_y() < 1500.0f);
  REQUIRE(cam.moving());
  for (int i = 0; i < 240 && cam.moving(); ++i) cam.step(1.0f / 120.0f);
  REQUIRE_FALSE(cam.moving());
  REQUIRE(std::abs(cam.zoom() - cam.target_zoom()) < 1e-3f);
}

TEST_CASE("a swipe steps once, past the threshold, when it is horizontal", "[shell][gesture]") {
  swipe_detector swipe(100.0, 250);
  REQUIRE(swipe.feed(40, 0, 1000) == 0);
  REQUIRE(swipe.feed(40, 5, 1010) == 0);
  REQUIRE(swipe.feed(40, 5, 1020) == 1);  // content left: next
  // Latched: the rest of the stroke and its inertia do nothing.
  REQUIRE(swipe.feed(400, 0, 1030) == 0);
  REQUIRE(swipe.latched());
  swipe.end();
  REQUIRE(swipe.feed(-150, 0, 1040) == -1);  // content right: previous
}

TEST_CASE("a vertical or diagonal stroke never turns the item", "[shell][gesture]") {
  swipe_detector swipe(100.0, 250);
  REQUIRE(swipe.feed(0, 300, 0) == 0);
  REQUIRE(swipe.feed(150, 0, 10) == 0);  // 150 sideways against 300 down
  swipe.end();
  REQUIRE(swipe.feed(120, 80, 20) == 0);  // diagonal
  REQUIRE(swipe.feed(60, 0, 30) == 1);    // then clearly sideways
}

TEST_CASE("a touchpad swipe ends after a quiet gap, not on a lift", "[shell][gesture]") {
  swipe_detector swipe(mv::shell::kWheelSwipeThreshold, mv::shell::kWheelSwipeGapMs);
  // A tilt click is one notch: one item per click, not per repeat.
  REQUIRE(swipe.feed(120, 0, 0) == 1);
  REQUIRE(swipe.feed(120, 0, 100) == 0);
  REQUIRE(swipe.feed(120, 0, 200) == 0);  // held: repeats inside the gap
  REQUIRE(swipe.feed(120, 0, 600) == 1);  // a second click after a pause
  // Small touchpad deltas accumulate.
  REQUIRE(swipe.feed(-30, 0, 1000) == 0);
  REQUIRE(swipe.feed(-30, 0, 1010) == 0);
  REQUIRE(swipe.feed(-30, 0, 1020) == 0);
  REQUIRE(swipe.feed(-30, 0, 1030) == -1);
}

TEST_CASE("a pinch suppresses the swipe until the touch ends", "[shell][gesture]") {
  swipe_detector swipe(mv::shell::touch_swipe_threshold_px(1.5f), 1000);
  REQUIRE(mv::shell::touch_swipe_threshold_px(1.5f) == 96.0);
  REQUIRE(mv::shell::touch_swipe_threshold_px(0.5f) == 64.0);
  swipe.suppress();
  REQUIRE(swipe.feed(500, 0, 10) == 0);
  swipe.end();
  REQUIRE(swipe.feed(500, 0, 20) == 1);
}
