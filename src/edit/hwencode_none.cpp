// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The headless core build (cmake/portable, Linux CI): no hardware encoder, so
// Path 2 reports unsupported_format and Path 1 is the only trim. The tests
// drive Path 2 through clip_internal.h with a software encoder instead.
#include "edit/hwencode.h"

namespace mv::edit::hwencode {

std::span<const char* const> candidates(codec) noexcept { return {}; }

}  // namespace mv::edit::hwencode

#include "edit/hwencode_label.inc"
