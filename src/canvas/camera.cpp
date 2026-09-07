// SPDX-License-Identifier: GPL-2.0-or-later
#include "canvas/camera.h"

#include <algorithm>
#include <cmath>

namespace mv::canvas {

namespace {

constexpr float kMinZoom = 0.02f;
constexpr float kMaxZoom = 64.0f;
constexpr float kWheelFactor = 1.15f;

}  // namespace

void camera::reset() noexcept { *this = camera{}; }

float camera::fit_zoom(float image_w, float image_h, float window_w, float window_h) noexcept {
  if (image_w <= 0.0f || image_h <= 0.0f || window_w <= 0.0f || window_h <= 0.0f) return 1.0f;
  return std::min(window_w / image_w, window_h / image_h);
}

void camera::clamp_zoom(float image_w, float image_h, float window_w, float window_h) noexcept {
  const float fit = fit_zoom(image_w, image_h, window_w, window_h);
  const float lo = std::min(kMinZoom, fit * 0.25f);
  target_zoom_ = std::clamp(target_zoom_, lo, kMaxZoom);
}

void camera::fit(float image_w, float image_h, float window_w, float window_h,
                 bool immediate) noexcept {
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
  target_zoom_ = 1.0f;
  fit_mode_ = false;
}

void camera::wheel_toward(float mouse_x, float mouse_y, float notches, float window_w,
                          float window_h, float image_w, float image_h) noexcept {
  if (notches == 0.0f) return;
  const float old_zoom = target_zoom_;
  target_zoom_ = old_zoom * std::pow(kWheelFactor, notches);
  clamp_zoom(image_w, image_h, window_w, window_h);
  const float img_x = target_pan_x_ + (mouse_x - window_w * 0.5f) / old_zoom;
  const float img_y = target_pan_y_ + (mouse_y - window_h * 0.5f) / old_zoom;
  target_pan_x_ = img_x - (mouse_x - window_w * 0.5f) / target_zoom_;
  target_pan_y_ = img_y - (mouse_y - window_h * 0.5f) / target_zoom_;
  fit_mode_ = false;
}

void camera::drag_begin() noexcept {
  dragging_ = true;
  fit_mode_ = false;
  pan_vx_ = pan_vy_ = 0.0f;
}

void camera::drag_delta(float dx_screen, float dy_screen) noexcept {
  if (!dragging_ || zoom_ == 0.0f) return;
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
  spring_step(pan_x_, pan_vx_, target_pan_x_, dt);
  spring_step(pan_y_, pan_vy_, target_pan_y_, dt);
  spring_step(zoom_, zoom_v_, target_zoom_, dt);
}

bool camera::moving() const noexcept {
  if (dragging_) return true;
  return !spring_settled(pan_x_, pan_vx_, target_pan_x_) ||
         !spring_settled(pan_y_, pan_vy_, target_pan_y_) ||
         !spring_settled(zoom_, zoom_v_, target_zoom_, 0.0005f, 0.01f);
}

}  // namespace mv::canvas
