// SPDX-License-Identifier: GPL-2.0-or-later
// Pan/zoom camera. 1.0 zoom is 100 % (one image pixel per screen pixel).
// Pan is the image pixel sitting at the window centre.
#pragma once

#include "canvas/spring.h"

namespace mv::canvas {

class camera {
 public:
  void reset() noexcept;

  // Fit the whole image in the window. `immediate` snaps (load, resize);
  // otherwise the springs settle.
  void fit(float image_w, float image_h, float window_w, float window_h,
           bool immediate) noexcept;

  // 100 %. Keeps the current centre.
  void one_to_one() noexcept;

  // Wheel zoom toward the cursor. `notches` is the usual WHEEL_DELTA units
  // already divided to ±1 per detent.
  void wheel_toward(float mouse_x, float mouse_y, float notches, float window_w,
                    float window_h, float image_w, float image_h) noexcept;

  void drag_begin() noexcept;
  void drag_delta(float dx_screen, float dy_screen) noexcept;
  void drag_end() noexcept;

  void step(float dt) noexcept;

  [[nodiscard]] bool moving() const noexcept;
  [[nodiscard]] bool fit_mode() const noexcept { return fit_mode_; }
  [[nodiscard]] bool dragging() const noexcept { return dragging_; }

  [[nodiscard]] float pan_x() const noexcept { return pan_x_; }
  [[nodiscard]] float pan_y() const noexcept { return pan_y_; }
  [[nodiscard]] float zoom() const noexcept { return zoom_; }

  [[nodiscard]] float target_pan_x() const noexcept { return target_pan_x_; }
  [[nodiscard]] float target_pan_y() const noexcept { return target_pan_y_; }
  [[nodiscard]] float target_zoom() const noexcept { return target_zoom_; }

  [[nodiscard]] static float fit_zoom(float image_w, float image_h, float window_w,
                                      float window_h) noexcept;

 private:
  void clamp_zoom(float image_w, float image_h, float window_w, float window_h) noexcept;

  float pan_x_ = 0.0f;
  float pan_y_ = 0.0f;
  float pan_vx_ = 0.0f;
  float pan_vy_ = 0.0f;
  float zoom_ = 1.0f;
  float zoom_v_ = 0.0f;
  float target_pan_x_ = 0.0f;
  float target_pan_y_ = 0.0f;
  float target_zoom_ = 1.0f;
  bool fit_mode_ = true;
  bool dragging_ = false;
};

}  // namespace mv::canvas
