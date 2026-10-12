// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "canvas/document_scroll.h"

#include <cmath>

#include "canvas/camera.h"

namespace mv::canvas {

document_scroll_output document_scroller::feed(const document_scroll_input& in) noexcept {
  document_scroll_output out;
  out.direct = in.precise;
  if (waiting_ && (in.land_ack == wait_seq_ || in.now - wait_since_ >= kLandTimeout)) {
    waiting_ = false;
  }
  const bool gap = last_ < 0.0 || in.now - last_ > kGapSeconds;
  last_ = in.now;
  // A wheel's fraction is not a trackpad's: switching kind drops the carry.
  if (in.precise != rem_direct_) {
    rem_x_ = rem_y_ = 0.0;
    rem_direct_ = in.precise;
  }

  double px = in.dx;
  double py = in.dy;
  if (py != 0.0 && std::fabs(py) >= std::fabs(px)) {
    const bool down = py > 0.0;
    const unsigned edge = down ? camera::kEdgeBottom : camera::kEdgeTop;
    if (waiting_) {
      // The published edges are still the old page's: neither pan it nor turn
      // again until the new page is on screen.
      overscroll_ = 0.0;
      py = 0.0;
    } else if ((in.edges & edge) != 0) {
      if (gap || (overscroll_ > 0.0) != down) overscroll_ = 0.0;
      overscroll_ += py;
      // The whole page on screen (fit): a trackpad turns per a third of the
      // canvas of travel so a fling pages rather than skipping to the end. A
      // notch, or any push past the edge of a page taller than the canvas,
      // turns at once.
      const bool whole_page = (in.edges & (camera::kEdgeTop | camera::kEdgeBottom)) ==
                              (camera::kEdgeTop | camera::kEdgeBottom);
      const double threshold =
          in.precise && whole_page ? static_cast<double>(kFitTurnFraction) * in.canvas_h : 0.0;
      if (std::fabs(overscroll_) > threshold) {
        out.turn = down ? 1 : -1;
        overscroll_ = 0.0;
      }
      py = 0.0;
    } else {
      overscroll_ = 0.0;
    }
  }

  rem_x_ += px;
  rem_y_ += py;
  out.px_x = static_cast<std::int64_t>(std::trunc(rem_x_));
  out.px_y = static_cast<std::int64_t>(std::trunc(rem_y_));
  rem_x_ -= static_cast<double>(out.px_x);
  rem_y_ -= static_cast<double>(out.px_y);
  return out;
}

void document_scroller::turned(std::uint32_t land_seq, double now) noexcept {
  waiting_ = true;
  wait_seq_ = land_seq;
  wait_since_ = now;
  overscroll_ = 0.0;
  rem_y_ = 0.0;
}

}  // namespace mv::canvas
