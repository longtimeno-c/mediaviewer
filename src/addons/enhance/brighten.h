// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance: "Brighten dark scenes" for a clip's picture (2026-11-02). No model:
// a tone lift fitted to each frame, held steady over time.
//
// Each frame's brightness is the geometric mean of its linear luminance (the
// scene's "key"), from every fourth pixel each way. A scene darker than the
// target key gets a gain g (at most `max_gain`), applied in linear light to R,
// G and B alike through a curve that lifts the shadows by g and never clips:
//
//     f(x) = g x / (1 + (g - 1) x),   f(0) = 0, f(1) = 1, f'(0) = g
//
// A scene at or above the target is left alone (g = 1), so a well-exposed
// clip comes back as it was. The gain follows the scenes smoothly (its log
// eased with a half-second time constant on the frames' own times), so it
// does not pump or flicker with a passing light; on a cut (the frame's
// brightness histogram changes by more than half) it moves at once. The
// curve is one 256-entry table per frame over 8-bit sRGB.
//
// Darker footage carries more noise, and lifting it shows the noise; the
// Detail model runs after this, on the brightened frame.
//
// Portable, no I/O (D9).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace mv::enhance {

enum class brighten_amount : std::uint8_t { off, some, more };

// The knob's value ("off", "some", "more"); off for anything else.
[[nodiscard]] brighten_amount brighten_of(std::string_view id) noexcept;

struct brighten_fit {
  double key = 0;     // the frame's geometric-mean linear luminance
  double target = 1;  // the gain this frame alone asks for
  double gain = 1;    // the gain applied, after easing
  bool cut = false;   // a scene change: the gain moved at once
};

class brightener {
 public:
  explicit brightener(brighten_amount amount) noexcept;

  [[nodiscard]] bool active() const noexcept { return amount_ != brighten_amount::off; }

  // `rgb`: width x height x 3 (8-bit sRGB, tightly packed) at `pts_us` on the
  // clip's timeline, frames in order. `out` becomes the lifted frame (the
  // same size). Returns what it did, for tests.
  brighten_fit apply(std::span<const std::uint8_t> rgb, std::uint32_t width, std::uint32_t height,
                     std::int64_t pts_us, std::vector<std::uint8_t>& out);

 private:
  brighten_amount amount_;
  bool started_ = false;
  double log_gain_ = 0;
  std::int64_t last_pts_ = 0;
  std::array<float, 32> last_hist_{};
};

// The lift curve, for tests: f(x) on linear light in [0, 1].
[[nodiscard]] double brighten_curve(double x, double gain) noexcept;

}  // namespace mv::enhance
