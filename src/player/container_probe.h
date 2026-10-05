// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 5 — is this file a clip we can open?
//
// CLAUDE.md: "Probe by magic bytes, never extension." A camera dump is full of
// files whose extension lies — .MOV written by a phone that is really MP4,
// .AVI that is really Matroska, and (the one that actually bites) a .MP4 that
// is a JPEG someone renamed. Deciding image-vs-video by extension puts the
// wrong decoder on the file and reports a corrupt clip instead of a photo.
//
// Pure: takes bytes, returns an answer. No I/O, no FFmpeg, no allocation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace mv::player {

// The containers the player opens: the D5 video set plus the audio-only files
// (MP3, iTunes M4A/M4P — docs/plans/audio-and-documents.md). Anything else is
// unknown, including formats we deliberately do not open — being able to name a
// container is not the same as being willing to open it.
enum class container : std::uint8_t {
  unknown = 0,
  mp4,        // ISO-BMFF: .mp4, .m4v, and most .mov from phones
  quicktime,  // classic QuickTime, also ISO-BMFF shaped
  matroska,   // .mkv and .webm — same EBML magic, distinguished by DocType
  webm,
  avi,        // RIFF
  mpeg_ts,    // .ts, 0x47 sync bytes on a 188-byte cadence
  mp3,        // ID3v2 tag, or an MPEG audio frame header
  m4a,        // ISO-BMFF with an iTunes audio brand: M4A, M4B, M4P (FairPlay)
};

// How many bytes probe() can make use of. Reading more is wasted I/O; reading
// fewer means MPEG-TS cannot be confirmed, since a single 0x47 proves nothing.
inline constexpr std::size_t probe_bytes = 1024;

[[nodiscard]] container probe(std::span<const std::uint8_t> head) noexcept;

// True for every container the player opens (a clip or an audio file). Named
// for the clip routing it drives: an audio file goes the clip's way.
[[nodiscard]] constexpr bool is_video(container c) noexcept {
  return c != container::unknown;
}

[[nodiscard]] constexpr bool is_audio(container c) noexcept {
  return c == container::mp3 || c == container::m4a;
}

[[nodiscard]] const char* container_name(container c) noexcept;

}  // namespace mv::player
