// SPDX-License-Identifier: GPL-2.0-or-later
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

// The D5 v1 container set. Anything else is not_video, including formats we
// deliberately do not ship in v1 — being able to name a container is not the
// same as being willing to open it.
enum class container : std::uint8_t {
  unknown = 0,
  mp4,        // ISO-BMFF: .mp4, .m4v, and most .mov from phones
  quicktime,  // classic QuickTime, also ISO-BMFF shaped
  matroska,   // .mkv and .webm — same EBML magic, distinguished by DocType
  webm,
  avi,        // RIFF
  mpeg_ts,    // .ts, 0x47 sync bytes on a 188-byte cadence
};

// How many bytes probe() can make use of. Reading more is wasted I/O; reading
// fewer means MPEG-TS cannot be confirmed, since a single 0x47 proves nothing.
inline constexpr std::size_t probe_bytes = 1024;

[[nodiscard]] container probe(std::span<const std::uint8_t> head) noexcept;

// True for every container in the D5 v1 video set.
[[nodiscard]] constexpr bool is_video(container c) noexcept {
  return c != container::unknown;
}

[[nodiscard]] const char* container_name(container c) noexcept;

}  // namespace mv::player
