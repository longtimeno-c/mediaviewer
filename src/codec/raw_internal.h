// SPDX-License-Identifier: GPL-2.0-or-later
// Private to src/codec/raw.cpp and tests/test_raw.cpp. Not part of the codec
// registry: decode.h's decode_raw / decode_raw_preview / looks_like_raw are
// the only entry points the pipeline uses. These knobs exist so the choices
// baked into those defaults (demosaic, brightness, preview scale) can be
// re-measured on real files instead of argued about.
#pragma once

#include <cstdint>
#include <span>

#include "codec/raster.h"
#include "core/job_system.h"
#include "core/result.h"

namespace mv::codec::raw_detail {

// LibRaw `user_qual`. Only the built-in, non-GPL interpolators are listed —
// the GPL demosaic packs are never linked (plan/11).
enum class demosaic : int {
  linear = 0,
  vng = 1,
  ppg = 2,
  ahd = 3,
};

struct raw_timings {
  double open_ms = 0, unpack_ms = 0, process_ms = 0, mem_ms = 0, pack_ms = 0;
  unsigned threads = 0;
};

struct raw_options {
  // Benchmark seam; production shares three extra workers across decodes.
  unsigned thread_limit = 4;
  raw_timings* timings = nullptr; // optional, caller-owned decode diagnostics
  demosaic quality = demosaic::ahd;
  // LibRaw's histogram auto-bright (dcraw default: 1 % of pixels clip).
  // false = fixed white point from the camera's white level.
  bool auto_bright = true;
  // Preview DCT scale: the largest of 1/2/4/8 that keeps the preview's long
  // side at or above this. 0 decodes the embedded JPEG at 1:1.
  std::uint32_t preview_min_long_side = 0;
};

[[nodiscard]] result<raster> decode_raw_with(std::span<const std::uint8_t> bytes,
                                             const job_context* ctx, const raw_options& opt);
[[nodiscard]] result<raster> decode_raw_preview_with(std::span<const std::uint8_t> bytes,
                                                     const job_context* ctx,
                                                     const raw_options& opt);

// The default options decode_raw / decode_raw_preview use.
[[nodiscard]] raw_options default_options() noexcept;

}  // namespace mv::codec::raw_detail
