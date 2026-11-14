// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/self-hosted.md "Tray": the server menu's watcher for the Windows
// shell (mediaviewer.h mv_host_tray_start / mv_host_tray_stop). Its own pair,
// not part of Phone access's resume: that moves into an add-on (PR #691).
#include <mediaviewer/mediaviewer.h>

#include "abi/guard.h"
#include "abi/host_tray_glue.h"

extern "C" {

MV_API mv_status MV_CALL mv_host_tray_start(void) {
  return static_cast<mv_status>(mv::abi::guard("mv_host_tray_start", []() -> mv::status {
    mv::abi::start_host_tray();
    return mv::status::ok;
  }));
}

MV_API mv_status MV_CALL mv_host_tray_stop(void) {
  return static_cast<mv_status>(mv::abi::guard("mv_host_tray_stop", []() -> mv::status {
    mv::abi::stop_host_tray();
    return mv::status::ok;
  }));
}

}  // extern "C"
