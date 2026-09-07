// SPDX-License-Identifier: GPL-2.0-or-later
// Decode bytes → colour-managed display image. No GPU, no I/O.
#pragma once

#include <span>

#include "core/job_system.h"
#include "image/colour.h"

namespace mv::image {

[[nodiscard]] result<display_image> decode_bytes(std::span<const std::uint8_t> bytes,
                                                 const job_context* ctx = nullptr);

// JPEG DCT 1/4 for first pixel. Unsupported for other formats or tiny files
// (output < 16 px on a side) — the caller skips and waits for the full decode.
[[nodiscard]] result<display_image> decode_preview(std::span<const std::uint8_t> bytes,
                                                   const job_context* ctx = nullptr);

}  // namespace mv::image
