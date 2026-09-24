// SPDX-License-Identifier: GPL-2.0-or-later
// PR 17 decode entrypoint: JPEG/PNG/BMP only (PR 2's format set). Not
// image/pipeline.h's decode_bytes() — that dispatches through codec::decode(),
// which also reaches GIF/WebP/TIFF/HEIC/AVIF/RAW decoders and the Windows
// OS-codec probe that PR 17 does not build on Darwin (plan/15 PR 17 scope).
#pragma once

#include <span>

#include "core/job_system.h"
#include "image/colour.h"

namespace mv::image {

// `raw_thread_limit` 0 uses the on-screen budget. Thumbnail fallback passes 1
// so a folder of RAWs does not fan out a full demosaic per file.
[[nodiscard]] result<display_image> decode_bytes_mac(std::span<const std::uint8_t> bytes,
                                                      const job_context* ctx = nullptr,
                                                      unsigned raw_thread_limit = 0);

}  // namespace mv::image
