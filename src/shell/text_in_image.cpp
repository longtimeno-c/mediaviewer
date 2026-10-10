// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/text_in_image.h"

#include <algorithm>
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

copied_text run_copy_text(std::string_view source_path, const edit::geometry& g,
                          text_recognizer& recognizer, text_pixel_order order) {
  copied_text out;
  result<text_image> image = render_text_image(source_path, g, order);
  if (!image) {
    out.unreadable = true;
    return out;
  }
  std::string raw;
  out.outcome = recognizer.recognise(*image, raw);
  if (out.outcome != text_recognition::ok) return out;
  out.text = tidy_text(raw);
  out.words = count_words(out.text);
  if (out.words == 0) out.text.clear();
  return out;
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

std::string copy_text_notice(const copied_text& r) {
  if (r.unreadable) return "Could not read this image";
  switch (r.outcome) {
    case text_recognition::ok:
      if (r.words == 0) return "No text found";
      return "Copied " + std::to_string(r.words) + (r.words == 1 ? " word" : " words");
    case text_recognition::no_language:
      return "No text recognition language is installed (Windows Settings > Time & language > Language)";
    case text_recognition::unavailable:
      return "Text recognition is not available on this system";
    case text_recognition::failed:
      break;
  }
  return "Could not recognise text in this image";
}

}  // namespace mv::shell
