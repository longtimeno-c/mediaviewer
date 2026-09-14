// SPDX-License-Identifier: GPL-2.0-or-later
// Shared helpers for the libFuzzer harnesses (PR 7, plan/09). Every harness is
// one decoder entry point; a successful result must be self-consistent, or the
// harness aborts so libFuzzer records the input as a crash — a decoder that
// returns ok with the wrong byte count is a heap overflow one stage later.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <vector>

#include "codec/decode.h"

namespace mv::fuzz {

inline std::span<const std::uint8_t> bytes(const std::uint8_t* data, std::size_t size) {
  return {data, size};
}

[[noreturn]] inline void fail(const char* what) {
  std::fprintf(stderr, "fuzz invariant violated: %s\n", what);
  std::fflush(stderr);
  std::abort();
}

inline void check(const result<codec::raster>& r, const char* who) {
  if (!r) return;
  const auto& img = r.value();
  if (img.width == 0 || img.height == 0) fail(who);
  if (img.rgba.size() != static_cast<std::uint64_t>(img.width) * img.height * 4) fail(who);
}

// Opens an animation and steps up to `max_frames`, rewinds, steps once more.
inline void step_animation(result<std::unique_ptr<codec::animation_source>> opened,
                           std::uint32_t max_frames) {
  if (!opened) return;
  auto& source = *opened.value();
  codec::canvas_frame frame;
  // Compositing cost is canvas area per frame. Cap the pixels stepped per
  // input (64 MP) so one 4096x4096-screen GIF in the corpus does not drag the
  // harness to ~50 exec/s; frame 0 and the rewind are always exercised.
  constexpr std::uint64_t kPixelBudget = 64ull * 1000 * 1000;
  const auto& first = source.info();
  const std::uint64_t area = std::max<std::uint64_t>(1, std::uint64_t{first.width} * first.height);
  max_frames = static_cast<std::uint32_t>(
      std::clamp<std::uint64_t>(kPixelBudget / area, 1, max_frames));
  for (int pass = 0; pass < 2; ++pass) {
    const std::uint32_t limit = pass == 0 ? max_frames : 1;
    for (std::uint32_t i = 0; i < limit; ++i) {
      auto more = source.next(frame, nullptr);
      if (!more || !more.value()) break;
      const auto& info = source.info();
      if (frame.rgba.size() != static_cast<std::uint64_t>(info.width) * info.height * 4) {
        fail("animation frame size does not match canvas");
      }
    }
    (void)source.rewind();
  }
}

inline std::shared_ptr<const std::vector<std::uint8_t>> shared(const std::uint8_t* data,
                                                               std::size_t size) {
  return std::make_shared<const std::vector<std::uint8_t>>(data, data + size);
}

}  // namespace mv::fuzz
