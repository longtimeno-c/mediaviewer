// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Finding an installed font file by family and style — the one per-OS piece
// of the document text engine (codec/text.h, docs/plans/audio-and-documents.md
// §2.5). CoreText on macOS (fonts_mac.cpp), DirectWrite on Windows
// (fonts_win.cpp). Only the file comes back; FreeType and HarfBuzz read it, so
// layout and pixels are the same code on both platforms.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace mv::codec::fonts {

struct face_ref {
  std::string path;            // UTF-8
  std::uint32_t index = 0;     // face in a collection (.ttc), when the OS knows it
  std::string postscript;      // the face's PostScript name, to find it in a collection
  bool bold = false;           // what the face is, which may differ from what was asked
  bool italic = false;
};

// The installed face of `family` nearest to the style, or nullopt when the
// family is not installed. Never a substitute family. [any-thread]
[[nodiscard]] std::optional<face_ref> find(std::string_view family, bool bold, bool italic);

// Families tried in order when a run's own family is not installed or lacks a
// character: the platform's UI sans-serif first, then wide-coverage families.
[[nodiscard]] std::span<const std::string_view> fallbacks() noexcept;

}  // namespace mv::codec::fonts
