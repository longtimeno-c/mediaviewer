// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The live region of the driftwire command line: the few rows at the bottom
// of the terminal that are redrawn (the cards, Nearby, the prompt, its
// hints), with everything finished printed once above them so it stays in
// the scrollback. Never the alternate screen: the terminal keeps a readable
// log of what happened after the program exits.
//
// Drawing a frame: the document is laid out by FTXUI at the terminal's width
// and read back cell by cell (rasterize); if the rows and the cursor are
// what is already on screen nothing is written. Otherwise one write: begin
// synchronized output (CSI ?2026h, ignored where unknown), hide the cursor,
// autowrap off, go up to the region's first row, clear to the end of the
// screen, the rows, then the cursor back into the prompt, autowrap on, the
// cursor shown, and end synchronized output. Without autowrap a row the
// terminal measures wider than FTXUI did (an emoji newer than FTXUI's width
// table, in a name any peer chose) is cut at the edge rather than wrapped
// onto a row the way back up would not count.
// The region is at most the terminal's height less one (a taller one would
// scroll its top out of reach of cursor-up); the caller shrinks Nearby first,
// and what is still too tall loses its top rows.
//
// A resize redraws from scratch. A narrower terminal reflows the rows
// already drawn (the terminals in use all do), so the way back up to the
// region's top is counted at the new width.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/dom/elements.hpp>

namespace mv::driftwire::cli {

// Where the bytes go: the terminal (term.h), or a fake one in the tests.
class output {
 public:
  virtual ~output() = default;
  virtual void write(std::string_view bytes) = 0;
};

// A document laid out at `width` columns, read back from FTXUI's cells.
struct raster {
  std::vector<std::string> rows;   // each with its SGR styling; trailing blanks dropped
  std::vector<std::string> plain;  // the same rows as characters alone (the tests read these)
  int cursor_row = -1;             // the focused element (the prompt's cursor), -1: none
  int cursor_col = -1;
};
[[nodiscard]] raster rasterize(const ftxui::Element& doc, int width);

class inline_view {
 public:
  inline_view(output& out, int width, int height);

  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }
  // The most rows the live region may take.
  [[nodiscard]] int max_rows() const noexcept { return height_ > 2 ? height_ - 1 : 1; }

  // The terminal's new size: the next present() draws everything.
  void resize(int width, int height);
  // Draws `doc` as the live region. False (and nothing written) when it is
  // what is on screen already.
  bool present(const ftxui::Element& doc);
  // Prints `entry` above the live region, where it stays (the transcript),
  // then draws the region again below it.
  void print(const ftxui::Element& entry);
  // Ctrl+L: draws the region again in full at the next present().
  void invalidate() noexcept { dirty_ = true; }
  // Takes the live region off the screen and shows the cursor where the
  // region began (to suspend, or to hand the terminal back for a moment).
  // The next present() draws it again.
  void erase();
  // The end: erases the live region, prints `last` dim (if any) and shows
  // the cursor. Nothing is drawn after this.
  void close(std::string_view last);

  // Synchronized output around each write (on by default).
  void set_synchronized(bool on) noexcept { sync_ = on; }

  // For the mouse: where the region is. Each write counts (the session asks
  // the terminal for the cursor after one, CSI 6n); after it the cursor is
  // up_from_top() rows below the region's first row; the document's first
  // cut_rows() rows were left off the top (too tall). So the document row of
  // a click on screen row R (0-based) is R - (cursor row - up_from_top()) + cut_rows().
  [[nodiscard]] std::uint64_t writes() const noexcept { return writes_; }
  [[nodiscard]] int up_from_top() const noexcept { return up_; }
  [[nodiscard]] int cut_rows() const noexcept { return cut_; }
  [[nodiscard]] bool drawn() const noexcept { return drawn_ && !closed_; }

 private:
  // Up to the region's first row and clear from there down.
  void erase_into(std::string& s) const;
  // `rows` from the cursor's line, then the cursor into place.
  void draw_into(std::string& s, const raster& r);
  void begin(std::string& s) const;
  void finish(std::string& s, bool show_cursor) const;

  output& out_;
  int width_ = 80, height_ = 24;
  raster shown_;          // the region on screen
  bool drawn_ = false;    // shown_ is on screen
  int up_ = 0;            // rows from the cursor up to the region's first row
  bool dirty_ = false;    // draw at the next present even if unchanged
  bool closed_ = false;
  bool sync_ = true;
  std::uint64_t writes_ = 0;
  int cut_ = 0;           // rows of the last document left off the top
};

}  // namespace mv::driftwire::cli
