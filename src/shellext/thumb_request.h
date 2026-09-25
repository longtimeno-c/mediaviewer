// SPDX-License-Identifier: GPL-2.0-or-later
// PR 15 (plan/09 "Windows integration", plan/10 PR 15): the portable half of
// the Explorer thumbnail handler. Bytes in, BGRA pixels out, with the limits
// plan/09 asks of anything a shell process runs: a hard cap on what it reads,
// a deadline after which it answers "no thumbnail", and nothing shared with
// the app (no cache, no decoder instance, no state between requests).
//
// shellext/ is a host, like shell/: Explorer's. It sits beside shell/ at the
// top of the graph and reaches the core only through image/ and codec/.
#pragma once

#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

#include "core/job_system.h"
#include "core/result.h"

namespace mv::shellext {

// A request for a file larger than this is refused unread: a 1 GB "photo" in
// a camera dump is a damaged file, and a surrogate that runs out of memory
// takes every other thumbnail in flight with it. Quick Look uses the same cap.
inline constexpr std::uint64_t kMaxSourceBytes = 512ull * 1024u * 1024u;
// Explorer asks for up to 256 px (Extra large icons) at 100 %, 2.5x that at
// 250 %. Anything larger is clamped, never refused.
inline constexpr std::uint32_t kMaxThumbEdge = 1024;
// After this the handler answers "no thumbnail" and cancels the decode.
inline constexpr std::chrono::milliseconds kDeadline{4000};

// Top-down BGRA, what a 32-bpp DIB section holds. Premultiplied when
// `has_alpha` (WTSAT_ARGB); opaque otherwise (WTSAT_RGB).
struct bgra_thumb {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  bool has_alpha = false;
  std::vector<std::uint8_t> bgra;
};

// The long edge is at most `cx` (clamped to [16, kMaxThumbEdge]); never
// enlarged. Same pixel path as the app's own thumbnails (make_thumb_rgba).
[[nodiscard]] result<bgra_thumb> render_thumbnail(std::span<const std::uint8_t> bytes,
                                                  std::uint32_t cx,
                                                  const job_context* ctx = nullptr);

// render_thumbnail on its own thread, waited for at most `deadline`. Late:
// status::cancelled, and the decode is told to stop; the thread owns its
// copy of the bytes and ends on its own. Any exception (out of memory in a
// decoder) is status::out_of_memory / internal, never a throw into COM.
[[nodiscard]] result<bgra_thumb> render_thumbnail_by(std::vector<std::uint8_t> bytes,
                                                     std::uint32_t cx,
                                                     std::chrono::milliseconds deadline = kDeadline);

}  // namespace mv::shellext
