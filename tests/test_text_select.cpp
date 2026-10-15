// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Text in images (docs/design/16 "Text in images"): selecting text on the
// canvas as Photos does -- press-drag, double- and triple-click, a click that
// clears -- and when words read from a still go stale.
#include <catch2/catch_test_macros.hpp>

#include "shell/text_select.h"

namespace {

using mv::shell::text_highlight;
using mv::shell::text_layout;
using mv::shell::text_select;
using mv::shell::text_word;

text_word word(float x0, float y0, float x1, float y1, std::uint32_t line, const char* text) {
  text_word w;
  w.x0 = x0;
  w.y0 = y0;
  w.x1 = x1;
  w.y1 = y1;
  w.line = line;
  w.text = text;
  return w;
}

// "Hello world" / "second line here".
text_layout two_lines() {
  text_layout l;
  l.words = {word(0.10f, 0.10f, 0.30f, 0.20f, 0, "Hello"), word(0.35f, 0.10f, 0.60f, 0.20f, 0, "world"),
             word(0.10f, 0.40f, 0.30f, 0.50f, 1, "second"), word(0.35f, 0.40f, 0.50f, 0.50f, 1, "line"),
             word(0.55f, 0.40f, 0.70f, 0.50f, 1, "here")};
  return l;
}

text_select::source item(std::uint64_t id) {
  text_select::source s;
  s.item = id;
  return s;
}

text_select ready() {
  text_select t;
  t.show(item(7));
  REQUIRE(t.wants_read());
  t.reading();
  REQUIRE_FALSE(t.wants_read());
  REQUIRE(t.set_words(item(7), two_lines()));
  REQUIRE_FALSE(t.wants_read());
  return t;
}

}  // namespace

TEST_CASE("words read for another still or geometry are dropped", "[shell][text]") {
  text_select t;
  REQUIRE_FALSE(t.wants_read());  // nothing on screen
  t.show(item(1));
  t.reading();
  t.show(item(2));                // navigated before the read came back
  REQUIRE_FALSE(t.set_words(item(1), two_lines()));
  REQUIRE_FALSE(t.has_words());
  REQUIRE(t.wants_read());

  t = ready();
  text_select::source turned = item(7);
  turned.geometry.orient = mv::codec::d4{0, -1, 1, 0};
  REQUIRE(t.press(0.2f, 0.15f, 1.0f, 2));
  REQUIRE(t.show(turned));        // a turn drops the words and the highlight
  REQUIRE_FALSE(t.has_words());
  REQUIRE_FALSE(t.has_selection());
  REQUIRE(t.wants_read());
  REQUIRE_FALSE(t.show(turned));  // the same again is no change
}

TEST_CASE("press-drag selects whole words in reading order", "[shell][text]") {
  text_select t = ready();
  REQUIRE(t.over_text(0.2f, 0.15f, 1.0f));
  REQUIRE_FALSE(t.over_text(0.9f, 0.9f, 1.0f));

  REQUIRE(t.press(0.4f, 0.15f, 1.0f, 1));  // on "world"
  REQUIRE_FALSE(t.has_selection());        // nothing until it drags
  t.drag(0.4f, 0.45f);                     // down to "line"
  REQUIRE(t.text() == "world\nsecond line");
  REQUIRE(t.words() == 3);
  t.drag(0.0f, 0.0f);                      // back, above and left: "Hello"
  REQUIRE(t.text() == "Hello world");
  t.release();
  REQUIRE(t.text() == "Hello world");

  text_highlight h;
  t.fill(h);
  REQUIRE(h.count == 1);
  REQUIRE(h.rect[0][0] == 0.10f);
  REQUIRE(h.rect[0][2] == 0.60f);
}

TEST_CASE("a click clears; a press off the text pans", "[shell][text]") {
  text_select t = ready();
  REQUIRE(t.press(0.2f, 0.15f, 1.0f, 2));
  t.release();
  REQUIRE(t.text() == "Hello");
  REQUIRE(t.press(0.2f, 0.45f, 1.0f, 1));  // a click on text that does not drag
  t.release();
  REQUIRE_FALSE(t.has_selection());

  REQUIRE(t.press(0.2f, 0.15f, 1.0f, 2));
  t.release();
  REQUIRE_FALSE(t.press(0.9f, 0.9f, 1.0f, 1));  // off the text: not ours
  REQUIRE_FALSE(t.has_selection());
  t.drag(0.2f, 0.15f);                           // a pan, not a selection
  REQUIRE_FALSE(t.has_selection());
  text_highlight h;
  h.count = 3;
  t.fill(h);
  REQUIRE(h.count == 0);
}

TEST_CASE("double-click a word, triple-click a line, drag on from either", "[shell][text]") {
  text_select t = ready();
  REQUIRE(t.press(0.4f, 0.45f, 1.0f, 2));
  REQUIRE(t.text() == "line");
  t.drag(0.65f, 0.45f);
  REQUIRE(t.text() == "line here");
  t.release();

  REQUIRE(t.press(0.4f, 0.45f, 1.0f, 3));
  REQUIRE(t.text() == "second line here");
  t.drag(0.2f, 0.15f);  // up a line: whole lines
  REQUIRE(t.text() == "Hello world\nsecond line here");
  t.release();
  REQUIRE(t.words() == 5);
}

TEST_CASE("select all and clear", "[shell][text]") {
  text_select t;
  REQUIRE_FALSE(t.select_all());
  t = ready();
  REQUIRE(t.select_all());
  REQUIRE(t.text() == "Hello world\nsecond line here");
  text_highlight h;
  t.fill(h);
  REQUIRE(h.count == 2);
  REQUIRE(t.clear());
  REQUIRE_FALSE(t.clear());
}
