// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 29 (docs/design/20): the aspect presets Crop locks its draft to, and since
// 2026-11-05 Extend's frame too (shell/edit_workspace.h extend_to_aspect). One
// list and one set of words for both: Free, Original, 1:1, 4:3, 3:2, 16:9, 5:4,
// each landscape or portrait (X in crop, the bar's swap in Extend).
//
// Pure, header-only and dependency-free so the Enhance add-on's tests can see
// edit_workspace.h without the edit session.
#pragma once

#include <cstdint>

namespace mv::shell {

// `original` is the picture's own ratio (after the stack's turns). Values are
// stable: the chromes send them.
enum class crop_aspect : std::uint8_t {
  free = 0,
  original = 1,
  square = 2,
  r4_3 = 3,
  r3_2 = 4,
  r16_9 = 5,
  r5_4 = 6,
  count
};
inline constexpr int kCropAspectCount = static_cast<int>(crop_aspect::count);

// "Free", "Original", "1:1", "4:3", ... (landscape spelling).
[[nodiscard]] constexpr const char* crop_aspect_label(crop_aspect a) noexcept {
  switch (a) {
    case crop_aspect::free: return "Free";
    case crop_aspect::original: return "Original";
    case crop_aspect::square: return "1:1";
    case crop_aspect::r4_3: return "4:3";
    case crop_aspect::r3_2: return "3:2";
    case crop_aspect::r16_9: return "16:9";
    case crop_aspect::r5_4: return "5:4";
    default: return "";
  }
}

// Free has no orientation and a square's is itself: the swap does nothing.
[[nodiscard]] constexpr bool crop_aspect_turns(crop_aspect a) noexcept {
  return a != crop_aspect::free && a != crop_aspect::square && a < crop_aspect::count;
}

// The preset's width over height in pixels over a `w` x `h` picture; 0 for
// Free (or no picture). The presets are landscape unless `portrait`; Original
// is the picture's own ratio, and `portrait` turns it the other way.
[[nodiscard]] constexpr double crop_aspect_pixels(crop_aspect a, bool portrait, std::uint32_t w,
                                                  std::uint32_t h) noexcept {
  if (a == crop_aspect::free || a >= crop_aspect::count || w == 0 || h == 0) return 0.0;
  double pixels = 1.0;  // square
  switch (a) {
    case crop_aspect::original: pixels = static_cast<double>(w) / h; break;
    case crop_aspect::r4_3: pixels = 4.0 / 3.0; break;
    case crop_aspect::r3_2: pixels = 3.0 / 2.0; break;
    case crop_aspect::r16_9: pixels = 16.0 / 9.0; break;
    case crop_aspect::r5_4: pixels = 5.0 / 4.0; break;
    default: break;
  }
  return portrait ? 1.0 / pixels : pixels;
}

}  // namespace mv::shell
