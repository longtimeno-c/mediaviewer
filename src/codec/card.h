// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A card: a dark picture with one anti-aliased glyph, as tightly packed sRGB
// RGBA, for a file that has no picture of its own or will not show one
// (docs/plans/audio-and-documents.md): an audio file without cover art (music),
// a FairPlay M4P or a password-protected PDF (locked). Pure CPU. Used by the
// player, the poster path and the document decoders, so every view agrees.
#pragma once

#include <cstdint>
#include <vector>

namespace mv::codec {

enum class card_kind : std::uint8_t {
  music,   // an audio file with no picture
  locked,  // FairPlay (M4P) or a password-protected document: shown, never opened
};

struct card {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgba;  // width * height * 4, sRGB, opaque
};

// 16:9 by default, the shape of the clips it sits beside. Both sides even, so
// the NV12 upload keeps every pixel.
[[nodiscard]] card make_card(card_kind kind, std::uint32_t width = 1280,
                             std::uint32_t height = 720);

}  // namespace mv::codec
