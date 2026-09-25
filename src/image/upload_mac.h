// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

#include "core/job_system.h"
#include "core/result.h"
#include "image/colour.h"
#include "image/gpu_image_mac.h"
#include "image/linear.h"

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

// PR 11: the FP16 working image (image/linear.h, D6) as an immutable
// MTLPixelFormatRGBA16Float texture, one level — the twin of present_lab.cpp's
// R16G16B16A16_FLOAT upload. The blit samples linear light from it directly.
[[nodiscard]] result<gpu_image_mac> upload_linear(void* mtl_device, const linear_image& src);

}  // namespace mv::image
