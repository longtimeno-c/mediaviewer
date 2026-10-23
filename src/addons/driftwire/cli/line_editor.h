// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// One editable line for the driftwire command line: the prompt, and the
// fields that ask for a code, a folder or a relay server. Pure and tested.
//
// The cursor moves by glyph (text.h: a character with its combining marks),
// so Backspace never leaves half an accent or half an emoji behind; columns
// are display columns. Keys as a shell's line editor has them (readline's
// emacs set, the part a one-line field needs):
//
//   Left / Right, Ctrl+B / Ctrl+F     a glyph
//   Alt+Left / Alt+Right, Ctrl+Left / Ctrl+Right, Alt+B / Alt+F
//                                      a word (letters and digits)
//   Home / End, Ctrl+A / Ctrl+E       the start, the end
//   Backspace, Delete (Ctrl+D on text) the glyph before, under the cursor
//   Ctrl+W                             the word before, back to a space (as bash)
//   Alt+Backspace, Alt+D               the word before, after (letters and digits)
//   Ctrl+U / Ctrl+K                    to the start, to the end
//   Ctrl+P / Ctrl+N                    the history, older, newer
//
// A paste is inserted whole; its newlines (and tabs) become spaces, so it
// never submits the line. Other control characters are dropped. A filter can
// limit what is typed (the code field takes digits only).
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "addons/driftwire/cli/keys.h"

namespace mv::driftwire::cli {

class line_editor {
 public:
  using filter_fn = bool (*)(char32_t);

  [[nodiscard]] const std::string& text() const noexcept { return text_; }
  [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }  // a byte offset, on a glyph boundary
  [[nodiscard]] bool empty() const noexcept { return text_.empty(); }
  // The cursor's display column, and the whole line's.
  [[nodiscard]] int cursor_column() const;
  [[nodiscard]] int width() const;

  // Replaces the line; the cursor goes to the end (or `cursor`, kept on a glyph boundary).
  void set(std::string_view text);
  void set(std::string_view text, std::size_t cursor);
  void clear();
  // Characters a filter refuses are not inserted (nullptr: everything printable).
  void set_filter(filter_fn f) noexcept { filter_ = f; }
  void set_max_glyphs(std::size_t n) noexcept { max_glyphs_ = n; }

  // Typed text at the cursor; control characters are dropped.
  void insert(std::string_view utf8);
  // Pasted text: newlines and tabs are spaces, then as insert.
  void paste(std::string_view utf8);

  void left();
  void right();
  void home() noexcept { cursor_ = 0; }
  void end() noexcept { cursor_ = text_.size(); }
  void word_left();
  void word_right();
  void backspace();
  void del();
  void kill_to_start();
  void kill_to_end();
  void kill_word_back_space();  // Ctrl+W
  void kill_word_back();        // Alt+Backspace
  void kill_word_forward();     // Alt+D

  // Applies an editing key; false when `e` is not one (Enter, Esc, Tab, ↑↓,
  // Ctrl+C ... are the caller's).
  bool handle(const key_event& e);

  // History: the submitted lines, oldest first (consecutive repeats once, at
  // most kHistory). Ctrl+P / Ctrl+N walk it; what was being typed comes back
  // at the newest end.
  static constexpr std::size_t kHistory = 100;
  void remember(std::string_view line);
  bool history_older();
  bool history_newer();
  [[nodiscard]] const std::vector<std::string>& history() const noexcept { return history_; }

  // What a field `columns` wide shows: the part of the line around the
  // cursor (scrolled as little as possible), the cursor's column in it, and
  // whether the line goes on to the left or right. With `cut_right` the slice
  // leaves the last column free (render.cpp puts an ellipsis there), and the
  // cursor always has a column of its own.
  struct window {
    std::string text;
    int cursor_col = 0;
    bool cut_left = false, cut_right = false;
  };
  [[nodiscard]] window view(int columns);

 private:
  void erase(std::size_t from, std::size_t to);
  std::size_t prev_glyph(std::size_t at) const;
  std::size_t next_glyph(std::size_t at) const;

  std::string text_;
  std::size_t cursor_ = 0;
  filter_fn filter_ = nullptr;
  std::size_t max_glyphs_ = 0;  // 0: no limit
  std::vector<std::string> history_;
  std::size_t history_at_ = 0;  // == history_.size(): not walking
  std::string draft_;           // the line being typed when the walk began
  int scroll_ = 0;              // the first column view() shows
};

}  // namespace mv::driftwire::cli
