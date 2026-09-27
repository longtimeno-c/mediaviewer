// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Running one clip job in MediaViewerClipJob (tools/clipjob; protocol in
// edit/clip_wire.h). Worker thread only: this blocks until the helper exits.
#pragma once

#include <string>

#include "core/result.h"
#include "edit/clip.h"

namespace mv::edit::clip {

// Starts the helper, streams its progress into `ctl`, asks it to cancel when
// `ctl.cancel` is raised and kills it if it has not gone after a grace period.
// status::io when the helper cannot be started; status::internal when it
// died without an answer (a crash in a driver's encoder ends here, not in
// the viewer). Every abnormal end sweeps the output folder's `.mvpart`
// temporaries.
[[nodiscard]] result<outcome> run_in_helper(const std::string& helper_utf8, const request& req,
                                            const control& ctl);

// Removes the staged-output temporaries (`.<name>.mvpart`) in `utf8_dir`.
void sweep_temporaries(const std::string& utf8_dir) noexcept;

}  // namespace mv::edit::clip
