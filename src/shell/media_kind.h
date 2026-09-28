// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Is this listing entry a clip (played through player/) rather than a still
// (decoded through image/)? By extension only: the folder listing filters by
// extension too, and the player probes the real container when it opens it.
// The list lives in io/pairing.h so the core's folder flags (mv_folder_item
// bit 2) and the hosts agree.
#pragma once

#include <string_view>

#include "io/pairing.h"

namespace mv::shell {

[[nodiscard]] inline bool is_video_name(std::string_view name) noexcept {
  return mv::io::is_video_name(name);
}

}  // namespace mv::shell
