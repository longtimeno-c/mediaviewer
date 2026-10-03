// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The document text engine (docs/plans/audio-and-documents.md §2.5): HarfBuzz
// shapes, FreeType draws, codec/fonts.h finds the files. One code path on both
// platforms, so a DOCX lays out and looks the same everywhere.
//
// Layout is in points from unhinted font units, so where a line or a page
// breaks does not depend on the size it is drawn at: a 1024 px preview and the
// 3200 px page agree on the page count. Drawing scales to pixels.
//
// One engine per decode: fonts are opened lazily and released with it. Not
// thread-safe; a decode worker owns its engine.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "codec/raster.h"

namespace mv::codec::text {

struct style {
  std::string family = "Calibri";
  double size_pt = 11.0;
  bool bold = false;
  bool italic = false;
  bool underline = false;
  bool strike = false;
  std::uint32_t rgb = 0x000000;
};

// One shaped word (or a run of spaces): glyphs in visual order, positions in
// points from the word's origin.
struct shaped {
  int font = -1;  // the engine's font slot; -1 when nothing could be loaded
  struct glyph {
    std::uint32_t id = 0;
    double x = 0.0;   // pen position, points
    double dy = 0.0;  // vertical offset, points (up is positive)
  };
  std::vector<glyph> glyphs;
  double advance = 0.0;  // points
  double ascent = 0.0;   // points above the baseline
  double descent = 0.0;  // points below it (positive)
  double line_gap = 0.0;
  bool synthetic_bold = false;
  bool synthetic_italic = false;
};

class engine {
 public:
  engine();
  ~engine();
  engine(const engine&) = delete;
  engine& operator=(const engine&) = delete;

  // Shapes `utf8` in the style's family, falling back (fonts::fallbacks) to
  // the first family that has every character. Never fails: missing glyphs
  // draw as the font's .notdef.
  [[nodiscard]] shaped shape(std::string_view utf8, const style& s);

  // The style's line metrics without shaping anything (an empty line).
  [[nodiscard]] shaped metrics(const style& s);

  // Draws a shaped word into `page` (RGBA8, opaque) with its pen at
  // (x_pt, baseline_pt) in points; `scale` is pixels per point.
  void draw(const shaped& word, const style& s, raster& page, double x_pt, double baseline_pt,
            double scale);

  // Whether any font at all could be loaded (the system has none we can read).
  [[nodiscard]] bool usable();

 private:
  struct impl;
  std::unique_ptr<impl> d_;
};

// Fills a rectangle (points) with a colour — rules, underlines, table borders.
void fill_rect(raster& page, double x_pt, double y_pt, double w_pt, double h_pt, double scale,
               std::uint32_t rgb);

}  // namespace mv::codec::text
