// SPDX-License-Identifier: GPL-2.0-or-later
// One still out of a clip, for the filmstrip and the gallery.
//
// plan/04's thumbnail cache is format-agnostic: it stores a JPEG-512 keyed by
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

#include <cstdint>
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

}  // namespace mv::player
