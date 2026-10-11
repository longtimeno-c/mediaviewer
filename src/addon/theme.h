// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Themes: the first thing an open add-on can contribute (docs/design/25 "Themes").
//
// A theme is a JSON file of colour tokens for the CHROME: the bars, panes,
// Settings and text around the photo. It never reaches the canvas's pixels
// (rule 2): a photo is drawn as it was before. Both hosts read the same
// table, so one file themes WinUI and SwiftUI alike (D9).
//
//   {"schema": 1,
//    "dark":  {"canvas": "#1c1b1a", "surface": "#262523", "title": "#f2efe9",
//              "body": "#b9b4aa", "disabled": "#6f6b64", "hairline": "#3a3835",
//              "accent": "#e0793a"},
//    "light": { ...the same seven... },
//    "font": "Avenir Next",
//    "shape": {"radius": 0.5, "density": 0.9, "type_scale": 1.1}}   // schema 2
//
// One of `dark` / `light` is enough: the chrome then keeps that appearance
// while the theme is on. With both it follows the system. `font` names a
// family installed on the machine, and the chrome keeps its own face when it
// is not there; a theme ships no font file.
//
// A theme that cannot be read is a trap, since Settings is where it would be
// turned off. So the host refuses one whose text does not stand out from its
// background, by the WCAG contrast ratio: title 4.5 : 1, body and accent
// 3 : 1, against both canvas and surface. Portable (D9).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mv::addon {

// 1: colours and a face (API 1). 2: also `shape` (API 2, PR 56).
inline constexpr int kThemeSchema = 2;
inline constexpr std::size_t kThemeMaxBytes = 64u << 10;

enum class theme_token : std::uint8_t {
  canvas = 0,  // the chrome's background
  surface,     // rows, fields and cards on it
  title,       // primary text
  body,        // secondary text
  disabled,
  hairline,    // separators and outlines
  accent,      // selection, progress, the focused control
  count_,
};
inline constexpr std::size_t kThemeTokens = static_cast<std::size_t>(theme_token::count_);

[[nodiscard]] std::string_view theme_token_name(theme_token t) noexcept;

struct rgba {
  std::uint8_t r = 0, g = 0, b = 0, a = 255;
  friend bool operator==(const rgba&, const rgba&) = default;
};

using theme_palette = std::array<rgba, kThemeTokens>;

// The chrome's proportions (theme schema 2, docs/design/25 "Theme, shape").
// Each is a scale on today's literals, which are 1: a host multiplies its
// corner radii by `radius`, its padding and row spacing by `density`, and its
// type sizes by `type_scale`, once, when the theme is applied. Bounded so no
// theme can make the chrome unreadable or unclickable.
inline constexpr double kShapeRadiusMin = 0.0, kShapeRadiusMax = 2.0;
inline constexpr double kShapeDensityMin = 0.75, kShapeDensityMax = 1.25;
inline constexpr double kShapeTypeMin = 0.85, kShapeTypeMax = 1.25;

struct theme_shape {
  double radius = 1.0;      // 0 square corners .. 2 twice as round
  double density = 1.0;     // 0.75 compact .. 1.25 roomy
  double type_scale = 1.0;  // 0.85 .. 1.25
  friend bool operator==(const theme_shape&, const theme_shape&) = default;
};

struct theme {
  std::optional<theme_palette> dark;
  std::optional<theme_palette> light;
  std::string font;  // a family name, or empty
  theme_shape shape;
};

enum class theme_fault : std::uint8_t {
  none = 0,
  malformed,     // not the JSON above
  unknown_token, // a key this host has no token for
  missing_token, // a palette without all seven
  bad_colour,    // not #rrggbb or #rrggbbaa
  translucent,   // canvas or surface with alpha: there is nothing under them to show
  low_contrast,  // text that does not stand out from its background
  bad_shape,     // a shape key unknown, not a number, or outside its range
};

[[nodiscard]] const char* theme_fault_name(theme_fault f) noexcept;

struct theme_result {
  theme_fault fault = theme_fault::malformed;
  theme t;
  [[nodiscard]] bool ok() const noexcept { return fault == theme_fault::none; }
};

[[nodiscard]] theme_result parse_theme(std::string_view json_text);

// WCAG 2 contrast ratio of `fg` drawn over the opaque `bg`, 1 .. 21.
[[nodiscard]] double contrast_ratio(rgba fg, rgba bg) noexcept;

// What the chromes read: {"font": "...", "dark": {"canvas": "#rrggbbaa", ...}
// | null, "light": ... | null, "shape": {"radius", "density", "type_scale"}}.
// Every colour eight digits, lowercase; `shape` always present (1s for a
// schema 1 theme), three decimals.
[[nodiscard]] std::string theme_to_json(const theme& t);

}  // namespace mv::addon
