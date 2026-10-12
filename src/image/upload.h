// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

#include "core/job_system.h"
#include "core/result.h"
#include "image/colour.h"
#include "image/gpu_image.h"

struct ID3D11Device;

namespace mv::image {

// docs/design/03: worker uploads count against ~2 ms of upload per frame.
inline constexpr std::uint32_t k_frame_upload_budget_us = 2000;

// Issue #166: a single-texture create of more than this many bytes (all mips)
// is assumed not to fit one frame's upload budget: about 2 ms of driver copy
// at ~4 GB/s, a ~2 MP still with its mips. An estimate to calibrate against
// the soak report's texture_max_create_ms / texture_max_create_mb.
inline constexpr std::uint64_t k_frame_budget_create_bytes = 8ull * 1024 * 1024;

// Issue #166: where an over-budget create may wait. ID3D10Multithread holds the
// device lock for the whole CreateTexture2D, so the render thread's presents
// stall behind it; a create that cannot fit a frame is budgeted by running it
// when no moving frame is being presented. upload() calls `wait` on the worker
// after the CPU mip chain and right before the create, with the texture's
// bytes; `wait` returns false to abandon the upload (cancelled). The texture is
// still one IMMUTABLE create with D3D11_SUBRESOURCE_DATA (docs/design/02).
struct create_gate {
  bool (*wait)(void* user, std::uint64_t texture_bytes) = nullptr;
  void* user = nullptr;
};

// CPU Mitchell mip chain, then CreateTexture2D(IMMUTABLE) on `device`. Safe to
// call from a decode worker: the device is free-threaded
// (ID3D10Multithread + D3D11_SUBRESOURCE_DATA).
// `mip_limit` 0 = full D3D chain. 1 = top level only (first full-res present
// before the CPU pyramid of a large still). `gate` (optional) runs before the
// create, as above.
[[nodiscard]] result<gpu_image> upload(ID3D11Device* device, const display_image& src,
                                       std::uint32_t generation,
                                       const job_context* ctx = nullptr,
                                       std::uint32_t mip_limit = 0,
                                       const create_gate* gate = nullptr);

// Issue #166: what the single-texture upload's CreateTexture2D costs (the CPU
// mip chain before it is not counted). A still just under the tile threshold
// goes up whole, ~320 MB with its mips, and ID3D10Multithread serialises that
// create with the render thread's immediate context. Process-wide, any
// thread, approximate by construction (relaxed atomics); the tile path has its
// own (tile_stats::last_create_us).
struct texture_upload_stats {
  std::uint64_t creates = 0;
  std::uint64_t over_budget = 0;      // creates longer than k_frame_upload_budget_us
  std::uint32_t last_create_us = 0;
  std::uint32_t max_create_us = 0;
  std::uint64_t max_create_bytes = 0;  // the texture (all mips) behind max_create_us
  // Creates that a create_gate held back (waited over 1 ms) until the canvas
  // stopped moving, and the longest such wait.
  std::uint64_t deferred = 0;
  std::uint32_t max_defer_us = 0;
};
[[nodiscard]] texture_upload_stats texture_upload_stats_now() noexcept;

// One 2x decimation step of that chain: separable Mitchell (B = C = 1/3) in
// linear light, sRGB-encoded result, floor-half extents (`max(1, floor(n/2))`,
// the D3D11 mip pitch — docs/design/03). Shared by the single-texture upload and the
// tiled pyramid (image/tiles.h). Any thread; holds four filtered rows at a time,
// not a float copy of the whole level. False only when `ctx` was cancelled.
[[nodiscard]] bool downsample_half(const std::uint8_t* src, std::uint32_t sw, std::uint32_t sh,
                                   std::vector<std::uint8_t>& dst, std::uint32_t& dw,
                                   std::uint32_t& dh, const job_context* ctx = nullptr);

// Mean sRGB luma (Rec.709 weights on the encoded values, 0..255) of a sparse
// grid of about 4096 samples. Cheap enough for every publish.
[[nodiscard]] std::uint8_t mean_luma(const display_image& src) noexcept;

}  // namespace mv::image
