// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/theme.h"

#include <cmath>

#include "core/json.h"

namespace mv::addon {
namespace {

constexpr std::array<std::string_view, kThemeTokens> kNames{
    "canvas", "surface", "title", "body", "disabled", "hairline", "accent"};

constexpr double kTitleContrast = 4.5;
constexpr double kBodyContrast = 3.0;

int hex_digit(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parse_colour(std::string_view s, rgba& out) noexcept {
  if ((s.size() != 7 && s.size() != 9) || s.front() != '#') return false;
  std::uint8_t v[4] = {0, 0, 0, 255};
  for (std::size_t i = 0; i < (s.size() - 1) / 2; ++i) {
    const int hi = hex_digit(s[1 + 2 * i]);
    const int lo = hex_digit(s[2 + 2 * i]);
    if (hi < 0 || lo < 0) return false;
    v[i] = static_cast<std::uint8_t>(hi << 4 | lo);
  }
  out = rgba{v[0], v[1], v[2], v[3]};
  return true;
}

double channel_linear(double c) noexcept {
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(double r, double g, double b) noexcept {
  return 0.2126 * channel_linear(r) + 0.7152 * channel_linear(g) + 0.0722 * channel_linear(b);
}

theme_fault parse_palette(const json::value& v, theme_palette& out) {
  if (v.k != json::kind::object) return theme_fault::malformed;
  std::array<bool, kThemeTokens> have{};
  for (const auto& [key, value] : v.o) {
    std::size_t index = kThemeTokens;
    for (std::size_t i = 0; i < kThemeTokens; ++i) {
      if (kNames[i] == key) index = i;
    }
    if (index == kThemeTokens) return theme_fault::unknown_token;
    if (value.k != json::kind::string || !parse_colour(value.s, out[index])) {
      return theme_fault::bad_colour;
    }
    have[index] = true;
  }
  for (const bool h : have) {
    if (!h) return theme_fault::missing_token;
  }
  const auto at = [&out](theme_token t) { return out[static_cast<std::size_t>(t)]; };
  if (at(theme_token::canvas).a != 255 || at(theme_token::surface).a != 255) {
    return theme_fault::translucent;
  }
  for (const theme_token bg : {theme_token::canvas, theme_token::surface}) {
    if (contrast_ratio(at(theme_token::title), at(bg)) < kTitleContrast ||
        contrast_ratio(at(theme_token::body), at(bg)) < kBodyContrast ||
        contrast_ratio(at(theme_token::accent), at(bg)) < kBodyContrast) {
      return theme_fault::low_contrast;
    }
  }
  return theme_fault::none;
}

void write_palette(json::writer& w, const std::optional<theme_palette>& p) {
  if (!p) {
    w.null();
    return;
  }
  static constexpr char kHex[] = "0123456789abcdef";
  w.begin_object();
  for (std::size_t i = 0; i < kThemeTokens; ++i) {
    const rgba c = (*p)[i];
    std::string text = "#";
    for (const std::uint8_t b : {c.r, c.g, c.b, c.a}) {
      text.push_back(kHex[b >> 4]);
      text.push_back(kHex[b & 0xF]);
    }
    w.key(kNames[i]).string(text);
  }
  w.end_object();
}

}  // namespace

std::string_view theme_token_name(theme_token t) noexcept {
  const auto i = static_cast<std::size_t>(t);
  return i < kThemeTokens ? kNames[i] : std::string_view();
}

const char* theme_fault_name(theme_fault f) noexcept {
  switch (f) {
    case theme_fault::none: return "ok";
    case theme_fault::malformed: return "malformed";
    case theme_fault::unknown_token: return "unknown_token";
    case theme_fault::missing_token: return "missing_token";
    case theme_fault::bad_colour: return "bad_colour";
    case theme_fault::translucent: return "translucent";
    case theme_fault::low_contrast: return "low_contrast";
  }
  return "unknown";
}

double contrast_ratio(rgba fg, rgba bg) noexcept {
  // Text with alpha is what it looks like once drawn: blended over `bg`.
  const double a = fg.a / 255.0;
  const auto mix = [a](std::uint8_t f, std::uint8_t b) {
    return (f / 255.0) * a + (b / 255.0) * (1.0 - a);
  };
  const double l1 = luminance(mix(fg.r, bg.r), mix(fg.g, bg.g), mix(fg.b, bg.b));
  const double l2 = luminance(bg.r / 255.0, bg.g / 255.0, bg.b / 255.0);
  const double hi = l1 > l2 ? l1 : l2;
  const double lo = l1 > l2 ? l2 : l1;
  return (hi + 0.05) / (lo + 0.05);
}

theme_result parse_theme(std::string_view text) {
  theme_result r;
  if (text.empty() || text.size() > kThemeMaxBytes) return r;
  const auto doc = json::parse(text, 4);
  if (!doc || doc->k != json::kind::object) return r;
  const auto schema = doc->integer("schema");
  if (!schema || *schema != kThemeSchema) return r;
  for (const auto& [key, value] : doc->o) {
    if (key == "schema") continue;
    if (key == "dark" || key == "light") {
      theme_palette p{};
      if (const theme_fault f = parse_palette(value, p); f != theme_fault::none) {
        r.fault = f;
        return r;
      }
      (key == "dark" ? r.t.dark : r.t.light) = p;
    } else if (key == "font") {
      if (value.k != json::kind::string || value.s.size() > 64) return r;
      for (const char c : value.s) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) return r;
      }
      r.t.font = value.s;
    } else {
      r.fault = theme_fault::unknown_token;
      return r;
    }
  }
  if (!r.t.dark && !r.t.light) return r;
  r.fault = theme_fault::none;
  return r;
}

std::string theme_to_json(const theme& t) {
  json::writer w;
  w.begin_object();
  w.key("font").string(t.font);
  w.key("dark");
  write_palette(w, t.dark);
  w.key("light");
  write_palette(w, t.light);
  w.end_object();
  return w.take();
}

}  // namespace mv::addon
