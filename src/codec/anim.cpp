// SPDX-License-Identifier: GPL-2.0-or-later
#include "codec/anim.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace mv::codec {
namespace {

constexpr std::uint64_t kMaxCanvasPixels = 256ull * 1000ull * 1000ull;

bool fits(const frame_region& r, std::uint32_t w, std::uint32_t h) noexcept {
  return r.width > 0 && r.height > 0 &&
         static_cast<std::uint64_t>(r.x) + r.width <= w &&
         static_cast<std::uint64_t>(r.y) + r.height <= h;
}

void clear_rect(std::vector<std::uint8_t>& canvas, std::uint32_t canvas_w,
                const frame_region& r) noexcept {
  for (std::uint32_t row = 0; row < r.height; ++row) {
    const std::size_t off = (static_cast<std::size_t>(r.y) + row) * canvas_w * 4 +
                            static_cast<std::size_t>(r.x) * 4;
    std::memset(canvas.data() + off, 0, static_cast<std::size_t>(r.width) * 4);
  }
}

}  // namespace

frame_position frame_at(std::span<const std::uint32_t> delays_ms, std::uint32_t loops,
                        std::uint64_t elapsed_ms) noexcept {
  frame_position pos;
  if (delays_ms.empty()) {
    pos.finished = true;
    return pos;
  }
  std::uint64_t total = 0;
  for (const std::uint32_t d : delays_ms) total += d;
  const auto last = static_cast<std::uint32_t>(delays_ms.size() - 1);
  if (total == 0 || delays_ms.size() == 1) {
    pos.index = 0;
    pos.finished = true;
    return pos;
  }
  const std::uint64_t cycle = elapsed_ms / total;
  if (loops != 0 && cycle >= loops) {
    pos.index = last;
    pos.finished = true;
    return pos;
  }
  const std::uint64_t t = elapsed_ms % total;
  std::uint64_t start = 0;
  for (std::uint32_t i = 0; i < delays_ms.size(); ++i) {
    const std::uint64_t end = start + delays_ms[i];
    if (t < end) {
      pos.index = i;
      pos.next_change_ms = cycle * total + end;
      return pos;
    }
    start = end;
  }
  pos.index = last;
  return pos;
}

bool compositor::reset(std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0 ||
      static_cast<std::uint64_t>(width) * height > kMaxCanvasPixels) {
    return false;
  }
  try {
    canvas_.assign(static_cast<std::size_t>(width) * height * 4, 0);
  } catch (const std::bad_alloc&) {
    return false;
  }
  saved_.clear();
  width_ = width;
  height_ = height;
  have_previous_ = false;
  first_frame_ = true;
  previous_ = {};
  return true;
}

bool compositor::draw(const frame_region& region, std::span<const std::uint8_t> rgba) {
  if (canvas_.empty() || !fits(region, width_, height_)) return false;
  if (rgba.size() != static_cast<std::size_t>(region.width) * region.height * 4) return false;

  // The previous frame's disposal happens before this frame is drawn.
  if (have_previous_) {
    if (previous_.dispose == dispose_op::background) {
      clear_rect(canvas_, width_, previous_);
    } else if (previous_.dispose == dispose_op::previous && saved_.size() == canvas_.size()) {
      for (std::uint32_t row = 0; row < previous_.height; ++row) {
        const std::size_t off = (static_cast<std::size_t>(previous_.y) + row) * width_ * 4 +
                                static_cast<std::size_t>(previous_.x) * 4;
        std::memcpy(canvas_.data() + off, saved_.data() + off,
                    static_cast<std::size_t>(previous_.width) * 4);
      }
    }
  }

  // APNG: "previous" on the first frame is treated as "background". Either
  // way, save the canvas now if this frame will need restoring.
  frame_region current = region;
  if (first_frame_ && current.dispose == dispose_op::previous) current.dispose = dispose_op::background;
  if (current.dispose == dispose_op::previous) {
    try {
      saved_ = canvas_;
    } catch (const std::bad_alloc&) {
      current.dispose = dispose_op::background;
    }
  }

  for (std::uint32_t row = 0; row < region.height; ++row) {
    const std::uint8_t* src = rgba.data() + static_cast<std::size_t>(row) * region.width * 4;
    std::uint8_t* dst = canvas_.data() +
                        (static_cast<std::size_t>(region.y) + row) * width_ * 4 +
                        static_cast<std::size_t>(region.x) * 4;
    if (region.blend == blend_op::source) {
      std::memcpy(dst, src, static_cast<std::size_t>(region.width) * 4);
      continue;
    }
    for (std::uint32_t col = 0; col < region.width; ++col, src += 4, dst += 4) {
      const unsigned sa = src[3];
      if (sa == 255) {
        std::memcpy(dst, src, 4);
        continue;
      }
      if (sa == 0) continue;
      const unsigned da = dst[3];
      // Straight-alpha "over", in 8-bit fixed point.
      const unsigned out_a = sa + (da * (255 - sa) + 127) / 255;
      for (int c = 0; c < 3; ++c) {
        const unsigned num = src[c] * sa * 255 + dst[c] * da * (255 - sa);
        dst[c] = static_cast<std::uint8_t>((num + out_a * 255 / 2) / (out_a * 255));
      }
      dst[3] = static_cast<std::uint8_t>(out_a);
    }
  }

  previous_ = current;
  have_previous_ = true;
  first_frame_ = false;
  return true;
}

void animation_clock::step(std::span<const std::uint32_t> delays_ms, std::uint32_t loops,
                           int direction, std::uint64_t now_ms) noexcept {
  if (delays_ms.empty()) return;
  std::uint64_t total = 0;
  for (const std::uint32_t d : delays_ms) total += d;
  const auto count = static_cast<std::uint32_t>(delays_ms.size());
  const std::uint64_t now = elapsed(now_ms);
  const frame_position at = frame_at(delays_ms, loops, now);
  const std::uint32_t target =
      direction >= 0 ? (at.index + 1) % count : (at.index + count - 1) % count;
  std::uint64_t start = 0;
  for (std::uint32_t i = 0; i < target; ++i) start += delays_ms[i];
  std::uint64_t cycle = total > 0 ? now / total : 0;
  if (loops != 0 && cycle >= loops) cycle = loops - 1;  // stay inside the last play
  paused_at_ms_ = cycle * total + start;
  paused_ = true;
}

}  // namespace mv::codec
