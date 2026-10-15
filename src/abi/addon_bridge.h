// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// abi.cpp owns mv_session; addon_abi.cpp posts add-on events through it.
#pragma once

#include <mediaviewer/mediaviewer.h>

namespace mv::abi {
void push_addon_completion(mv_session_t session, const mv_completion& c) noexcept;
// mv_present_set_busy's flag: the present loop is panning, playing or loading.
// Background passes (an import, #291's folder tag pass) wait while it is set.
[[nodiscard]] bool present_busy() noexcept;
}  // namespace mv::abi
