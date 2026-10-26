// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance (#297): what the item on screen is, from its first bytes (magic,
// never the extension). A container that may hold a picture or only sound
// (MP4 / MOV, Matroska, AVI, MPEG-TS) is `media`: the add-on then asks the
// host whether it has a picture (sampler_open) and sound (audio_open).
//
// Portable, no I/O (D9).
#pragma once

#include <cstdint>
#include <span>

namespace mv::enhance {

enum class item_kind : std::uint8_t {
  unknown = 0,
  still,      // JPEG, PNG, HEIC / AVIF, WebP, TIFF and RAW, BMP, ICO
  animation,  // GIF, an AVIF sequence: greyed until a model declares it
  media,      // a video container: a clip, or sound alone in one
  audio,      // MP3, an M4A / M4P (FairPlay is never played or read)
  document,   // PDF, DOCX, PPTX: always greyed
};

// The first bytes of a file (64 is enough).
[[nodiscard]] item_kind sniff(std::span<const std::uint8_t> head) noexcept;

}  // namespace mv::enhance
