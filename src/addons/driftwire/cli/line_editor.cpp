// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/line_editor.h"

#include <algorithm>

#include "addons/driftwire/cli/text.h"

namespace mv::driftwire::cli {

namespace {

// The code point at the start of `s` (a stray byte as itself).
char32_t first_code_point(std::string_view s) {
  if (s.empty()) return 0;
  const auto c = static_cast<unsigned char>(s[0]);
  if (c < 0x80 || s.size() < 2) return c;
  if ((c >> 5) == 6) return static_cast<char32_t>(((c & 0x1F) << 6) | (s[1] & 0x3F));
  if ((c >> 4) == 14 && s.size() >= 3) {
    return static_cast<char32_t>(((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F));
  }
  if ((c >> 3) == 30 && s.size() >= 4) {
    return static_cast<char32_t>(((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F));
  }
  return c;
}

// Letters and digits make words; so does any character outside ASCII (a
// name in another script moves by word, not by letter).
bool word_glyph(std::string_view g) {
  const auto c = static_cast<unsigned char>(g.empty() ? 0 : g[0]);
  return c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool space_glyph(std::string_view g) { return g == " "; }

// One line of a pasted list, as one shell word (see line_editor.h).
std::string quoted_line(std::string_view line, char q) {
  if (line.size() >= 2 && (line.front() == '\'' || line.front() == '"') && line.back() == line.front()) {
    return std::string(line);  // quoted already
  }
  if (q == '"') {
    // Windows: a backslash separates and an apostrophe is a letter; a path holds no '"'.
    if (line.find_first_of(" \t") == std::string_view::npos) return std::string(line);
    return "\"" + std::string(line) + "\"";
  }
  if (line.find_first_of(" \t'\"\\") == std::string_view::npos) return std::string(line);
  std::string out = "'";
  for (const char c : line) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return out + "'";
}

}  // namespace

int line_editor::cursor_column() const { return display_width(std::string_view(text_).substr(0, cursor_)); }

int line_editor::width() const { return display_width(text_); }

void line_editor::set(std::string_view text) { set(text, text.size()); }

void line_editor::set(std::string_view text, std::size_t cursor) {
  text_.clear();
  cursor_ = 0;
  insert(text);
  cursor_ = 0;
  // Onto the first glyph boundary at or after `cursor`.
  for (const auto& g : glyph_spans(text_)) {
    if (g.at >= cursor) break;
    cursor_ = g.at + g.len;
  }
  history_at_ = history_.size();
}

void line_editor::clear() {
  text_.clear();
  cursor_ = 0;
  scroll_ = 0;
  history_at_ = history_.size();
}

void line_editor::insert(std::string_view utf8) {
  // Code point by code point first: a control has no width, so as a glyph it
  // would ride along with the character before it.
  std::string kept;
  kept.reserve(utf8.size());
  for (std::size_t i = 0; i < utf8.size();) {
    const auto c = static_cast<unsigned char>(utf8[i]);
    std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (i + n > utf8.size()) n = 1;
    const std::string_view piece = utf8.substr(i, n);
    i += n;
    const char32_t cp = first_code_point(piece);
    if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) continue;  // controls
    if (filter_ && !filter_(cp)) continue;
    kept.append(piece);
  }
  std::string clean;
  if (max_glyphs_) {
    std::size_t glyphs = glyph_spans(text_).size();
    for (const auto& g : glyph_spans(kept)) {
      if (glyphs >= max_glyphs_) break;
      clean.append(kept, g.at, g.len);
      ++glyphs;
    }
  } else {
    clean = std::move(kept);
  }
  text_.insert(cursor_, clean);
  cursor_ += clean.size();
}

void line_editor::paste(std::string_view utf8) {
  // The lines (CRLF, CR or LF), without the empty ones.
  std::vector<std::string_view> rows;
  for (std::size_t at = 0; at <= utf8.size();) {
    const std::size_t end = std::min(utf8.find_first_of("\r\n", at), utf8.size());
    if (end > at) rows.push_back(utf8.substr(at, end - at));
    at = end + (end + 1 < utf8.size() && utf8[end] == '\r' && utf8[end + 1] == '\n' ? 2 : 1);
  }
  const bool list = paste_quote_ != 0 && rows.size() > 1;
  std::string flat;
  flat.reserve(utf8.size() + 2 * rows.size());
  for (const std::string_view row : rows) {
    if (!flat.empty()) flat += ' ';
    std::string r = list ? quoted_line(row, paste_quote_) : std::string(row);
    std::replace(r.begin(), r.end(), '\t', ' ');
    flat += r;
  }
  // A paste that ended in a newline still ends in a separator, as typed text would.
  if (!utf8.empty() && (utf8.back() == '\n' || utf8.back() == '\r') && !flat.empty()) flat += ' ';
  insert(flat);
}

std::size_t line_editor::prev_glyph(std::size_t at) const {
  std::size_t prev = 0;
  for (const auto& g : glyph_spans(text_)) {
    if (g.at >= at) break;
    prev = g.at;
  }
  return prev;
}

std::size_t line_editor::next_glyph(std::size_t at) const {
  for (const auto& g : glyph_spans(text_)) {
    if (g.at >= at) return g.at + g.len;
  }
  return text_.size();
}

void line_editor::left() {
  if (cursor_ > 0) cursor_ = prev_glyph(cursor_);
}

void line_editor::right() {
  if (cursor_ < text_.size()) cursor_ = next_glyph(cursor_);
}

void line_editor::word_left() {
  const auto spans = glyph_spans(text_);
  std::size_t i = 0;
  while (i < spans.size() && spans[i].at < cursor_) ++i;  // i: the glyph at the cursor
  const auto g = [&](std::size_t k) { return std::string_view(text_).substr(spans[k].at, spans[k].len); };
  while (i > 0 && !word_glyph(g(i - 1))) --i;
  while (i > 0 && word_glyph(g(i - 1))) --i;
  cursor_ = i < spans.size() ? spans[i].at : text_.size();
}

void line_editor::word_right() {
  const auto spans = glyph_spans(text_);
  std::size_t i = 0;
  while (i < spans.size() && spans[i].at < cursor_) ++i;
  const auto g = [&](std::size_t k) { return std::string_view(text_).substr(spans[k].at, spans[k].len); };
  while (i < spans.size() && !word_glyph(g(i))) ++i;
  while (i < spans.size() && word_glyph(g(i))) ++i;
  cursor_ = i < spans.size() ? spans[i].at : text_.size();
}

void line_editor::erase(std::size_t from, std::size_t to) {
  if (to <= from) return;
  text_.erase(from, to - from);
  cursor_ = from;
}

void line_editor::backspace() {
  if (cursor_ > 0) erase(prev_glyph(cursor_), cursor_);
}

void line_editor::del() {
  if (cursor_ < text_.size()) erase(cursor_, next_glyph(cursor_));
}

void line_editor::kill_to_start() { erase(0, cursor_); }

void line_editor::kill_to_end() { erase(cursor_, text_.size()); }

void line_editor::kill_word_back_space() {
  const auto spans = glyph_spans(text_);
  std::size_t i = 0;
  while (i < spans.size() && spans[i].at < cursor_) ++i;
  const auto g = [&](std::size_t k) { return std::string_view(text_).substr(spans[k].at, spans[k].len); };
  while (i > 0 && space_glyph(g(i - 1))) --i;
  while (i > 0 && !space_glyph(g(i - 1))) --i;
  erase(i < spans.size() ? spans[i].at : text_.size(), cursor_);
}

void line_editor::kill_word_back() {
  const std::size_t end_at = cursor_;
  word_left();
  erase(cursor_, end_at);
}

void line_editor::kill_word_forward() {
  const std::size_t from = cursor_;
  word_right();
  erase(from, cursor_);
}

bool line_editor::handle(const key_event& e) {
  switch (e.k) {
    case key::text:
      if (e.alt) {
        // Alt+B / Alt+F / Alt+D: readline's word keys (and Option+Left on macOS).
        if (e.text == "b") {
          word_left();
        } else if (e.text == "f") {
          word_right();
        } else if (e.text == "d") {
          kill_word_forward();
        } else {
          return false;
        }
        return true;
      }
      insert(e.text);
      return true;
    case key::paste: paste(e.text); return true;
    case key::left:
      if (e.alt || e.ctrl) {
        word_left();
      } else {
        left();
      }
      return true;
    case key::right:
      if (e.alt || e.ctrl) {
        word_right();
      } else {
        right();
      }
      return true;
    case key::home: home(); return true;
    case key::end: end(); return true;
    case key::backspace:
      if (e.alt || e.ctrl) {
        kill_word_back();
      } else {
        backspace();
      }
      return true;
    case key::del: del(); return true;
    case key::ctrl:
      switch (e.letter) {
        case 'a': home(); return true;
        case 'e': end(); return true;
        case 'b': left(); return true;
        case 'f': right(); return true;
        case 'u': kill_to_start(); return true;
        case 'k': kill_to_end(); return true;
        case 'w': kill_word_back_space(); return true;
        case 'd':
          if (text_.empty()) return false;  // the caller's: quit on an empty line
          del();
          return true;
        case 'p': history_older(); return true;
        case 'n': history_newer(); return true;
        default: return false;
      }
    default: return false;
  }
}

void line_editor::remember(std::string_view line) {
  if (!line.empty() && (history_.empty() || history_.back() != line)) {
    history_.emplace_back(line);
    if (history_.size() > kHistory) history_.erase(history_.begin());
  }
  history_at_ = history_.size();
  draft_.clear();
}

bool line_editor::history_older() {
  if (history_at_ == 0) return false;
  if (history_at_ == history_.size()) draft_ = text_;
  --history_at_;
  text_ = history_[history_at_];
  cursor_ = text_.size();
  return true;
}

bool line_editor::history_newer() {
  if (history_at_ >= history_.size()) return false;
  ++history_at_;
  text_ = history_at_ == history_.size() ? draft_ : history_[history_at_];
  cursor_ = text_.size();
  return true;
}

line_editor::window line_editor::view(int columns) {
  window w;
  columns = std::max(1, columns);
  const auto spans = glyph_spans(text_);
  const int total = display_width(text_);
  const int cur = cursor_column();
  const int end_cell = cursor_ >= text_.size() ? 1 : 0;  // the cursor past the end needs a column
  if (total + end_cell <= columns) {
    scroll_ = 0;
    w.text = text_;
    w.cursor_col = cur;
    return w;
  }
  // Too long: keep the cursor in sight, moving the view as little as
  // possible. All the columns are the text's unless more of it follows on
  // the right: then the last one is the ellipsis's.
  const int most = std::max(0, total + end_cell - columns);  // never further than shows the end
  scroll_ = std::clamp(scroll_, 0, most);
  const auto follow = [&](int room) {
    if (cur < scroll_) scroll_ = cur;
    if (cur >= scroll_ + room) scroll_ = cur - room + 1;
  };
  follow(columns);
  if (total + end_cell - scroll_ > columns) follow(std::max(1, columns - 1));
  // Start on a glyph: one cut by the left edge is not shown.
  std::size_t first = 0;
  int first_col = 0, col = 0;
  for (; first < spans.size(); ++first) {
    if (col >= scroll_) break;
    col += spans[first].width;
  }
  first_col = col;
  w.cut_left = first > 0;
  w.cut_right = total - first_col + end_cell > columns;
  const int avail = w.cut_right ? columns - 1 : columns;
  int used = 0;
  for (std::size_t i = first; i < spans.size(); ++i) {
    if (used + spans[i].width > avail) break;
    w.text.append(std::string_view(text_).substr(spans[i].at, spans[i].len));
    used += spans[i].width;
  }
  w.cursor_col = cur - first_col;
  return w;
}

}  // namespace mv::driftwire::cli
