// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/inline_view.h"

#include <algorithm>

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include "addons/driftwire/cli/text.h"

namespace mv::driftwire::cli {

namespace {

constexpr std::string_view kSyncOn = "\x1B[?2026h";
constexpr std::string_view kSyncOff = "\x1B[?2026l";
constexpr std::string_view kHideCursor = "\x1B[?25l";
constexpr std::string_view kShowCursor = "\x1B[?25h";
// Autowrap (DECAWM) off while the region is drawn: a row the terminal draws
// wider than FTXUI laid it out (a newer emoji its width table counts as one
// column, a name any peer chose) is cut at the edge, not wrapped onto a row
// the way back up does not count. On again for the transcript and after.
constexpr std::string_view kWrapOff = "\x1B[?7l";
constexpr std::string_view kWrapOn = "\x1B[?7h";

struct style {
  bool bold = false, dim = false, italic = false, underline = false, inverted = false, strike = false;
  ftxui::Color fg, bg;
};

style style_of(const ftxui::Cell& c) {
  style s;
  s.bold = c.bold;
  s.dim = c.dim;
  s.italic = c.italic;
  s.underline = c.underlined || c.underlined_double;
  s.inverted = c.inverted;
  s.strike = c.strikethrough;
  s.fg = c.foreground_color;
  s.bg = c.background_color;
  return s;
}

// One SGR that resets and sets `s` whole (simpler than a difference, and as short for a few styles a row).
void sgr(std::string& out, const style& s) {
  out += "\x1B[0";
  if (s.bold) out += ";1";
  if (s.dim) out += ";2";
  if (s.italic) out += ";3";
  if (s.underline) out += ";4";
  if (s.inverted) out += ";7";
  if (s.strike) out += ";9";
  if (s.fg != ftxui::Color()) {
    out += ';';
    s.fg.PrintTo(out, false);
  }
  if (s.bg != ftxui::Color()) {
    out += ';';
    s.bg.PrintTo(out, true);
  }
  out += 'm';
}

bool blank(const ftxui::Cell& c) {
  return (c.character.empty() || c.character == " ") && c.background_color == ftxui::Color() && !c.inverted &&
         !c.underlined && !c.underlined_double && !c.strikethrough;
}

void csi(std::string& s, int n, char f) {
  s += "\x1B[";
  s += std::to_string(n);
  s += f;
}

}  // namespace

raster rasterize(const ftxui::Element& doc, int width) {
  raster r;
  if (!doc || width <= 0) return r;
  doc->ComputeRequirement();
  const int h = doc->requirement().min_y;
  if (h <= 0) return r;
  ftxui::Screen screen(width, h);
  ftxui::Render(screen, doc);
  if (const auto cur = screen.cursor(); cur.shape != ftxui::Screen::Cursor::Hidden) {
    r.cursor_row = cur.y;
    r.cursor_col = cur.x;
  }
  std::string plain_sgr;
  sgr(plain_sgr, style{});
  for (int y = 0; y < h; ++y) {
    int last = -1;
    for (int x = 0; x < width; ++x) {
      if (!blank(screen.CellAt(x, y))) last = x;
    }
    std::string row, chars;
    // Compared as written, not as styles: two colours FTXUI keeps apart
    // (a gradient's) are often one in 256 or 16 colours.
    std::string now = plain_sgr, next;
    bool second_half = false;  // the cell after a wide character belongs to it
    for (int x = 0; x <= last; ++x) {
      const ftxui::Cell& c = screen.CellAt(x, y);
      if (second_half) {
        second_half = false;
        continue;
      }
      next.clear();
      sgr(next, style_of(c));
      if (next != now) {
        row += next;
        now.swap(next);
      }
      const std::string_view g = c.character.empty() ? std::string_view(" ") : std::string_view(c.character);
      row += g;
      chars += g;
      second_half = g.size() > 1 && ftxui::string_width(g) == 2;
    }
    if (now != plain_sgr) row += "\x1B[0m";
    r.rows.push_back(std::move(row));
    r.plain.push_back(std::move(chars));
  }
  return r;
}

inline_view::inline_view(output& out, int width, int height)
    : out_(out), width_(std::max(1, width)), height_(std::max(1, height)) {}

void inline_view::resize(int width, int height) {
  width = std::max(1, width);
  height = std::max(1, height);
  if (width == width_ && height == height_) return;
  if (drawn_ && width < width_ && !shown_.rows.empty()) {
    // The terminal rewraps every row wider than it now is: count the way
    // up from the cursor to the region's top at the new width.
    const int last = static_cast<int>(shown_.rows.size()) - 1;
    const int row = shown_.cursor_row >= 0 ? shown_.cursor_row : last;
    int up = 0;
    for (int i = 0; i < row; ++i) {
      const int w = display_width(shown_.plain[static_cast<std::size_t>(i)]);
      up += std::max(1, (w + width - 1) / width);
    }
    const int col = shown_.cursor_row >= 0 ? shown_.cursor_col
                                           : std::min(width_ - 1, display_width(shown_.plain[static_cast<std::size_t>(last)]));
    up_ = up + col / width;
  }
  width_ = width;
  height_ = height;
  dirty_ = true;
}

void inline_view::begin(std::string& s) const {
  if (sync_) s += kSyncOn;
  s += kHideCursor;
  s += kWrapOff;
}

void inline_view::finish(std::string& s, bool show_cursor) const {
  s += kWrapOn;
  if (show_cursor) s += kShowCursor;
  if (sync_) s += kSyncOff;
}

void inline_view::erase_into(std::string& s) const {
  if (drawn_ && up_ > 0) csi(s, up_, 'A');
  s += "\r\x1B[J";
}

void inline_view::draw_into(std::string& s, const raster& r) {
  for (std::size_t i = 0; i < r.rows.size(); ++i) {
    if (i) s += "\r\n";
    s += r.rows[i];
  }
  if (r.rows.empty()) {
    up_ = 0;
    return;
  }
  const int last = static_cast<int>(r.rows.size()) - 1;
  if (r.cursor_row >= 0) {
    if (last - r.cursor_row > 0) csi(s, last - r.cursor_row, 'A');
    s += '\r';
    if (r.cursor_col > 0) csi(s, r.cursor_col, 'C');
    up_ = r.cursor_row;
  } else {
    up_ = last;
  }
}

bool inline_view::present(const ftxui::Element& doc) {
  if (closed_) return false;
  raster r = rasterize(doc, width_);
  cut_ = 0;
  if (const int over = static_cast<int>(r.rows.size()) - max_rows(); over > 0) {
    cut_ = over;
    r.rows.erase(r.rows.begin(), r.rows.begin() + over);
    r.plain.erase(r.plain.begin(), r.plain.begin() + over);
    r.cursor_row = r.cursor_row >= over ? r.cursor_row - over : -1;
    if (r.cursor_row < 0) r.cursor_col = -1;
  }
  if (drawn_ && !dirty_ && r.rows == shown_.rows && r.cursor_row == shown_.cursor_row &&
      r.cursor_col == shown_.cursor_col) {
    return false;
  }
  std::string s;
  begin(s);
  erase_into(s);
  draw_into(s, r);
  finish(s, r.cursor_row >= 0);
  out_.write(s);
  ++writes_;
  shown_ = std::move(r);
  drawn_ = true;
  dirty_ = false;
  return true;
}

void inline_view::print(const ftxui::Element& entry) {
  const raster e = rasterize(entry, width_);
  std::string s;
  begin(s);
  if (!closed_) erase_into(s);
  // The transcript wraps as the terminal likes: nothing counts its rows.
  s += kWrapOn;
  for (const auto& row : e.rows) {
    s += row;
    s += "\r\n";
  }
  up_ = 0;
  const bool region = !closed_ && drawn_ && !shown_.rows.empty();
  if (region) {
    s += kWrapOff;
    draw_into(s, shown_);
  }
  finish(s, closed_ || (region && shown_.cursor_row >= 0));
  out_.write(s);
  ++writes_;
}

void inline_view::erase() {
  if (closed_ || !drawn_) return;
  std::string s;
  begin(s);
  erase_into(s);
  finish(s, true);
  out_.write(s);
  shown_ = {};
  drawn_ = false;
  up_ = 0;
}

void inline_view::close(std::string_view last) {
  if (closed_) return;
  std::string s;
  begin(s);
  erase_into(s);
  s += kWrapOn;
  if (!last.empty()) {
    s += "\x1B[0;2m";
    s += last;
    s += "\x1B[0m\r\n";
  }
  finish(s, true);
  out_.write(s);
  shown_ = {};
  drawn_ = false;
  closed_ = true;
  up_ = 0;
}

}  // namespace mv::driftwire::cli
