// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/text.h"

#include <ftxui/screen/string.hpp>

namespace mv::driftwire::cli {

namespace {

// Bytes in the UTF-8 sequence at `s[i]`: its lead byte's length when the
// continuation bytes are there, else 1 (a stray byte stands alone).
std::size_t sequence_at(std::string_view s, std::size_t i) {
  const auto c = static_cast<unsigned char>(s[i]);
  const std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
  if (i + n > s.size()) return 1;
  for (std::size_t k = 1; k < n; ++k) {
    if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return 1;
  }
  return n;
}

}  // namespace

int display_width(std::string_view utf8) { return ftxui::string_width(utf8); }

std::vector<glyph_span> glyph_spans(std::string_view utf8) {
  std::vector<glyph_span> out;
  out.reserve(utf8.size());
  for (std::size_t i = 0; i < utf8.size();) {
    const std::size_t n = sequence_at(utf8, i);
    const int w = ftxui::string_width(utf8.substr(i, n));
    if (w == 0 && !out.empty()) {
      out.back().len += n;  // a combining mark, a joiner: part of the glyph before
    } else {
      out.push_back({i, n, w});
    }
    i += n;
  }
  return out;
}

std::string fit(std::string_view utf8, int width, std::string_view ellipsis) {
  if (width <= 0) return {};
  if (display_width(utf8) <= width) return std::string(utf8);
  const int ew = display_width(ellipsis);
  const bool room = ew < width;  // too narrow for the ellipsis: cut without it
  const int budget = room ? width - ew : width;
  std::string out;
  int used = 0;
  for (const auto& g : glyph_spans(utf8)) {
    if (used + g.width > budget) break;
    out.append(utf8.substr(g.at, g.len));
    used += g.width;
  }
  // No space before the ellipsis: "Ann's …" reads worse than "Ann's…".
  while (!out.empty() && out.back() == ' ') out.pop_back();
  if (room) out.append(ellipsis);
  return out;
}

std::vector<std::string> wrap(std::string_view utf8, int width) {
  std::vector<std::string> out;
  if (width <= 0) width = 1;
  std::size_t start = 0;
  while (true) {
    const std::size_t nl = utf8.find('\n', start);
    const std::string_view para = utf8.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
    std::string line;
    int line_w = 0;
    std::size_t i = 0;
    bool wrote = false;
    while (i < para.size()) {
      while (i < para.size() && para[i] == ' ') ++i;
      if (i >= para.size()) break;
      std::size_t j = para.find(' ', i);
      if (j == std::string_view::npos) j = para.size();
      std::string_view word = para.substr(i, j - i);
      int ww = display_width(word);
      if (line_w > 0 && line_w + 1 + ww <= width) {
        line += ' ';
        line.append(word);
        line_w += 1 + ww;
      } else {
        if (line_w > 0) {
          out.push_back(std::move(line));
          wrote = true;
          line.clear();
          line_w = 0;
        }
        // A word wider than a line: as much as fits, then the rest on the next.
        while (ww > width) {
          std::string piece;
          int pw = 0;
          std::size_t used = 0;
          for (const auto& g : glyph_spans(word)) {
            if (pw + g.width > width && pw > 0) break;
            piece.append(word.substr(g.at, g.len));
            pw += g.width;
            used = g.at + g.len;
          }
          out.push_back(std::move(piece));
          wrote = true;
          word = word.substr(used);
          ww = display_width(word);
        }
        line.assign(word);
        line_w = ww;
      }
      i = j;
    }
    if (line_w > 0 || !wrote) out.push_back(std::move(line));
    if (nl == std::string_view::npos) break;
    start = nl + 1;
  }
  return out;
}

std::string pad(std::string_view utf8, int width) {
  std::string out(utf8);
  const int w = display_width(utf8);
  if (w < width) out.append(static_cast<std::size_t>(width - w), ' ');
  return out;
}

std::string repeat(std::string_view s, int n) {
  std::string out;
  if (n <= 0) return out;
  out.reserve(s.size() * static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) out.append(s);
  return out;
}

}  // namespace mv::driftwire::cli
