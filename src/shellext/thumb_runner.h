// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// shellext-private: the one bounded runner both handler paths share (issue
// #610). A still (render_thumbnail_by) and a clip poster (render_poster_by) take
// slots from the same kMaxInFlight cap and answer within the same deadline, so
// a folder of MKVs cannot starve the photos beside it of a surrogate, or the
// other way round. Nothing outside src/shellext includes this.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/job_system.h"
#include "core/result.h"
#include "shellext/thumb_request.h"

namespace mv::shellext::detail {

// One request's work and everything it reads. The runner's thread owns it
// until the work returns, past the caller's deadline if need be, so the bytes
// or the stream it reads stay alive for exactly as long as they are read.
class thumb_work {
 public:
  thumb_work() = default;
  thumb_work(const thumb_work&) = delete;
  thumb_work& operator=(const thumb_work&) = delete;
  virtual ~thumb_work() = default;
  [[nodiscard]] virtual result<bgra_thumb> run(const job_context& ctx) = 0;
};

// Runs `work` on its own thread, waited for at most `deadline`. Late:
// status::cancelled and `ctx` is cancelled; at the cap: status::busy and
// nothing started. Never throws.
[[nodiscard]] result<bgra_thumb> run_bounded(std::unique_ptr<thumb_work> work,
                                             std::chrono::milliseconds deadline) noexcept;

// Straight RGBA -> BGRA, premultiplied where not opaque (a DIB with alpha is
// premultiplied). Takes the buffer over; no copy.
[[nodiscard]] bgra_thumb to_bgra(std::uint32_t width, std::uint32_t height,
                                 std::vector<std::uint8_t>&& rgba) noexcept;

}  // namespace mv::shellext::detail
