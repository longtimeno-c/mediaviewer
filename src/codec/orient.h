// SPDX-License-Identifier: GPL-2.0-or-later
// Display-path orientation (plan/04: "Apply EXIF/container orientation on the
// display path, never as a surprise 90° pixel rotate of the original").
//
// The decoded raster is reordered in memory on the worker that decoded it;
// the file is never touched. HEIC/AVIF get this from libheif, RAW from
// LibRaw's flip; JPEG gets it here (PR 10, closing plan/12 PR 7 row 4 for
// JPEG). TIFF, PNG and WebP orientation stay unapplied, as before.
#pragma once

#include <cstdint>
#include <span>

#include "codec/orientation.h"
#include "codec/raster.h"
#include "core/job_system.h"
#include "core/result.h"

namespace mv::codec {

// Reorders `img` so that it shows through `g` (stored → displayed). False only
// on allocation failure; `img` is unchanged then.
[[nodiscard]] bool apply_orientation(raster& img, d4 g);

// decode_jpeg, then the file's EXIF orientation applied. What the viewer, the
// preview and the thumbnailer show for a JPEG. decode_raw_preview keeps using
// the unrotated decode_jpeg: LibRaw's flip is applied there instead.
[[nodiscard]] result<raster> decode_jpeg_display(std::span<const std::uint8_t> bytes,
                                                 const job_context* ctx = nullptr,
                                                 int scale_denom = 1);

}  // namespace mv::codec
