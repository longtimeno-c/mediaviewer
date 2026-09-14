// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <vector>

#include "core/job_system.h"
#include "core/result.h"
#include "image/colour.h"
#include "image/gpu_image.h"

struct ID3D11Device;

namespace mv::image {

// CPU Mitchell mip chain, then CreateTexture2D(IMMUTABLE) on `device`. Safe to
// call from a decode worker: the device is free-threaded
// (ID3D10Multithread + D3D11_SUBRESOURCE_DATA).
// `mip_limit` 0 = full D3D chain. 1 = top level only (first full-res present
// before the CPU pyramid of a large still).
[[nodiscard]] result<gpu_image> upload(ID3D11Device* device, const display_image& src,
                                       std::uint32_t generation,
                                       const job_context* ctx = nullptr,
                                       std::uint32_t mip_limit = 0);

// One 2x decimation step of that chain: separable Mitchell (B = C = 1/3) in
// linear light, sRGB-encoded result, floor-half extents (`max(1, floor(n/2))`,
// the D3D11 mip pitch — plan/03). Shared by the single-texture upload and the
// tiled pyramid (image/tiles.h). Any thread; holds four filtered rows at a time,
// not a float copy of the whole level. False only when `ctx` was cancelled.
[[nodiscard]] bool downsample_half(const std::uint8_t* src, std::uint32_t sw, std::uint32_t sh,
                                   std::vector<std::uint8_t>& dst, std::uint32_t& dw,
                                   std::uint32_t& dh, const job_context* ctx = nullptr);

// Mean sRGB luma (Rec.709 weights on the encoded values, 0..255) of a sparse
// grid of about 4096 samples. Cheap enough for every publish.
[[nodiscard]] std::uint8_t mean_luma(const display_image& src) noexcept;

}  // namespace mv::image
