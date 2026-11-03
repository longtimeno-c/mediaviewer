// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #610: a clip's thumbnail for the OS file browser (Explorer, Finder, and
// later Linux's thumbnailer), the poster the filmstrip shows: the same frame,
// sample aspect and display matrix (player::poster_frame), as BGRA for the
// shell. Beside render_thumbnail in thumb_request.h, with the same edge clamp,
// the same deadline and the same in-flight cap.
//
// A clip is never read whole. The host hands over a seekable reader (an
// IStream on Windows, the file on macOS and Linux) and FFmpeg reads through
// it, so a 4 GB clip costs a few reads near its head and its index, never
// 4 GB of memory and never kMaxSourceBytes. Software decode, one thread, one
// frame: no GPU and no hardware decoder session in a shell surrogate.
//
// Its own translation unit so the still path (and fuzz_thumbnail's still-only
// builds) need not link FFmpeg.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "core/job_system.h"
#include "core/result.h"
#include "player/poster.h"
#include "shellext/thumb_request.h"

namespace mv::shellext {

// The long edge is at most `cx` (clamped to [16, kMaxThumbEdge]); never
// enlarged. An audio-only file gets the player's music card, as in the app.
[[nodiscard]] result<bgra_thumb> render_poster(player::byte_source& source, std::uint32_t cx,
                                               const job_context* ctx = nullptr);

// render_poster on its own thread, as render_thumbnail_by: waited for at most
// `deadline` (late: status::cancelled, and the reads and the decode are told
// to stop), refused unstarted at the shared kMaxInFlight cap (status::busy).
// The thread owns `source` and releases it when the decode ends, which may be
// after this returns. Never throws.
[[nodiscard]] result<bgra_thumb> render_poster_by(std::unique_ptr<player::byte_source> source,
                                                  std::uint32_t cx,
                                                  std::chrono::milliseconds deadline = kDeadline);

// A byte_source over memory the caller keeps alive (tests, fuzz_thumbnail).
class span_source final : public player::byte_source {
 public:
  explicit span_source(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}
  result<std::size_t> read_at(std::uint64_t offset, std::span<std::uint8_t> out) noexcept override;
  std::uint64_t size() const noexcept override { return bytes_.size(); }

 private:
  std::span<const std::uint8_t> bytes_;
};

// A byte_source over a file, for the hosts that are handed a path: Finder's
// Quick Look extension and the planned Linux thumbnailer (docs/plans/linux.md
// §5). Null when the file cannot be opened or is empty. The Explorer handler
// never uses it: it is given a stream, not a path.
[[nodiscard]] std::unique_ptr<player::byte_source> open_file_source(const char* utf8_path);

}  // namespace mv::shellext
