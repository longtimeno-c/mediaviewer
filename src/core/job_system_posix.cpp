// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// POSIX half of the job pool (core/job_system_platform.h): pthread name and,
// on Apple, the QoS class. The pool itself is core/job_system.cpp; the
// autorelease pool around each job is job_scope in the header.
#include "core/job_system_platform.h"

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace mv::detail {

worker_thread::worker_thread() noexcept {
#if defined(__APPLE__)
  pthread_setname_np("mv.worker");
  // UTILITY between jobs and for background sweeps (thumbnails, relists, date
  // scans); USER_INITIATED while a view-tied job runs (enter_lane).
  pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
  qos_ = static_cast<std::uint32_t>(QOS_CLASS_UTILITY);
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
