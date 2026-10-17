// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The D5 extension set, written once (issue #170). A folder listing keeps a
// file whose extension is in this table (io/dir_win.cpp, io/dir_mac.cpp), and
// io/pairing.h reads the kinds (RAW, video, audio, document) from it, so the
// listing and the badges / pairing cannot disagree between the platforms.
//
// Extensions only decide what a listing shows and how it is badged; a decode
// still probes magic bytes, never the extension (docs/design/04). Adding a
// format here is a D5 change (CLAUDE.md, docs/design/01).
// Portable, header-only, no OS headers (D9).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mv::io {

enum class media_kind : std::uint8_t {
  still,     // decoded as a still (not RAW)
  raw,       // camera RAW, LibRaw (D5)
  video,     // a D5 video container
  audio,     // D5 amended 2026-10-03: Open With only
  document,  // D5 amended 2026-10-03: Open With only
};

struct media_extension {
  std::string_view ext;  // with the dot, lower case
  media_kind kind;
};

inline constexpr media_extension kMediaExtensions[] = {
    {".jpg", media_kind::still},   {".jpeg", media_kind::still}, {".png", media_kind::still},
    {".bmp", media_kind::still},   {".gif", media_kind::still},  {".webp", media_kind::still},
    {".tif", media_kind::still},   {".tiff", media_kind::still}, {".ico", media_kind::still},
    {".heic", media_kind::still},  {".heif", media_kind::still}, {".hif", media_kind::still},
    {".avif", media_kind::still},

    {".dng", media_kind::raw},     {".cr2", media_kind::raw},    {".cr3", media_kind::raw},
    {".nef", media_kind::raw},     {".nrw", media_kind::raw},    {".arw", media_kind::raw},
    {".srf", media_kind::raw},     {".sr2", media_kind::raw},    {".orf", media_kind::raw},
    {".raf", media_kind::raw},     {".rw2", media_kind::raw},    {".pef", media_kind::raw},
    {".ptx", media_kind::raw},     {".srw", media_kind::raw},    {".rwl", media_kind::raw},
    {".3fr", media_kind::raw},     {".fff", media_kind::raw},    {".iiq", media_kind::raw},
    {".mef", media_kind::raw},     {".mos", media_kind::raw},    {".raw", media_kind::raw},

    {".mp4", media_kind::video},   {".mov", media_kind::video},  {".mkv", media_kind::video},
    {".webm", media_kind::video},  {".avi", media_kind::video},  {".ts", media_kind::video},
    {".m4v", media_kind::video},

    {".mp3", media_kind::audio},   {".m4a", media_kind::audio},  {".m4p", media_kind::audio},

    {".pdf", media_kind::document}, {".docx", media_kind::document},
    {".pptx", media_kind::document},  // D5 amended 2026-10-15 (#429)
};

namespace media_extension_detail {

// ASCII case-insensitive; `lower` is already lower case.
[[nodiscard]] constexpr bool equals_lower(std::string_view ext, std::string_view lower) noexcept {
  if (ext.size() != lower.size()) return false;
  for (std::size_t i = 0; i < ext.size(); ++i) {
    char c = ext[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != lower[i]) return false;
  }
  return true;
}

}  // namespace media_extension_detail

// The table row for `ext` (with its dot, any ASCII case), or null.
[[nodiscard]] constexpr const media_extension* find_media_extension(std::string_view ext) noexcept {
  for (const auto& e : kMediaExtensions) {
    if (media_extension_detail::equals_lower(ext, e.ext)) return &e;
  }
  return nullptr;
}

// True when a folder listing keeps a file with this extension.
[[nodiscard]] constexpr bool is_listed_extension(std::string_view ext) noexcept {
  return find_media_extension(ext) != nullptr;
}

[[nodiscard]] constexpr bool is_media_extension(std::string_view ext, media_kind kind) noexcept {
  const media_extension* e = find_media_extension(ext);
  return e != nullptr && e->kind == kind;
}

// The extension of a file name, dot included, or empty when there is none.
[[nodiscard]] constexpr std::string_view extension_of(std::string_view name) noexcept {
  const auto dot = name.find_last_of('.');
  if (dot == std::string_view::npos || dot + 1 >= name.size()) return {};
  return name.substr(dot);
}

static_assert(is_listed_extension(".JPG") && is_media_extension(".Cr3", media_kind::raw) &&
              is_media_extension(".mov", media_kind::video) && !is_listed_extension(".jxl") &&
              !is_listed_extension(".") && !is_listed_extension(""));

}  // namespace mv::io
