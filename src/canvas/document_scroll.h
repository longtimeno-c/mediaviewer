// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Scrolling a document (docs/design/04 "Scrolling a document"): what one wheel or
// trackpad event over a PDF or DOCX does to the view. Shared by both hosts
// (`scroll_document` in main.cpp, `-scrollDocument:` in main_mac.mm) so the
// page-turn rule is one piece of code (D9). Pure: no clock reads, no I/O; the
// host passes its own clock and what the render thread last published.
//
// - Away from an edge the event pans. A wheel notch moves the camera *target*
//   and the canvas spring (ω=18, ζ=1) carries the view there; a trackpad delta
//   (and its momentum) is `direct` and moves the view with no spring, so the
//   page stays under the fingers.
// - Pushing into the top or bottom edge turns the page at once (no hold):
//   a notch, or any trackpad travel when the page is taller than the canvas.
//   Momentum runs on through the document. When the whole page is on screen
//   (fit) a trackpad turns one page per kFitTurnFraction of the canvas height.
// - After a turn the edges the render thread published are the old page's
//   until the new one lands, so nothing turns again until it has acknowledged
//   the turn (`land_ack`), or kLandTimeout has passed (a page that failed).
#pragma once

#include <cstdint>

namespace mv::canvas {

struct document_scroll_input {
  double dx = 0.0;  // screen pixels; positive moves the view right
  double dy = 0.0;  // screen pixels; positive moves the view down
  bool precise = false;  // trackpad / precision touchpad (not whole wheel notches)
  double now = 0.0;      // seconds, any monotonic host clock
  unsigned edges = 0;    // camera::vertical_edges as the render thread last published
  float canvas_h = 0.0f; // usable canvas height, pixels
  std::uint32_t land_ack = 0;  // the page_land_seq the render thread has applied
};

struct document_scroll_output {
  // Whole screen pixels to add to input_snapshot::scroll_px_* (spring) or
  // scroll_direct_px_* (direct). The fraction carries to the next event.
  std::int64_t px_x = 0;
  std::int64_t px_y = 0;
  bool direct = false;
  int turn = 0;  // +1 next page (lands at its top), -1 previous (lands at its bottom)
};

class document_scroller {
 public:
  static constexpr float kFitTurnFraction = 1.0f / 3.0f;
  static constexpr double kGapSeconds = 0.3;   // a pause starts a new push
  static constexpr double kLandTimeout = 0.5;  // give up waiting for the land ack

  [[nodiscard]] document_scroll_output feed(const document_scroll_input& in) noexcept;

  // The host turned a page and published `land_seq` as page_land_seq.
  void turned(std::uint32_t land_seq, double now) noexcept;

  void reset() noexcept { *this = document_scroller{}; }

  [[nodiscard]] bool waiting_for_land() const noexcept { return waiting_; }

 private:
  double overscroll_ = 0.0;
  double last_ = -1.0;
  double rem_x_ = 0.0;
  double rem_y_ = 0.0;
  bool rem_direct_ = false;
  bool waiting_ = false;
  std::uint32_t wait_seq_ = 0;
  double wait_since_ = 0.0;
};

}  // namespace mv::canvas
