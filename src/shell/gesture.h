// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Touch and trackpad gestures (issue #212, docs/design/16 "Touch and trackpad").
// Pure and shared by both hosts. A gesture never gets its own camera path: a
// pinch becomes wheel units toward a point (input_snapshot::wheel_total, so
// camera::wheel_toward and the ω=18, ζ=1 springs do the zoom), a pan becomes
// screen pixels (input_snapshot::scroll_px_*, camera::pan_by_screen), and a
// swipe is the same folder step A / D take. Nothing here touches a thread.
#pragma once

#include <cmath>
#include <cstdint>

namespace mv::shell {

// One wheel notch is a 15 % zoom step (canvas/camera.cpp kWheelFactor) and 120
// native units (WHEEL_DELTA). Kept here, not included, so this header stays
// free of the canvas.
inline constexpr double kGestureWheelFactor = 1.15;
inline constexpr double kGestureWheelUnits = 120.0;

// Pinch scale → wheel units. A pinch arrives as many small ratios (a touch
// contact's distance over the last one, AppKit's magnification); truncating
// each would lose a slow pinch entirely, so the fraction carries over.
class pinch_to_wheel {
 public:
  // `scale` is new size / old size (> 1 zooms in). Returns whole wheel units to
  // add to wheel_total; 0 when the step is still below one unit.
  [[nodiscard]] std::int64_t feed(double scale) noexcept {
    if (!(scale > 0.0) || !std::isfinite(scale)) return 0;
    remainder_ += std::log(scale) / std::log(kGestureWheelFactor) * kGestureWheelUnits;
    const double whole = std::trunc(remainder_);
    remainder_ -= whole;
    return static_cast<std::int64_t>(whole);
  }
  void reset() noexcept { remainder_ = 0.0; }

 private:
  double remainder_ = 0.0;
};

// Touchpad / tilt wheel: one notch of travel. A tilt click is one item; a
// two-finger sideways swipe crosses it within a few millimetres.
inline constexpr double kWheelSwipeThreshold = kGestureWheelUnits;
// Quiet time that ends a touchpad swipe (longer than its event cadence, short
// enough that a second deliberate swipe is a second step).
inline constexpr std::uint64_t kWheelSwipeGapMs = 250;

// Swipe-next: horizontal travel that crosses `threshold` and dominates the
// vertical travel steps one item, once per gesture. `dx` is positive when the
// content moves left (fingers move left: the next item comes in from the
// right), so the result is +1 next, -1 previous, 0 nothing.
//
// A gesture ends at end() (a touch lifts) or after `gap_ms` with no input (a
// touchpad or tilt wheel, which sends no end). The latch holds until then, so
// a touchpad's inertia does not run on through the folder.
class swipe_detector {
 public:
  // The touchpad / tilt-wheel detector, in wheel units.
  constexpr swipe_detector() noexcept : swipe_detector(kWheelSwipeThreshold, kWheelSwipeGapMs) {}
  constexpr swipe_detector(double threshold, std::uint64_t gap_ms) noexcept
      : threshold_(threshold), gap_ms_(gap_ms) {}

  [[nodiscard]] int feed(double dx, double dy, std::uint64_t now_ms) noexcept {
    if (started_ && now_ms - last_ms_ > gap_ms_) end();
    started_ = true;
    last_ms_ = now_ms;
    if (latched_) return 0;
    travel_x_ += dx;
    travel_y_ += dy;
    // Twice the vertical: a diagonal or a vertical scroll (the wheel zooms)
    // with some sideways drift never turns the item.
    if (std::abs(travel_x_) < threshold_ || std::abs(travel_x_) < 2.0 * std::abs(travel_y_)) {
      return 0;
    }
    latched_ = true;
    return travel_x_ > 0.0 ? 1 : -1;
  }
  // The gesture became something else (a pinch): no step until it ends.
  void suppress() noexcept {
    started_ = true;
    latched_ = true;
  }
  void end() noexcept {
    started_ = false;
    latched_ = false;
    travel_x_ = travel_y_ = 0.0;
  }
  [[nodiscard]] bool latched() const noexcept { return latched_; }

 private:
  double threshold_;
  std::uint64_t gap_ms_;
  double travel_x_ = 0.0;
  double travel_y_ = 0.0;
  std::uint64_t last_ms_ = 0;
  bool started_ = false;
  bool latched_ = false;
};

// Touchscreen swipe distance: 64 DIP, so it is a deliberate stroke at any DPI
// and well past the system's pan start slop.
[[nodiscard]] inline double touch_swipe_threshold_px(float dpi_scale) noexcept {
  return 64.0 * (dpi_scale > 1.0f ? static_cast<double>(dpi_scale) : 1.0);
}

}  // namespace mv::shell
