// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Scrolling a document (issue #353, docs/design/04 "Scrolling a document"): the
// shared page-turn rule, the wheel glide, the trackpad's direct pan, and the
// one "Press T" filmstrip hint.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdint>

#include "canvas/camera.h"
#include "canvas/document_scroll.h"
#include "shell/filmstrip_hint.h"

using Catch::Matchers::WithinAbs;
using mv::canvas::camera;
using mv::canvas::document_scroll_input;
using mv::canvas::document_scroller;

namespace {

constexpr unsigned kTop = camera::kEdgeTop;
constexpr unsigned kBottom = camera::kEdgeBottom;
constexpr unsigned kBoth = kTop | kBottom;

document_scroll_input event(double dy, bool precise, double now, unsigned edges,
                            std::uint32_t ack = 0) {
  document_scroll_input in;
  in.dy = dy;
  in.precise = precise;
  in.now = now;
  in.edges = edges;
  in.canvas_h = 900.0f;
  in.land_ack = ack;
  return in;
}

// A page taller than the window, zoomed in: 800 x 1100 image at 2x in 1000 x 700.
camera zoomed_page() {
  camera cam;
  cam.fit(800.0f, 1100.0f, 1000.0f, 700.0f, true);
  cam.set_zoom(2.0f, 800.0f, 1100.0f, 1000.0f, 700.0f);
  for (int i = 0; i < 240; ++i) cam.step(1.0f / 120.0f);
  return cam;
}

}  // namespace

TEST_CASE("document scroll: away from an edge the event pans, a wheel on the spring",
          "[canvas][document_scroll]") {
  document_scroller s;
  auto out = s.feed(event(90.0, false, 1.0, kTop));
  REQUIRE(out.turn == 0);
  REQUIRE(out.px_y == 90);
  REQUIRE_FALSE(out.direct);
  // A trackpad's fractions carry, and it pans directly.
  out = s.feed(event(2.5, true, 1.01, 0));
  REQUIRE(out.direct);
  REQUIRE(out.px_y == 2);
  out = s.feed(event(2.5, true, 1.02, 0));
  REQUIRE(out.px_y == 3);
}

TEST_CASE("document scroll: a notch past the edge turns at once, no 60 px hold",
          "[canvas][document_scroll]") {
  document_scroller s;
  // At fit (both edges) every notch is a page.
  auto out = s.feed(event(90.0, false, 1.0, kBoth));
  REQUIRE(out.turn == 1);
  REQUIRE(out.px_y == 0);
  s.turned(1, 1.0);
  // Until the render thread acknowledges the new page, nothing turns again
  // (the published edges are the old page's).
  out = s.feed(event(90.0, false, 1.05, kBoth, /*ack=*/0));
  REQUIRE(out.turn == 0);
  REQUIRE(out.px_y == 0);
  // Landed: the next notch turns the next page; a burst is not latched.
  out = s.feed(event(90.0, false, 1.10, kBoth, /*ack=*/1));
  REQUIRE(out.turn == 1);
  s.turned(2, 1.10);
  // Upward from the top turns back.
  out = s.feed(event(-90.0, false, 1.20, kBoth, /*ack=*/2));
  REQUIRE(out.turn == -1);
}

TEST_CASE("document scroll: trackpad momentum runs through pages", "[canvas][document_scroll]") {
  document_scroller s;
  std::uint32_t seq = 0;
  int turns = 0;
  double t = 1.0;
  // Zoomed in: the page is taller than the canvas. A fling reaches the bottom,
  // turns, the new page lands at its top and the same momentum keeps scrolling.
  unsigned edges = kTop;
  double travelled = 0.0;
  for (int i = 0; i < 400; ++i, t += 1.0 / 120.0) {
    const auto out = s.feed(event(30.0, true, t, edges, seq));
    REQUIRE(out.direct);
    travelled += static_cast<double>(out.px_y);
    if (out.turn != 0) {
      ++turns;
      s.turned(++seq, t);
      edges = kTop;  // the new page lands at its top
      travelled = 0.0;
    } else if (travelled >= 1500.0) {
      edges = kBottom;  // scrolled down to the bottom of this page
    } else {
      edges = 0;
    }
  }
  REQUIRE(turns >= 5);
}

TEST_CASE("document scroll: at fit a trackpad pages per a third of the canvas",
          "[canvas][document_scroll]") {
  document_scroller s;
  std::uint32_t seq = 0;
  int turns = 0;
  double t = 1.0;
  // 3000 px of travel at 900 px canvas: 300 px per page, about ten pages.
  for (int i = 0; i < 100; ++i, t += 1.0 / 120.0) {
    const auto out = s.feed(event(30.0, true, t, kBoth, seq));
    if (out.turn != 0) {
      ++turns;
      s.turned(++seq, t);
    }
  }
  REQUIRE(turns >= 8);
  REQUIRE(turns <= 10);
}

TEST_CASE("document scroll: a page that never lands does not wedge scrolling",
          "[canvas][document_scroll]") {
  document_scroller s;
  REQUIRE(s.feed(event(90.0, false, 1.0, kBoth)).turn == 1);
  s.turned(7, 1.0);
  REQUIRE(s.feed(event(90.0, false, 1.2, kBoth, 0)).turn == 0);
  REQUIRE(s.waiting_for_land());
  REQUIRE(s.feed(event(90.0, false, 1.0 + document_scroller::kLandTimeout + 0.01, kBoth, 0)).turn == 1);
}

TEST_CASE("document scroll: a wheel notch glides with no visible jump", "[canvas][document_scroll]") {
  camera cam = zoomed_page();
  const float start = cam.pan_y();
  // One notch is a tenth of the canvas: 70 screen px = 35 image px at 2x.
  cam.pan_by_screen(0.0f, 70.0f, 800.0f, 1100.0f, 1000.0f, 700.0f);
  float last = start;
  float max_step = 0.0f;
  int frames = 0;
  while (cam.moving() && frames < 600) {
    cam.step(1.0f / 60.0f);
    max_step = std::max(max_step, std::fabs(cam.pan_y() - last) * 2.0f);  // screen px
    last = cam.pan_y();
    ++frames;
  }
  // It moved over several frames (not one), none of them more than ~a third of
  // the notch at 60 Hz, and it settles so idle returns to 0 presents.
  REQUIRE(frames > 4);
  REQUIRE(max_step < 70.0f * 0.4f);
  REQUIRE_FALSE(cam.moving());
  REQUIRE_THAT(cam.pan_y() - start, WithinAbs(35.0f, 0.06f));
}

TEST_CASE("document scroll: a trackpad delta moves the view at once, clamped",
          "[canvas][document_scroll]") {
  camera cam = zoomed_page();
  const float start = cam.pan_y();
  cam.scroll_direct(0.0f, 20.0f, 800.0f, 1100.0f, 1000.0f, 700.0f);
  REQUIRE_THAT(cam.pan_y() - start, WithinAbs(10.0f, 1e-3f));
  REQUIRE_THAT(cam.target_pan_y() - start, WithinAbs(10.0f, 1e-3f));
  REQUIRE_FALSE(cam.moving());
  // Clamped at the bottom: the view stops where the page ends.
  cam.scroll_direct(0.0f, 100000.0f, 800.0f, 1100.0f, 1000.0f, 700.0f);
  REQUIRE((cam.vertical_edges(1100.0f, 700.0f) & camera::kEdgeBottom) != 0);
  REQUIRE_THAT(cam.pan_y(), WithinAbs(cam.target_pan_y(), 1e-3f));
  // At fit nothing moves (the opening view is locked).
  camera fitted;
  fitted.fit(800.0f, 1100.0f, 1000.0f, 700.0f, true);
  const float y = fitted.pan_y();
  fitted.scroll_direct(0.0f, 50.0f, 800.0f, 1100.0f, 1000.0f, 700.0f);
  REQUIRE(fitted.pan_y() == y);
}

TEST_CASE("filmstrip hint: one hint, due over a document with the strip up, until T or Got it",
          "[shell][filmstrip_hint]") {
  using mv::shell::strip_hint_subject;
  using mv::shell::strip_hint_due;
  using mv::shell::strip_hint_learned_by_toggle;
  using mv::shell::strip_hint_text;
  REQUIRE(strip_hint_text(strip_hint_subject::document) == "Press T to hide the filmstrip and scroll");
  REQUIRE(strip_hint_text(strip_hint_subject::clip) ==
          "Press T to hide the filmstrip and skip 2 s with \xE2\x86\x90 \xE2\x86\x92");
  REQUIRE(strip_hint_due(strip_hint_subject::clip, true, false));
  REQUIRE_FALSE(strip_hint_due(strip_hint_subject::clip, true, true));  // one learned flag
  REQUIRE(strip_hint_learned_by_toggle(strip_hint_subject::clip, true));
  REQUIRE(strip_hint_text(strip_hint_subject::none).empty());
  REQUIRE(strip_hint_due(strip_hint_subject::document, true, false));
  REQUIRE_FALSE(strip_hint_due(strip_hint_subject::document, false, false));  // strip hidden
  REQUIRE_FALSE(strip_hint_due(strip_hint_subject::document, true, true));    // learned
  REQUIRE_FALSE(strip_hint_due(strip_hint_subject::none, true, false));
  // T hiding the strip over a document teaches it; T showing it does not.
  REQUIRE(strip_hint_learned_by_toggle(strip_hint_subject::document, true));
  REQUIRE_FALSE(strip_hint_learned_by_toggle(strip_hint_subject::document, false));
  REQUIRE_FALSE(strip_hint_learned_by_toggle(strip_hint_subject::none, true));
  REQUIRE(mv::shell::page_notice(0, 50) == "Page 1 of 50");
  REQUIRE(mv::shell::page_notice(4, 50) == "Page 5 of 50");
}
