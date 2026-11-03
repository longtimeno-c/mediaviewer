// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// One still out of a clip, for the filmstrip and the gallery.
//
// docs/design/04's thumbnail cache is format-agnostic: it stores a JPEG-512 keyed by
// (path, mtime, size). What it could not do was produce one for a video, so a
// camera dump — which is photos AND clips in one folder — listed its clips as
// blank tiles. This is the missing producer: software-decode one frame near the
// head of the clip and hand back RGBA for the same downscale-and-encode path
// every photo already takes.
//
// Deliberately NOT the playback pipeline. No D3D11VA, no ring, no clock, no
// threads: a thumbnail is a one-shot on a pool thread, and hardware-decoding it
// would contend with the clip the user is actually watching for decoder
// sessions. Software, single-threaded, one frame, closed.
//
// [pool-thread] only. It blocks on I/O and decode, so it must never be called
// from the UI or render thread (CLAUDE.md rule 1).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/job_system.h"
#include "core/result.h"

namespace mv::player {

struct poster_image {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgba;  // tightly packed, width * height * 4
};

// Decodes one frame near the head of `utf8_path`, scaled so its long edge is at
// most `max_long_edge` (with the stream's sample aspect ratio applied, so
// anamorphic clips are not thumbed as squashed). `ctx`, when given, is polled
// for cancellation — navigating away must abandon this like any other job.
[[nodiscard]] result<poster_image> poster_frame(const char* utf8_path,
                                                std::uint32_t max_long_edge,
                                                const job_context* ctx = nullptr);

// A seekable byte source for a clip there is no path to (issue #610): the
// Explorer thumbnail handler's IStream, memory in a test. FFmpeg reads it
// through a custom AVIOContext, so a 4 GB clip costs a few 64 KiB reads, never
// the clip in memory. Called only from the thread running poster_frame, one
// call at a time; an implementation needs no lock of its own.
class byte_source {
 public:
  byte_source() = default;
  byte_source(const byte_source&) = delete;
  byte_source& operator=(const byte_source&) = delete;
  virtual ~byte_source() = default;

  // Up to `out.size()` bytes from `offset`: the count read, 0 at or past the
  // end, an error when the source fails (a share that went away mid-read).
  [[nodiscard]] virtual result<std::size_t> read_at(std::uint64_t offset,
                                                    std::span<std::uint8_t> out) noexcept = 0;
  // The total size in bytes. A container's index often sits at the end (an
  // MKV's cues, an MP4's moov), so it must be known; 0 is an empty source.
  [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
};

// The same poster as the path overload (same frame, aspect, display matrix
// and audio card), read through `source`. `ctx` is also polled from inside
// FFmpeg's reads, so a cancel lands even while the demuxer is probing.
[[nodiscard]] result<poster_image> poster_frame(byte_source& source, std::uint32_t max_long_edge,
                                                const job_context* ctx = nullptr);

}  // namespace mv::player
