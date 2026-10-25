// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// POSIX half of the job pool (core/job_system_platform.h): pthread name and,
// on Apple, the QoS class; on Linux, a nice level below the thread that
// started the pool. The pool itself is core/job_system.cpp; the autorelease
// pool around each job is job_scope in the header.
#include "core/job_system_platform.h"

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cerrno>

#include "core/parallel.h"
#endif

namespace mv::detail {

#if defined(__linux__)
namespace {
// Linux PR 1: the analogue of Windows' THREAD_PRIORITY_BELOW_NORMAL. Nice is
// per thread on Linux (setpriority on a thread id), a new thread inherits its
// creator's, and raising one's own nice needs no privilege, so a worker can
// always step down; it could not step back up without CAP_SYS_NICE, and it
// never needs to.
constexpr int k_worker_nice_step = 5;
}  // namespace
#endif

worker_thread::worker_thread() noexcept {
#if defined(__APPLE__)
  pthread_setname_np("mv.worker");
  // UTILITY between jobs and for background sweeps (thumbnails, relists, date
  // scans); USER_INITIATED while a view-tied job runs (enter_lane).
  pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
  qos_ = static_cast<std::uint32_t>(QOS_CLASS_UTILITY);
#elif defined(__linux__)
  // Named so perf, gdb and /proc show which thread stalled (15 chars at most).
  (void)pthread_setname_np(pthread_self(), "mv.worker");
  // Below the thread that started the pool: a decode must never outrank the
  // frame it is feeding. The render thread stays at the default (docs/design/12,
  // 2026-10-23: no unprivileged real-time class on Linux).
  errno = 0;
  const int base = ::getpriority(PRIO_PROCESS, static_cast<id_t>(::gettid()));
  if (errno == 0) {
    const int nice = base + k_worker_nice_step > 19 ? 19 : base + k_worker_nice_step;
    (void)::setpriority(PRIO_PROCESS, static_cast<id_t>(::gettid()), nice);
  }
#endif
  // Other POSIX hosts (none built): default name and scheduling, as before.
}

#if defined(__APPLE__)
void worker_thread::enter_lane(bool background) noexcept {
  const qos_class_t want = background ? QOS_CLASS_UTILITY : QOS_CLASS_USER_INITIATED;
  if (static_cast<std::uint32_t>(want) != qos_ && pthread_set_qos_class_self_np(want, 0) == 0) {
    qos_ = static_cast<std::uint32_t>(want);
  }
}
#endif

}  // namespace mv::detail

#if defined(__linux__)
namespace mv::core::detail {

// core/parallel.h's band priority on Linux: the negated nice value, so below
// normal reads negative here as THREAD_PRIORITY_BELOW_NORMAL does on Windows.
int current_thread_priority() noexcept {
  errno = 0;
  const int nice = ::getpriority(PRIO_PROCESS, static_cast<id_t>(::gettid()));
  return errno == 0 ? -nice : 0;
}

void set_current_thread_priority(int priority) noexcept {
  // Lowering the nice value (raising priority) fails without privilege; a band
  // only ever copies its caller's, which it inherited at creation anyway.
  (void)::setpriority(PRIO_PROCESS, static_cast<id_t>(::gettid()), -priority);
}

}  // namespace mv::core::detail
#endif
