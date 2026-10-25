// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The look of the driftwire command line (docs/design/28 "From a terminal"):
// Driftwire's own colours, the glyphs, and their fallbacks.
//
// One accent (the Wi-Fi teal), a tint per bearer as the app's path picture
// draws it (ScreenView.swift LinkNode.tint), dim grey for borders and hints.
// Mid-tone colours only, so a light or a dark terminal reads them. Truecolor
// where the terminal has it; FTXUI picks the nearest of 256 or 16 otherwise;
// NO_COLOR (no-color.org) or --no-color: no colour at all, the dim attribute
// for what would be grey. A terminal whose locale is not UTF-8 gets ASCII
// glyphs (and plain digits for the code).
//
// FTXUI turns a colour into the terminal's depth when the colour is made,
// from one process-wide setting: make_theme sets it, so make the theme once
// (the program has one terminal) and render with it.
#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include <ftxui/screen/color.hpp>
#include <ftxui/screen/terminal.hpp>

namespace mv::driftwire::cli {

enum class color_depth : std::uint8_t { none, c16, c256, truecolor };

// What a piece of text means; render.cpp turns it into colour and weight.
enum class tone : std::uint8_t { normal, accent, dim, success, error, warning, info };

// The path a transfer takes, each with its tint.
enum class bearer : std::uint8_t { wifi, bluetooth, internet, dht, relay };

struct rgb {
  std::uint8_t r = 0, g = 0, b = 0;
};

namespace palette {
inline constexpr rgb kTeal{0x40, 0xC8, 0xE0};    // accent, Wi-Fi (systemTeal)
inline constexpr rgb kBlue{0x0A, 0x84, 0xFF};    // Bluetooth (systemBlue)
inline constexpr rgb kIndigo{0x5E, 0x5C, 0xE6};  // the internet (systemIndigo)
inline constexpr rgb kOrange{0xFF, 0x9F, 0x0A};  // met on the DHT (systemOrange)
inline constexpr rgb kPurple{0xBF, 0x5A, 0xF2};  // through a relay (systemPurple)
inline constexpr rgb kGreen{0x30, 0xD1, 0x58};   // success
inline constexpr rgb kRed{0xFF, 0x45, 0x3A};     // error
inline constexpr rgb kYellow{0xFF, 0xD6, 0x0A};  // warning, cancelled, declined
inline constexpr rgb kGrey{0x8E, 0x8E, 0x93};    // dim text, borders
inline constexpr rgb kShine{0xA9, 0xE6, 0xF1};   // the shimmer's band: the accent, lighter
}  // namespace palette

// Every glyph the UI draws, so ASCII is one table away.
struct glyph_set {
  std::string_view bullet, elbow, pointer, peer, nobody, ellipsis, sep, wordmark, arrows, dash;
  std::string_view line, dot, caret, track;
  // Frames: ╭ ╮ ╰ ╯ ─ │ (cards), ┌ ┐ └ ┘ (the code boxes).
  std::string_view tl, tr, bl, br, h, v, btl, btr, bbl, bbr;
  std::array<std::string_view, 6> spinner;  // ping-pong: 0..n-1..1
  int spinner_frames = 6;
  std::array<std::string_view, 8> eighths;  // ▏ ... █; ASCII: whole cells only
  bool partial_cells = true;
  bool big_digits = true;  // the code in three-row digits (else plain bold digits)
};

inline constexpr glyph_set kUnicodeGlyphs{
    "\xE2\x8F\xBA",  // ⏺ bullet
    "\xE2\x8E\xBF",  // ⎿ elbow
    "\xE2\x9D\xAF",  // ❯ pointer
    "\xE2\x97\x89",  // ◉ peer
    "\xE2\x97\x8B",  // ○ nobody
    "\xE2\x80\xA6",  // … ellipsis
    "\xC2\xB7",      // · sep
    "\xE2\x89\x8B",  // ≋ wordmark
    "\xE2\x86\x91\xE2\x86\x93",  // ↑↓
    "\xE2\x80\x93",  // – dash (en)
    "\xE2\x94\x81",  // ━ line
    "\xE2\x97\x8F",  // ● dot
    "\xE2\x96\x8F",  // ▏ caret
    "\xE2\x94\x80",  // ─ track
    "\xE2\x95\xAD", "\xE2\x95\xAE", "\xE2\x95\xB0", "\xE2\x95\xAF", "\xE2\x94\x80", "\xE2\x94\x82",  // ╭╮╰╯─│
    "\xE2\x94\x8C", "\xE2\x94\x90", "\xE2\x94\x94", "\xE2\x94\x98",                                  // ┌┐└┘
    {"\xC2\xB7", "\xE2\x9C\xA2", "\xE2\x9C\xB3", "\xE2\x9C\xB6", "\xE2\x9C\xBB", "\xE2\x9C\xBD"},   // · ✢ ✳ ✶ ✻ ✽
    6,
    {"\xE2\x96\x8F", "\xE2\x96\x8E", "\xE2\x96\x8D", "\xE2\x96\x8C", "\xE2\x96\x8B", "\xE2\x96\x8A",
     "\xE2\x96\x89", "\xE2\x96\x88"},  // ▏▎▍▌▋▊▉█
    true,
    true,
};

inline constexpr glyph_set kAsciiGlyphs{
    "*", "`-", ">", "*", "o", "...", "-", "~", "up/down", "-",
    "=", "o", "_", "-",
    "+", "+", "+", "+", "-", "|",
    "+", "+", "+", "+",
    {"-", "\\", "|", "/", "-", "-"},
    4,
    {"#", "#", "#", "#", "#", "#", "#", "#"},
    false,
    false,
};

struct theme {
  color_depth depth = color_depth::truecolor;
  const glyph_set* g = &kUnicodeGlyphs;

  // The roles, already at the terminal's depth (Color::Default with none).
  ftxui::Color accent, wifi, bluetooth, internet, dht, relay, success, error, warning, grey, shine, gradient_end;

  [[nodiscard]] bool colour() const noexcept { return depth != color_depth::none; }
  [[nodiscard]] bool shimmer() const noexcept { return depth == color_depth::truecolor; }
  [[nodiscard]] ftxui::Color of(bearer b) const noexcept {
    switch (b) {
      case bearer::bluetooth: return bluetooth;
      case bearer::internet: return internet;
      case bearer::dht: return dht;
      case bearer::relay: return relay;
      case bearer::wifi: break;
    }
    return wifi;
  }
};

[[nodiscard]] inline ftxui::Color make_color(color_depth d, rgb c) {
  if (d == color_depth::none) return ftxui::Color::Default;
  return ftxui::Color::RGB(c.r, c.g, c.b);
}

// Sets FTXUI's colour support to `depth` (see the header comment), then
// makes the colours. ASCII glyphs when `unicode` is false.
[[nodiscard]] inline theme make_theme(color_depth depth, bool unicode) {
  using ftxui::Terminal::Color;
  ftxui::Terminal::SetColorSupport(depth == color_depth::none   ? Color::Palette1
                                   : depth == color_depth::c16  ? Color::Palette16
                                   : depth == color_depth::c256 ? Color::Palette256
                                                                : Color::TrueColor);
  theme t;
  t.depth = depth;
  t.g = unicode ? &kUnicodeGlyphs : &kAsciiGlyphs;
  t.accent = t.wifi = make_color(depth, palette::kTeal);
  t.bluetooth = make_color(depth, palette::kBlue);
  t.internet = t.gradient_end = make_color(depth, palette::kIndigo);
  t.dht = make_color(depth, palette::kOrange);
  t.relay = make_color(depth, palette::kPurple);
  t.success = make_color(depth, palette::kGreen);
  t.error = make_color(depth, palette::kRed);
  t.warning = make_color(depth, palette::kYellow);
  t.grey = make_color(depth, palette::kGrey);
  t.shine = make_color(depth, palette::kShine);
  return t;
}

// The terminal's depth: none for --no-color or a non-empty NO_COLOR, else
// what FTXUI reads from TERM / COLORTERM / TERM_PROGRAM (and, on Windows,
// whether the console takes VT sequences).
[[nodiscard]] inline color_depth detect_color_depth(bool no_color_flag) {
  if (no_color_flag) return color_depth::none;
  if (const char* nc = std::getenv("NO_COLOR"); nc && nc[0] != '\0') return color_depth::none;  // NOLINT
  switch (ftxui::Terminal::ColorSupport()) {
    case ftxui::Terminal::Color::Palette1: return color_depth::none;
    case ftxui::Terminal::Color::Palette16: return color_depth::c16;
    case ftxui::Terminal::Color::Palette256: return color_depth::c256;
    case ftxui::Terminal::Color::TrueColor: break;
  }
  return color_depth::truecolor;
}

}  // namespace mv::driftwire::cli
