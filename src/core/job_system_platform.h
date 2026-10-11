// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The per-platform half of the job pool (core/job_system.cpp), kept to the
// three things that differ (issue #170):
//
//   worker_thread     each worker's name, and its priority (Windows,
//                     job_system_win.cpp) or QoS class (Apple,
//                     job_system_posix.cpp), including the per-job QoS switch.
//   job_scope         wraps every job body and completion callback. An
//                     autorelease pool on Apple, nothing on Windows.
//
// No platform header is included here: windows.h stays in *_win.cpp, and the
// pool is the two Objective-C runtime calls clang emits for @autoreleasepool,
// declared below, so the shared job_system.cpp stays plain C++
// (docs/design/15).
#pragma once

#include <cstdint>

#if defined(__APPLE__)
// <objc/objc-internal.h> is not public. These are the functions clang lowers
// @autoreleasepool to; libobjc exports them (cmake/darwin.cmake links it).
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void* pool);
#endif

namespace mv::detail {

// Lives on a pool worker's stack for the worker's whole life.
class worker_thread {
 public:
  // Names the calling thread "mv.worker". Windows: THREAD_PRIORITY_BELOW_NORMAL,
  // so a decode never outranks the frame it feeds. Apple: QOS_CLASS_UTILITY
  // between jobs and for background sweeps.
  worker_thread() noexcept;

  // Before each job body that runs. Apple: USER_INITIATED for a view-tied job,
  // UTILITY for a background one; Apple Silicon steers UTILITY to the
  // efficiency cores, 2-3x slower, and the decode of the photo on screen was
  // running there. Changed only when the class differs from the last job's.
  // Windows: the priority is fixed, nothing to do.
#if defined(__APPLE__)
  void enter_lane(bool background) noexcept;
#else
  void enter_lane(bool /*background*/) noexcept {}
#endif

 private:
#if defined(__APPLE__)
  std::uint32_t qos_ = 0;  // the qos_class_t in force on this thread
#endif
};

// Around every job body and completion callback. Job bodies (and the ABI's
// completions) may call Metal/AppKit/Foundation APIs that autorelease
// temporaries; a worker thread with no pool in place does not crash, it leaks
// every one of them silently.
#if defined(__APPLE__)
class job_scope {
 public:
  job_scope() noexcept : pool_(objc_autoreleasePoolPush()) {}
  ~job_scope() { objc_autoreleasePoolPop(pool_); }
  job_scope(const job_scope&) = delete;
  job_scope& operator=(const job_scope&) = delete;

 private:
  void* pool_;
};
#else
class job_scope {
 public:
  job_scope() noexcept = default;
  job_scope(const job_scope&) = delete;
  job_scope& operator=(const job_scope&) = delete;
};
#endif

}  // namespace mv::detail
