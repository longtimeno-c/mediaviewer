// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

#include "core/job_system.h"
#include "core/result.h"
#include "image/colour.h"
#include "image/gpu_image_mac.h"

namespace mv::image {

// CPU Mitchell mip chain, then an immutable MTLTexture (replaceRegion at
// creation time only, matching the D3D11 IMMUTABLE upload rule in CLAUDE.md
// and plan/02). `device` is an id<MTLDevice>, passed as void* so this header
// stays includable from plain .cpp translation units; the implementation
// (upload_mac.mm) is the only place that bridges it back to Objective-C.
// Safe to call from a decode worker: MTLDevice is safe to use from any thread
// (Apple's guarantee), and this call never touches an encoder.
[[nodiscard]] result<gpu_image_mac> upload(void* mtl_device, const display_image& src,
                                           const job_context* ctx = nullptr);

}  // namespace mv::image
