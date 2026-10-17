// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The frame-time histogram both pacers score the PR 1 gate with (gfx/pacer.h on
// D3D11, gfx/metal_pacer.h on Metal). One copy, so the gate's maths cannot drift
// between the platforms (issue #170). Portable, header-only, names no GPU API (D9).
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace mv::gfx {

class frame_histogram {
 public:
  // 0.05 ms buckets up to 60 ms, plus an overflow bucket. Fixed-size so the
  // render thread never allocates.
  static constexpr std::size_t bucket_count = 1201;
  static constexpr double bucket_ms = 0.05;

  void clear() noexcept {
    buckets_.fill(0);
    count_ = 0;
  }

  void add(double ms) noexcept {
    // Negative or NaN lands in bucket 0 rather than indexing out of range.
    std::size_t index = 0;
    if (ms > 0.0) {
      const double slot = ms / bucket_ms;
      index = slot >= static_cast<double>(bucket_count - 1) ? bucket_count - 1
                                                            : static_cast<std::size_t>(slot);
    }
    ++buckets_[index];
    ++count_;
  }

  [[nodiscard]] std::uint64_t count() const noexcept { return count_; }

  // The `fraction` percentile, reported as its bucket's upper edge: percentiles
  // should never flatter. Zero when empty.
  [[nodiscard]] double percentile(double fraction) const noexcept {
    if (count_ == 0) return 0.0;
    const auto target =
        static_cast<std::uint64_t>(std::ceil(static_cast<double>(count_) * fraction));
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < bucket_count; ++i) {
      seen += buckets_[i];
      if (seen >= target) return static_cast<double>(i + 1) * bucket_ms;
    }
    return static_cast<double>(bucket_count) * bucket_ms;
  }

 private:
  std::array<std::uint32_t, bucket_count> buckets_{};
  std::uint64_t count_ = 0;
};

}  // namespace mv::gfx
