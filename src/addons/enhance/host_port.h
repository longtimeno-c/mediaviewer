// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance's jobs over the host function table (#297 slice 4): runner.h's
// run_port from the table's sound rows (sound_open ... sound_write_close),
// ONNX Runtime from the add-on's own folder, and the OS's free memory.
#pragma once

#include <mediaviewer/mediaviewer_addon.h>

#include <string>

#include "addons/enhance/runner.h"

namespace mv::enhance {

// The sound rows are appended after busy_post: an older host's table stops
// before them, and Enhance then installs models but runs none.
[[nodiscard]] bool has_sound_rows(const mv_host_api* h);

// The picture rows (slice 5), appended after the sound rows.
[[nodiscard]] bool has_picture_rows(const mv_host_api* h);

// `self_dir` holds ONNX Runtime (beside the add-on's library). The table
// outlives the port.
[[nodiscard]] run_port run_port_of(const mv_host_api* h, const std::string& self_dir);

}  // namespace mv::enhance
