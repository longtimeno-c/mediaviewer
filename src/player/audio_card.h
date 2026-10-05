// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The picture an audio file shows when it has no cover art of its own, and the
// one a FairPlay-protected file shows instead of playing
// (docs/plans/audio-and-documents.md §2.2). Pure CPU: a dark card with one
// anti-aliased glyph, as tightly packed sRGB RGBA. Used by the player (the still
// it presents) and the poster path (the filmstrip tile), so both agree.
#pragma once

#include <cstdint>
#include <vector>

namespace mv::player {

enum class audio_card_kind : std::uint8_t {
  music,      // an audio file with no picture
  protected_, // FairPlay (M4P): shown, never played
};

struct audio_card {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgba;  // width * height * 4, sRGB, opaque
};

// 16:9 by default, the shape of the clips it sits beside. Both sides even, so
// the NV12 upload keeps every pixel.
[[nodiscard]] audio_card make_audio_card(audio_card_kind kind, std::uint32_t width = 1280,
                                         std::uint32_t height = 720);

}  // namespace mv::player
