// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/text_in_image.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "codec/decode.h"
#include "edit/geometry.h"
#include "io/file.h"

namespace mv::shell {
namespace {

// Straight alpha over white, and the host's channel order. One pass.
void flatten_for_text(std::vector<std::uint8_t>& px, text_pixel_order order) noexcept {
  const bool swap = order == text_pixel_order::bgra;
  for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
    const unsigned a = px[i + 3];
    if (a != 255) {
      for (std::size_t c = 0; c < 3; ++c) {
        px[i + c] = static_cast<std::uint8_t>((px[i + c] * a + 255u * (255u - a) + 127u) / 255u);
      }
      px[i + 3] = 255;
    }
    if (swap) std::swap(px[i], px[i + 2]);
  }
}

bool is_space_byte(unsigned char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// The length of a non-ASCII separator at `s[i]` (U+00A0, U+3000), else 0.
std::size_t wide_space_at(std::string_view s, std::size_t i) noexcept {
  if (i + 1 < s.size() && static_cast<unsigned char>(s[i]) == 0xC2 &&
      static_cast<unsigned char>(s[i + 1]) == 0xA0) {
    return 2;
  }
  if (i + 2 < s.size() && static_cast<unsigned char>(s[i]) == 0xE3 &&
      static_cast<unsigned char>(s[i + 1]) == 0x80 && static_cast<unsigned char>(s[i + 2]) == 0x80) {
    return 3;
  }
  return 0;
}

}  // namespace

result<text_image> render_text_image(std::string_view source_path, const edit::geometry& g,
                                     text_pixel_order order, std::uint32_t max_edge) {
  MV_TRY(std::vector<std::uint8_t> bytes, io::read_all(source_path));
  MV_TRY(codec::raster decoded, codec::decode(bytes));
  bytes = {};
  const edit::size2 source{decoded.width, decoded.height};
  edit::placement p = edit::place(g, source);
  if (max_edge != 0 && std::max(p.output.w, p.output.h) > max_edge) {
    edit::geometry smaller = g;
    smaller.resize = edit::resize_spec{edit::resize_mode::long_edge, max_edge, 0, 100.0f};
    p = edit::place(smaller, source);
  }
  MV_TRY(codec::raster out, edit::render(decoded, p));
  decoded = codec::raster{};
  flatten_for_text(out.rgba, order);
  text_image image;
  image.width = out.width;
  image.height = out.height;
  image.pixels = std::move(out.rgba);
  return image;
}

read_text run_text_recognition(std::string_view source_path, const edit::geometry& g,
                               text_recognizer& recognizer, text_pixel_order order) {
  read_text out;
  result<text_image> image = render_text_image(source_path, g, order);
  if (!image) {
    out.unreadable = true;
    return out;
  }
  out.outcome = recognizer.recognise(*image, out.layout);
  if (out.outcome != text_recognition::ok) out.layout = {};
  return out;
}

void add_line_split_evenly(text_layout& layout, std::uint32_t line, std::string_view utf8,
                           float x0, float y0, float x1, float y1) {
  // Each word's start and end, in bytes and in code points.
  struct span { std::size_t begin, end, cp_begin, cp_end; };
  std::vector<span> words;
  std::size_t cp = 0;
  bool in_word = false;
  for (std::size_t i = 0; i < utf8.size();) {
    const std::size_t sep = is_space_byte(static_cast<unsigned char>(utf8[i])) ? 1 : wide_space_at(utf8, i);
    if (sep != 0) {
      if (in_word) {
        words.back().end = i;
        words.back().cp_end = cp;
      }
      in_word = false;
      i += sep;
      ++cp;
      continue;
    }
    if (!in_word) words.push_back({i, utf8.size(), cp, 0});
    in_word = true;
    const auto c = static_cast<unsigned char>(utf8[i]);
    i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    ++cp;
  }
  if (in_word) words.back().cp_end = cp;
  if (words.empty() || cp == 0) return;
  const float per = (x1 - x0) / static_cast<float>(cp);
  for (const span& w : words) {
    text_word word;
    word.x0 = x0 + per * static_cast<float>(w.cp_begin);
    word.x1 = x0 + per * static_cast<float>(w.cp_end);
    word.y0 = y0;
    word.y1 = y1;
    word.line = line;
    word.text = std::string(utf8.substr(w.begin, std::min(w.end, utf8.size()) - w.begin));
    layout.words.push_back(std::move(word));
  }
}

text_layout parse_word_records(std::string_view utf8, std::uint32_t width, std::uint32_t height) {
  text_layout layout;
  if (width == 0 || height == 0) return layout;
  while (!utf8.empty()) {
    const std::size_t nl = utf8.find('\n');
    std::string_view rec = utf8.substr(0, nl);
    utf8 = nl == std::string_view::npos ? std::string_view{} : utf8.substr(nl + 1);
    if (!rec.empty() && rec.back() == '\r') rec.remove_suffix(1);
    double field[5] = {};
    bool ok = true;
    for (double& f : field) {
      const std::size_t tab = rec.find('\t');
      if (tab == std::string_view::npos) {
        ok = false;
        break;
      }
      const std::string number(rec.substr(0, tab));
      char* end = nullptr;
      f = std::strtod(number.c_str(), &end);
      if (number.empty() || end != number.c_str() + number.size()) ok = false;
      rec.remove_prefix(tab + 1);
    }
    if (!ok || rec.empty() || field[0] < 0.0 || field[3] <= 0.0 || field[4] <= 0.0) continue;
    text_word w;
    w.line = static_cast<std::uint32_t>(field[0]);
    w.x0 = static_cast<float>(field[1] / width);
    w.y0 = static_cast<float>(field[2] / height);
    w.x1 = static_cast<float>((field[1] + field[3]) / width);
    w.y1 = static_cast<float>((field[2] + field[4]) / height);
    w.text = std::string(rec);
    layout.words.push_back(std::move(w));
  }
  return layout;
}

int word_at(const text_layout& layout, float u, float v, float aspect) noexcept {
  if (aspect <= 0.0f) aspect = 1.0f;
  int best = -1;
  float best_d = 0.0f;
  for (std::size_t i = 0; i < layout.words.size(); ++i) {
    const text_word& w = layout.words[i];
    const float gv = 0.25f * (w.y1 - w.y0);
    const float gu = gv / aspect;
    if (u < w.x0 - gu || u > w.x1 + gu || v < w.y0 - gv || v > w.y1 + gv) continue;
    // Grown boxes of neighbours overlap: the nearest box wins.
    const float du = u < w.x0 ? (w.x0 - u) * aspect : u > w.x1 ? (u - w.x1) * aspect : 0.0f;
    const float dv = v < w.y0 ? w.y0 - v : v > w.y1 ? v - w.y1 : 0.0f;
    const float d = du + dv;
    if (best < 0 || d < best_d) {
      best = static_cast<int>(i);
      best_d = d;
    }
  }
  return best;
}

int nearest_word(const text_layout& layout, float u, float v) noexcept {
  const auto& words = layout.words;
  int best_first = -1, best_last = -1;
  float best_d = 0.0f;
  for (std::size_t i = 0; i < words.size();) {
    std::size_t j = i;
    float top = words[i].y0, bottom = words[i].y1;
    while (j + 1 < words.size() && words[j + 1].line == words[i].line) {
      ++j;
      top = std::min(top, words[j].y0);
      bottom = std::max(bottom, words[j].y1);
    }
    const float d = v < top ? top - v : v > bottom ? v - bottom : 0.0f;
    if (best_first < 0 || d < best_d) {
      best_first = static_cast<int>(i);
      best_last = static_cast<int>(j);
      best_d = d;
    }
    i = j + 1;
  }
  if (best_first < 0) return -1;
  int best = best_first;
  float best_du = 0.0f;
  for (int i = best_first; i <= best_last; ++i) {
    const text_word& w = words[static_cast<std::size_t>(i)];
    const float du = u < w.x0 ? w.x0 - u : u > w.x1 ? u - w.x1 : 0.0f;
    if (i == best_first || du < best_du) {
      best = i;
      best_du = du;
    }
  }
  return best;
}

void line_bounds(const text_layout& layout, int word, int& first, int& last) noexcept {
  first = last = -1;
  if (word < 0 || static_cast<std::size_t>(word) >= layout.words.size()) return;
  const std::uint32_t line = layout.words[static_cast<std::size_t>(word)].line;
  first = last = word;
  while (first > 0 && layout.words[static_cast<std::size_t>(first - 1)].line == line) --first;
  while (static_cast<std::size_t>(last + 1) < layout.words.size() &&
         layout.words[static_cast<std::size_t>(last + 1)].line == line) {
    ++last;
  }
}

std::string selected_text(const text_layout& layout, text_selection sel) {
  std::string out;
  if (sel.empty() || static_cast<std::size_t>(sel.last()) >= layout.words.size()) return out;
  for (int i = sel.first(); i <= sel.last(); ++i) {
    const text_word& w = layout.words[static_cast<std::size_t>(i)];
    if (i != sel.first()) out.push_back(layout.words[static_cast<std::size_t>(i - 1)].line == w.line ? ' ' : '\n');
    out += w.text;
  }
  return out;
}

void selection_rects(const text_layout& layout, text_selection sel, std::vector<text_rect>& out) {
  out.clear();
  if (sel.empty() || static_cast<std::size_t>(sel.last()) >= layout.words.size()) return;
  for (int i = sel.first(); i <= sel.last(); ++i) {
    const text_word& w = layout.words[static_cast<std::size_t>(i)];
    if (i == sel.first() || layout.words[static_cast<std::size_t>(i - 1)].line != w.line) {
      out.push_back({w.x0, w.y0, w.x1, w.y1});
      continue;
    }
    text_rect& r = out.back();
    r.x0 = std::min(r.x0, w.x0);
    r.y0 = std::min(r.y0, w.y0);
    r.x1 = std::max(r.x1, w.x1);
    r.y1 = std::max(r.y1, w.y1);
  }
}

std::string tidy_text(std::string_view utf8) {
  std::string out;
  out.reserve(utf8.size());
  std::size_t line_start = 0;  // in `out`
  for (std::size_t i = 0; i < utf8.size(); ++i) {
    const char c = utf8[i];
    if (c == '\r' || c == '\n') {
      while (out.size() > line_start && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
      out.push_back('\n');
      line_start = out.size();
      if (c == '\r' && i + 1 < utf8.size() && utf8[i + 1] == '\n') ++i;
      continue;
    }
    out.push_back(c);
  }
  while (out.size() > line_start && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
  // Blank lines (whitespace only, now empty) at either end.
  const std::size_t first = out.find_first_not_of('\n');
  if (first == std::string::npos) return {};
  const std::size_t last = out.find_last_not_of('\n');
  return out.substr(first, last - first + 1);
}

std::size_t count_words(std::string_view utf8) noexcept {
  std::size_t words = 0;
  bool in_word = false;
  for (std::size_t i = 0; i < utf8.size();) {
    std::size_t sep = is_space_byte(static_cast<unsigned char>(utf8[i])) ? 1 : wide_space_at(utf8, i);
    if (sep != 0) {
      in_word = false;
      i += sep;
      continue;
    }
    if (!in_word) ++words;
    in_word = true;
    ++i;
  }
  return words;
}

std::string copy_text_notice(std::size_t words) {
  return "Copied " + std::to_string(words) + (words == 1 ? " word" : " words");
}

std::string text_recognition_notice(const read_text& r) {
  if (r.unreadable) return {};
  switch (r.outcome) {
    case text_recognition::no_language:
      return "To select text in images, add a language with text recognition (Windows Settings > Time & language > Language)";
    case text_recognition::unavailable:
      return "Selecting text in images is not available on this system";
    case text_recognition::ok:
    case text_recognition::failed:
      break;
  }
  return {};
}

}  // namespace mv::shell
