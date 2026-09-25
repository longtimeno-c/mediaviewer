// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The geometry ops evaluated at full resolution, for export (plan/07: the
// interactive preview runs at viewport resolution on the GPU — the blit's
// output → source map, `placement::map` — and the full-resolution chain runs
// once, on export).
//
// Worker thread only. Rotate, flip and an unstraightened, unscaled crop are
// exact pixel copies, so an export of those ops is the original's pixels
// rearranged and nothing else. Straighten and resize resample in linear light
// (sRGB transfer; a tagged profile's own curve is close enough for a
// geometric resample and the profile travels with the pixels unchanged).
#pragma once

#include "codec/raster.h"
#include "core/job_system.h"
#include "core/result.h"
#include "edit/edit_stack.h"

namespace mv::edit {

// `src` is the image `p` was placed against (its width/height are the
// `source` passed to place()). Returns a raster of `p.output` with the
// source's format, intent and ICC profile carried over.
[[nodiscard]] result<codec::raster> render(const codec::raster& src, const placement& p,
                                           const job_context* ctx = nullptr);

}  // namespace mv::edit
