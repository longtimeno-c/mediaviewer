// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Native-only, for tests. Not in the C ABI and not P/Invoked (like native.h).
//
// mv_tests links mv_io statically AND loads mediaviewer_core.dll, which has its
// own copy of mv_io. mv::io::set_*_override called from the test exe sets the
// exe's copy, so the DLL would still write thumbnails and add-ons under the
// user's real %LocalAppData%\MediaViewer. This sets the DLL's copy.
#pragma once

#include "mediaviewer/mediaviewer.h"

namespace mv::abi {

// Each argument: null leaves that folder as it is, "" restores the default,
// anything else is the folder (UTF-8) the core uses from now on.
MV_API void set_user_dirs_for_tests(const char* thumbs_utf8, const char* addons_utf8,
                                    const char* open_addons_utf8) noexcept;

}  // namespace mv::abi
