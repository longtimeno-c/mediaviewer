// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Text by display width for the driftwire command line. A name can be any
// Unicode (emoji, CJK, combining marks), so nothing is cut or measured in
// bytes: the columns are FTXUI's (string_width), the same measure its screen
// uses to place each character, so a cut here and a cell there agree.
//
// A glyph is a code point that takes columns plus the zero-width ones after
// it (combining marks, joiners, variation selectors): the unit a cursor moves
// over and a backspace removes.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mv::driftwire::cli {

struct glyph_span {
  std::size_t at = 0;   // byte offset
  std::size_t len = 0;  // bytes
  int width = 0;        // columns (0, 1 or 2)
};

[[nodiscard]] int display_width(std::string_view utf8);
[[nodiscard]] std::vector<glyph_span> glyph_spans(std::string_view utf8);
// `utf8` in at most `width` columns, ending in `ellipsis` when cut.
[[nodiscard]] std::string fit(std::string_view utf8, int width, std::string_view ellipsis);
// Greedy word wrap at spaces to `width` columns; a word wider than a line is
// broken between glyphs; "\n" breaks a line. At least one (maybe empty) line.
[[nodiscard]] std::vector<std::string> wrap(std::string_view utf8, int width);
// `utf8` followed by spaces up to `width` columns.
[[nodiscard]] std::string pad(std::string_view utf8, int width);
// `s` `n` times.
[[nodiscard]] std::string repeat(std::string_view s, int n);

}  // namespace mv::driftwire::cli
