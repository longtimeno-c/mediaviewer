// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Animation timing and compositing shared by GIF, APNG and animated WebP
// (plan/04 "Animation"). Pure CPU code: no decoder, no clock, no GPU.
//
// Timing follows browsers, because PR 6's verify says "animation timing
// matches a browser": Chromium and Firefox treat a frame delay of 10 ms or
// less as 100 ms. (plan/04 said "< 20 ms"; for GIF's centisecond delays the two
// rules agree, and for APNG / WebP millisecond delays the browser rule is the
// one people compare against — plan/12 2026-09-13.)
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include "codec/format.h"
#include "core/job_system.h"
#include "core/result.h"

namespace mv::codec {

[[nodiscard]] constexpr std::uint32_t browser_frame_delay_ms(std::uint32_t ms) noexcept {
  return ms <= 10 ? 100 : ms;
}

// GIF Graphic Control Extension delay, in hundredths of a second.
[[nodiscard]] constexpr std::uint32_t gif_delay_ms(std::uint16_t centiseconds) noexcept {
  return browser_frame_delay_ms(static_cast<std::uint32_t>(centiseconds) * 10u);
}

// APNG fcTL delay_num / delay_den seconds; a zero denominator means 1/100 s.
[[nodiscard]] constexpr std::uint32_t apng_delay_ms(std::uint16_t num, std::uint16_t den) noexcept {
  const std::uint64_t d = den == 0 ? 100u : den;
  return browser_frame_delay_ms(
      static_cast<std::uint32_t>((static_cast<std::uint64_t>(num) * 1000u + d / 2) / d));
}

struct frame_position {
  std::uint32_t index = 0;
  bool finished = false;  // loop count exhausted: hold the last frame, stop presenting
  // Elapsed time at which the frame changes next; max() when it never will.
  std::uint64_t next_change_ms = std::numeric_limits<std::uint64_t>::max();
};

// Which frame is due `elapsed_ms` after the animation started. `loops` is the
// file's play count; 0 means forever. The render thread asks this against QPC
// each vblank, so it is O(frames) and allocation-free.
[[nodiscard]] frame_position frame_at(std::span<const std::uint32_t> delays_ms,
                                      std::uint32_t loops, std::uint64_t elapsed_ms) noexcept;

// What happens to a frame's rectangle before the next frame is drawn.
enum class dispose_op : std::uint8_t { none = 0, background = 1, previous = 2 };
enum class blend_op : std::uint8_t { source = 0, over = 1 };

struct frame_region {
  std::uint32_t x = 0;
  std::uint32_t y = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  dispose_op dispose = dispose_op::none;
  blend_op blend = blend_op::source;
};

// The canvas an animation draws frames into, frame by frame, in order. RGBA8,
// straight (not premultiplied) alpha, matching the still decoders.
class compositor {
 public:
  // Clears to transparent black. Returns false if the size is unusable.
  bool reset(std::uint32_t width, std::uint32_t height);

  // Applies the previous frame's disposal, then draws `rgba` (region.width x
  // region.height) at the region. Returns false, drawing nothing, if the
  // region does not fit the canvas or the pixel span is the wrong size — a
  // hostile file never writes out of bounds.
  bool draw(const frame_region& region, std::span<const std::uint8_t> rgba);

  [[nodiscard]] std::span<const std::uint8_t> pixels() const noexcept { return canvas_; }
  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

 private:
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  std::vector<std::uint8_t> canvas_;
  std::vector<std::uint8_t> saved_;  // canvas before a dispose-previous frame
  bool have_previous_ = false;
  bool first_frame_ = true;
  frame_region previous_{};
};

// Decoded animation: every frame already composited onto the full canvas, so
// presenting frame N is a texture swap, not a replay of disposal ops.
struct animation_frames {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t loops = 0;  // 0 = forever
  format_family format = format_family::unknown;
  std::vector<std::uint32_t> delays_ms;           // browser-clamped, one per frame
  std::vector<std::vector<std::uint8_t>> frames;  // RGBA8 straight alpha, width*height*4 each
  // Colour tagging, as for a still raster: the frames are source-encoded.
  std::vector<std::uint8_t> icc;
  bool tagged_srgb = false;
};

// decode_animation (tests and tools) collects every frame; this caps what it
// will hold. Playback never collects frames — it pulls them one at a time from
// an animation_source into a small ring (plan/04).
inline constexpr std::size_t kAnimationByteBudget = 512ull * 1024ull * 1024ull;

struct animation_info {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t loops = 1;        // 0 = forever
  std::uint32_t frame_count = 0;  // 0 = not known yet (GIF learns it at the end of a play)
  format_family format = format_family::unknown;
  // Colour tagging, as for a still raster: frames are source-encoded.
  std::vector<std::uint8_t> icc;
  bool tagged_srgb = false;
};

struct canvas_frame {
  std::uint32_t index = 0;
  std::uint32_t delay_ms = 0;        // browser-clamped
  std::vector<std::uint8_t> rgba;    // width*height*4, straight alpha; reused between calls
};

// One animation, decoded one frame at a time (plan/04: "decode frames ahead
// into a small ring"). A source holds its canvas and, for dispose-previous, one
// saved copy — never all frames — so a long 4K animation costs a few canvases,
// not its whole length, and frame 0 is ready before frame 1 is decoded.
// One decode worker at a time; not thread-safe.
class animation_source {
 public:
  virtual ~animation_source() = default;

  // Size, loop count and colour tagging. `loops` and `frame_count` may only
  // become known while frames are decoded (a GIF's NETSCAPE extension, its end).
  [[nodiscard]] virtual const animation_info& info() const noexcept = 0;

  // Composites the next frame into `out`: true with a frame, false at the end
  // of one play. A file that breaks after frame 0 ends the play there.
  [[nodiscard]] virtual result<bool> next(canvas_frame& out, const job_context* ctx) = 0;

  // Back before frame 0: a loop wrap, or a step backwards.
  [[nodiscard]] virtual expected rewind() = 0;
};

// Play / pause / frame step for an animation on the QPC frame clock (plan/16:
// when the current item is animated, Space plays and pauses, `,` `.` step
// frames, like video). Pure: the caller passes "now" in milliseconds.
// Paused holds a frame and lets the canvas idle; stepping pauses, as it does
// for a clip.
class animation_clock {
 public:
  void start(std::uint64_t now_ms) noexcept {
    origin_ms_ = now_ms;
    paused_at_ms_ = 0;
    paused_ = false;
  }

  [[nodiscard]] bool paused() const noexcept { return paused_; }

  // Animation time at `now`, for frame_at.
  [[nodiscard]] std::uint64_t elapsed(std::uint64_t now_ms) const noexcept {
    if (paused_) return paused_at_ms_;
    return now_ms >= origin_ms_ ? now_ms - origin_ms_ : 0;
  }

  void toggle(std::uint64_t now_ms) noexcept {
    if (paused_) {
      origin_ms_ = now_ms >= paused_at_ms_ ? now_ms - paused_at_ms_ : 0;
      paused_ = false;
    } else {
      paused_at_ms_ = elapsed(now_ms);
      paused_ = true;
    }
  }

  // Pause on the start of the next (direction > 0) or previous frame,
  // wrapping within the current loop. Never finishes a finite loop count.
  void step(std::span<const std::uint32_t> delays_ms, std::uint32_t loops, int direction,
            std::uint64_t now_ms) noexcept;

 private:
  std::uint64_t origin_ms_ = 0;
  std::uint64_t paused_at_ms_ = 0;
  bool paused_ = false;
};

// When the next animation frame is due, for frames that arrive one at a time
// from a decode ring instead of a complete delay list (plan/04). Pure: the
// render thread passes QPC milliseconds.
class frame_schedule {
 public:
  void reset() noexcept { *this = frame_schedule{}; }

  // The next frame should be taken now (the first one always is).
  [[nodiscard]] bool due(std::uint64_t now_ms) const noexcept {
    return !paused_ && (!showing_ || now_ms >= next_due_ms_);
  }

  // A frame with `delay_ms` went on screen at `now_ms`. The file's cadence is
  // kept against the schedule, not against when the frame happened to arrive;
  // a frame later than `slack_ms` (the decoder fell behind) restarts the
  // cadence from now and is counted.
  void shown(std::uint32_t delay_ms, std::uint64_t now_ms, std::uint32_t slack_ms) noexcept {
    if (!showing_ || now_ms > next_due_ms_ + slack_ms) {
      if (showing_) ++late_;
      next_due_ms_ = now_ms + delay_ms;
    } else {
      next_due_ms_ += delay_ms;
    }
    showing_ = true;
  }

  // Paused holds the frame and keeps what was left of its delay for resume.
  void pause(std::uint64_t now_ms) noexcept {
    if (paused_) return;
    paused_ = true;
    remaining_ms_ = next_due_ms_ > now_ms ? next_due_ms_ - now_ms : 0;
  }
  void resume(std::uint64_t now_ms) noexcept {
    if (!paused_) return;
    paused_ = false;
    next_due_ms_ = now_ms + remaining_ms_;
  }

  // A frame stepped to while paused (`,` `.`): on resume it stays its full delay.
  void stepped(std::uint32_t delay_ms) noexcept {
    showing_ = true;
    remaining_ms_ = delay_ms;
  }

  [[nodiscard]] bool paused() const noexcept { return paused_; }
  [[nodiscard]] std::uint32_t late() const noexcept { return late_; }
  [[nodiscard]] std::uint64_t next_due_ms() const noexcept { return next_due_ms_; }

 private:
  std::uint64_t next_due_ms_ = 0;
  std::uint64_t remaining_ms_ = 0;
  std::uint32_t late_ = 0;
  bool showing_ = false;
  bool paused_ = false;
};

}  // namespace mv::codec
