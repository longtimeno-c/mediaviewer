// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Text in images (Live Text, as in Photos; docs/design/16 "Text in images"):
// the host's half of selecting text on the canvas. Both hosts drive one of
// these from the UI thread with pointer positions already turned into 0..1 of
// the displayed image (input_state.h canvas_map), and publish what it fills
// into input_snapshot::text for the render thread to draw.
//
// The rules, as Photos has them:
//   - A press on a word starts a selection there; dragging extends it a whole
//     word at a time, in reading order, to the word nearest the pointer (off
//     the text too). A press on text never pans.
//   - A double-click selects the word, a triple-click its line.
//   - A click on text that does not drag, or a press anywhere off the text,
//     clears the selection; off the text the press then pans as before.
//   - Copy (Cmd/Ctrl+C) copies the selection; Select All (Cmd/Ctrl+A) with a
//     selection selects every word; Esc clears it.
//
// It also keeps which still and which geometry the words were read from, so
// the host knows when to read again (a new item, a crop, a turn). UI thread
// only; no I/O, nothing that blocks.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "edit/edit_stack.h"
#include "shell/input_state.h"
#include "shell/text_in_image.h"

namespace mv::shell {

class text_select {
 public:
  // What a recognition is for: the item on screen and the geometry it was
  // shown with. Words read under any other pair are stale.
  struct source {
    std::uint64_t item = 0;  // 0 = none
    edit::geometry geometry{};
  };

  [[nodiscard]] static bool same(const source& a, const source& b) noexcept;

  // The item on screen, or its geometry, changed: anything read before is
  // dropped, with the selection. True when a highlight was showing (redraw).
  bool show(const source& now);
  [[nodiscard]] const source& current() const noexcept { return current_; }

  // Recognition: the host asks `wants_read` before starting one (nothing read
  // or reading for this source yet) and marks it `reading`; `set_words`
  // takes the result and is ignored when the source has moved on since.
  [[nodiscard]] bool wants_read() const noexcept { return current_.item != 0 && !reading_ && !read_; }
  void reading() noexcept { reading_ = true; }
  bool set_words(const source& from, text_layout layout);
  [[nodiscard]] bool has_words() const noexcept { return !layout_.empty(); }

  // The pointer is over a word (for the I-beam cursor).
  [[nodiscard]] bool over_text(float u, float v, float aspect) const noexcept;

  // A left press, `clicks` 1, 2 or 3. True when it landed on text and selects
  // (the host sets input_snapshot::text_press so the canvas does not pan).
  bool press(float u, float v, float aspect, int clicks);
  // The pointer moved with the button down after a press that returned true.
  void drag(float u, float v);
  void release();

  bool select_all();
  bool clear();  // true when something was selected
  [[nodiscard]] bool has_selection() const noexcept { return !sel_.empty(); }
  [[nodiscard]] std::string text() const { return selected_text(layout_, sel_); }
  [[nodiscard]] std::size_t words() const noexcept {
    return sel_.empty() ? 0 : static_cast<std::size_t>(sel_.last() - sel_.first() + 1);
  }

  // The highlight for the snapshot. Cheap and allocation-free; call it after
  // any of the above.
  void fill(text_highlight& out) const noexcept;

 private:
  source current_;
  bool reading_ = false;
  bool read_ = false;
  text_layout layout_;
  text_selection sel_;
  bool pressed_ = false;
  bool dragged_ = false;
  int press_clicks_ = 0;
  int press_first_ = -1;  // the word (or line) the press selected, for
  int press_last_ = -1;   // extending a double- or triple-click drag
};

}  // namespace mv::shell
