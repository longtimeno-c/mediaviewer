// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

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

}  // namespace mv::image
