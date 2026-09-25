// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// mv_crash_context — the core's crash-context table, handed to the host.
// Separate TU so abi.cpp (shared by several PR 7 slices) is not touched.
#include "mediaviewer/mediaviewer_crash.h"

#include "abi/guard.h"
#include "core/crash_context.h"

extern "C" {

mv_status MV_CALL mv_crash_context(mv_crash_context_info* out_info) {
  return static_cast<mv_status>(mv::abi::guard("mv_crash_context", [&]() -> mv::status {
    MV_REQUIRE(out_info != nullptr, "out_info must not be null");
    const auto slots = mv::crash_context::slots();
    out_info->slots = slots.data();
    out_info->slot_count = static_cast<uint32_t>(mv::crash_context::kSlotCount);
    out_info->slot_bytes = static_cast<uint32_t>(mv::crash_context::kSlotBytes);
    out_info->last_call_correlation_id = mv::crash_context::last_call_address();
    return mv::status::ok;
  }));
}

}  // extern "C"
