// SPDX-License-Identifier: GPL-2.0-or-later
#include "canvas/camera.h"

#include <algorithm>
#include <cmath>

namespace mv::canvas {

namespace {

constexpr float kMaxZoom = 64.0f;
constexpr float kMinZoom = 0.5f;  // 50 % — wheel and presets; Fit may go lower
constexpr float kWheelFactor = 1.15f;
// Relative slack so "already at rest" survives a few frames of float error
// without treating a real zoom-in as the floor.
constexpr float kFitLockEps = 1e-4f;
// Rubber-band past the rest pose (50 %, or fit when already fitted below
// 50 %). The *target* dips; the spring follows. Pop-back starts after the
// wheel has been quiet for kRubberHold seconds.
constexpr float kRubberDip = 0.90f;
constexpr float kRubberMin = 0.84f;
constexpr float kRubberNear = 1.12f;
constexpr float kRubberFollow = 0.5f;
constexpr float kRubberHold = 0.22f;

bool at_or_below(float zoom, float rest) noexcept {
  if (rest <= 0.0f) return false;
  return zoom <= rest * (1.0f + kFitLockEps);
}

float rubber_target(float current, float rest, float notches) noexcept {
  const float floor_z = rest * kRubberMin;
  float z = current;
  if (z >= rest * (1.0f - kFitLockEps)) {
    const float t = std::min(1.0f, std::fabs(notches));
    z = rest * (1.0f - (1.0f - kRubberDip) * t);
  } else {
    z *= std::pow(kWheelFactor, notches * kRubberFollow);
  }
  if (z > rest) z = rest;
  if (z < floor_z) z = floor_z;
  return z;
}

// Wheel rest pose: 50 %, unless Fit has already taken us below that.
float wheel_rest(float fit, bool fitted) noexcept {
  if (fitted && fit > 0.0f && fit < kMinZoom) return fit;
  return kMinZoom;
}

}  // namespace

void camera::clear_rubber() noexcept {
  rubber_held_ = false;
  rubber_idle_ = 0.0f;
  rubber_fit_zoom_ = 0.0f;
}

void camera::reset() noexcept { *this = camera{}; }

float camera::fit_zoom(float image_w, float image_h, float window_w, float window_h) noexcept {
  if (image_w <= 0.0f || image_h <= 0.0f || window_w <= 0.0f || window_h <= 0.0f) return 1.0f;
  return std::min(window_w / image_w, window_h / image_h);
}

void camera::fit(float image_w, float image_h, float window_w, float window_h,
                 bool immediate) noexcept {
  clear_rubber();
  target_zoom_ = fit_zoom(image_w, image_h, window_w, window_h);
  target_pan_x_ = image_w * 0.5f;
  target_pan_y_ = image_h * 0.5f;
  fit_mode_ = true;
  if (immediate) {
    pan_x_ = target_pan_x_;
    pan_y_ = target_pan_y_;
    zoom_ = target_zoom_;
    pan_vx_ = pan_vy_ = zoom_v_ = 0.0f;
  }
}

void camera::one_to_one() noexcept {
  clear_rubber();
  target_zoom_ = 1.0f;
  fit_mode_ = false;
}

void camera::set_zoom(float zoom, float image_w, float image_h, float window_w,
                      float window_h) noexcept {
  if (fit_zoom(image_w, image_h, window_w, window_h) <= 0.0f) return;
  clear_rubber();
  if (zoom < kMinZoom) zoom = kMinZoom;
  if (zoom > kMaxZoom) zoom = kMaxZoom;
  target_zoom_ = zoom;
  fit_mode_ = false;
}

void camera::wheel_toward(float mouse_x, float mouse_y, float notches, float window_w,
                          float window_h, float image_w, float image_h) noexcept {
  if (notches == 0.0f) return;
  const float fit = fit_zoom(image_w, image_h, window_w, window_h);
  if (fit <= 0.0f) return;
  const float rest = wheel_rest(fit, fit_mode_);

  // While rubber-banding the displayed zoom is below the rest pose; zoom-in
  // should lift off from what the user sees, not jump to rest first.
  float from = target_zoom_ > 0.0f ? target_zoom_ : rest;
  if (notches > 0.0f && zoom_ < rest) from = zoom_;

  if (notches > 0.0f) {
    clear_rubber();
    target_zoom_ = from * std::pow(kWheelFactor, notches);
    if (target_zoom_ > kMaxZoom) target_zoom_ = kMaxZoom;
    const float img_x = target_pan_x_ + (mouse_x - window_w * 0.5f) / from;
    const float img_y = target_pan_y_ + (mouse_y - window_h * 0.5f) / from;
    target_pan_x_ = img_x - (mouse_x - window_w * 0.5f) / target_zoom_;
    target_pan_y_ = img_y - (mouse_y - window_h * 0.5f) / target_zoom_;
    fit_mode_ = false;
    return;
  }

  const float unconstrained = from * std::pow(kWheelFactor, notches);
  if (!at_or_below(from, rest) && unconstrained > rest) {
    target_zoom_ = unconstrained;
    if (target_zoom_ > kMaxZoom) target_zoom_ = kMaxZoom;
    const float img_x = target_pan_x_ + (mouse_x - window_w * 0.5f) / from;
    const float img_y = target_pan_y_ + (mouse_y - window_h * 0.5f) / from;
    target_pan_x_ = img_x - (mouse_x - window_w * 0.5f) / target_zoom_;
    target_pan_y_ = img_y - (mouse_y - window_h * 0.5f) / target_zoom_;
    fit_mode_ = false;
    return;
  }

  // Floor: 50 %, or fit when already fitted below 50 %. Dip the *target*.
  target_pan_x_ = image_w * 0.5f;
  target_pan_y_ = image_h * 0.5f;
  const bool rest_is_fit = rest < kMinZoom;
  fit_mode_ = rest_is_fit;
  if (from <= rest * kRubberNear) {
    const float from_z = std::min(zoom_, target_zoom_);
    target_zoom_ = rubber_target(from_z, rest, notches);
    rubber_fit_zoom_ = rest;
    rubber_held_ = true;
    rubber_idle_ = 0.0f;
  } else {
    target_zoom_ = rest;
    clear_rubber();
  }
}

void camera::drag_begin() noexcept {
  if (fit_mode_) return;  // opening view is locked until the user zooms in
  dragging_ = true;
  pan_vx_ = pan_vy_ = 0.0f;
}

void camera::drag_delta(float dx_screen, float dy_screen) noexcept {
  if (!dragging_ || fit_mode_ || zoom_ == 0.0f) return;
  // Dragging is direct: springs would lag the cursor and feel like the image
  // is on a rubber band. Targets and currents move together; velocity stays 0.
  const float dx = dx_screen / zoom_;
  const float dy = dy_screen / zoom_;
  target_pan_x_ -= dx;
  target_pan_y_ -= dy;
  pan_x_ = target_pan_x_;
  pan_y_ = target_pan_y_;
}

void camera::drag_end() noexcept { dragging_ = false; }

void camera::step(float dt) noexcept {
  if (dragging_) return;
  if (rubber_held_) {
    rubber_idle_ += dt;
    if (rubber_idle_ >= kRubberHold && rubber_fit_zoom_ > 0.0f) {
      target_zoom_ = rubber_fit_zoom_;
      rubber_held_ = false;
      rubber_idle_ = 0.0f;
    }
  }
  spring_step(pan_x_, pan_vx_, target_pan_x_, dt);
  spring_step(pan_y_, pan_vy_, target_pan_y_, dt);
  spring_step(zoom_, zoom_v_, target_zoom_, dt);
}

bool camera::moving() const noexcept {
  if (dragging_ || rubber_held_) return true;
  return !spring_settled(pan_x_, pan_vx_, target_pan_x_) ||
         !spring_settled(pan_y_, pan_vy_, target_pan_y_) ||
         !spring_settled(zoom_, zoom_v_, target_zoom_, 0.0005f, 0.01f);
}

}  // namespace mv::canvas
