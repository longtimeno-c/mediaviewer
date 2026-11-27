// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The converter's runner for `driftwire serve` and `host` (docs/plans/phone-clip-convert.md):
// each plan and segment is one run of the app's clip helper (MediaViewerClipJob
// --convert-plan / --convert-segment), which holds FFmpeg and the hardware encoder and
// lowers its own priority. A helper that crashes or hangs ends that one job. Nothing here
// logs a path.
#pragma once

#include <memory>
#include <string>

#include "wire/serve_convert.h"

namespace mv::driftwire::cli {

struct convert_check {
  bool ok = false;
  std::string encoder;  // "h264_videotoolbox"
  std::string family;   // "VideoToolbox": what the card says
};
// Runs `helper --convert-check OUT` (OUT a file in `scratch_dir`). Not ok with no helper there,
// no hardware encoder, or a helper that says nothing in 20 s.
[[nodiscard]] convert_check check_converter(const std::string& helper, const std::string& scratch_dir);

[[nodiscard]] serve::convert::runner helper_runner(const std::string& helper);

// The converter `serve` and `host` give the service: checked, its cache in `cache_dir`.
// nullptr when the helper is not there or found no hardware encoder.
[[nodiscard]] std::shared_ptr<serve::convert::service> start_converter(const std::string& helper,
                                                                       const std::string& cache_dir,
                                                                       convert_check* found = nullptr);

}  // namespace mv::driftwire::cli
