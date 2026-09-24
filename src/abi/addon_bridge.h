// SPDX-License-Identifier: GPL-2.0-or-later
// abi.cpp owns mv_session; addon_abi.cpp posts add-on events through it.
#pragma once

#include <mediaviewer/mediaviewer.h>

namespace mv::abi {
void push_addon_completion(mv_session_t session, const mv_completion& c) noexcept;
}  // namespace mv::abi
