// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Windows half of the job pool (core/job_system_platform.h): thread
// description and priority. The pool itself is core/job_system.cpp.
#include "core/job_system_platform.h"
#include "core/parallel.h"

#include <windows.h>

namespace mv::detail {

worker_thread::worker_thread() noexcept {
  // Named so ETW traces and the debugger show which thread stalled.
  ::SetThreadDescription(::GetCurrentThread(), L"mv.worker");
  // Below normal: a decode must never outrank the frame it is feeding.
  ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
}

}  // namespace mv::detail

namespace mv::core::detail {

int current_thread_priority() noexcept { return ::GetThreadPriority(::GetCurrentThread()); }

void set_current_thread_priority(int priority) noexcept {
  if (priority == THREAD_PRIORITY_ERROR_RETURN) return;
  (void)::SetThreadPriority(::GetCurrentThread(), priority);
}

}  // namespace mv::core::detail
