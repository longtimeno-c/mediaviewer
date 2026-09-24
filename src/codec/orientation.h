// SPDX-License-Identifier: GPL-2.0-or-later
// The eight EXIF orientations as a group (D4), shared by the display path
// (codec/orient.cpp), the edit stack (edit/) and the lossless JPEG transform.
//
// One convention everywhere: an element maps a point of the *displayed* image
// to the point of the *stored* image it shows, in centred coordinates
// (-1..1 on each axis, y down):
//
//   stored = [a b; c d] * displayed
//
// Composition is matrix product. If the file is displayed through `base` and
// the user then rotates what they see by `op`, the stored pixels are reached
// through `compose(base, op)`.
//
// The same element as (transpose, flip_x, flip_y), applied in that order to
// go from stored to displayed, is what a pixel loop wants:
//   px = flip_x ? W'-1-x : x;  py = flip_y ? H'-1-y : y;
//   stored = transpose ? (py, px) : (px, py)
// where (W', H') are the displayed dimensions.
#pragma once

#include <cstdint>

namespace mv::codec {

struct d4 {
  std::int8_t a = 1, b = 0, c = 0, d = 1;

  friend constexpr bool operator==(const d4&, const d4&) = default;

  [[nodiscard]] constexpr bool transposes() const noexcept { return a == 0; }
  [[nodiscard]] constexpr bool flip_x() const noexcept { return a == 0 ? c < 0 : a < 0; }
  [[nodiscard]] constexpr bool flip_y() const noexcept { return a == 0 ? b < 0 : d < 0; }
  [[nodiscard]] constexpr bool identity() const noexcept { return *this == d4{}; }
};

[[nodiscard]] constexpr d4 compose(d4 base, d4 op) noexcept {
  return d4{static_cast<std::int8_t>(base.a * op.a + base.b * op.c),
            static_cast<std::int8_t>(base.a * op.b + base.b * op.d),
            static_cast<std::int8_t>(base.c * op.a + base.d * op.c),
            static_cast<std::int8_t>(base.c * op.b + base.d * op.d)};
}

[[nodiscard]] constexpr d4 inverse(d4 g) noexcept {
  // Signed permutation matrices are orthogonal: the inverse is the transpose.
  return d4{g.a, g.c, g.b, g.d};
}

// User operations on what is displayed.
inline constexpr d4 kRotateCw{0, 1, -1, 0};
inline constexpr d4 kRotateCcw{0, -1, 1, 0};
inline constexpr d4 kFlipH{-1, 0, 0, 1};
inline constexpr d4 kFlipV{1, 0, 0, -1};

// EXIF Orientation 1..8 (anything else is 1).
[[nodiscard]] constexpr d4 from_exif(int orientation) noexcept {
  switch (orientation) {
    case 2: return d4{-1, 0, 0, 1};   // mirror horizontal
    case 3: return d4{-1, 0, 0, -1};  // rotate 180
    case 4: return d4{1, 0, 0, -1};   // mirror vertical
    case 5: return d4{0, 1, 1, 0};    // transpose
    case 6: return d4{0, 1, -1, 0};   // rotate 90 CW
    case 7: return d4{0, -1, -1, 0};  // transverse
    case 8: return d4{0, -1, 1, 0};   // rotate 90 CCW
    default: return d4{};
  }
}

[[nodiscard]] constexpr int to_exif(d4 g) noexcept {
  for (int o = 1; o <= 8; ++o) {
    if (from_exif(o) == g) return o;
  }
  return 1;
}

}  // namespace mv::codec
