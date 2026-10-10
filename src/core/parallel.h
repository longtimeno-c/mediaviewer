// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Split one large, already-on-a-worker loop (a colour transform, a mip level)
// into bands across a few extra threads. For work that is measured in tens of
// milliseconds on one core; below min_per_thread items it stays serial, so
// thumbnails and small images pay nothing.
//
// The calling thread runs the first band itself. fn must not throw (it is
// called on threads that have no handler) and must only touch its own band.
#pragma once

#include <algorithm>
#include <cstddef>
#include <thread>

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace mv::core {

#if !defined(__APPLE__)
namespace detail {
// Win32 thread priority of the calling thread / set on the calling thread
// (core/job_system_win.cpp), kept out of this header so it pulls no <windows.h>.
[[nodiscard]] int current_thread_priority() noexcept;
void set_current_thread_priority(int priority) noexcept;
}  // namespace detail
#endif

// Extra cores a decode may borrow. Leaves two logical processors for the
// present thread and the UI (the same margin as raw_thread_ceiling), and never
// more than four: prefetch runs beside the selected image and must not starve it.
[[nodiscard]] inline unsigned band_threads() noexcept {
  const unsigned hw = std::thread::hardware_concurrency();
  if (hw <= 3) return 1;
  return std::min(hw - 2, 4u);
}

template <class Fn>
void parallel_bands(std::size_t count, std::size_t min_per_thread, Fn&& fn) noexcept {
  if (count == 0) return;
  std::size_t threads = band_threads();
  if (min_per_thread > 0) threads = std::min(threads, std::max<std::size_t>(1, count / min_per_thread));
  if (threads <= 1) {
    fn(std::size_t{0}, count);
    return;
  }
  const std::size_t step = (count + threads - 1) / threads;
  std::thread extra[4];
  std::size_t started = 0;
  for (std::size_t t = 1; t < threads; ++t) {
    const std::size_t begin = t * step;
    if (begin >= count) break;
    const std::size_t end = std::min(count, begin + step);
    try {
#if defined(__APPLE__)
      // A band runs at its caller's QoS, not the default: the job system
      // raises a view-tied decode to USER_INITIATED.
      const qos_class_t qos = qos_class_self();
      extra[started] = std::thread([&fn, begin, end, qos] {
        pthread_set_qos_class_self_np(qos, 0);
        fn(begin, end);
      });
#else
      // Likewise the caller's priority: pool workers run below normal, and a
      // band started at the default NORMAL would outrank the UI thread.
      const int priority = detail::current_thread_priority();
      extra[started] = std::thread([&fn, begin, end, priority] {
        detail::set_current_thread_priority(priority);
        fn(begin, end);
      });
#endif
      ++started;
    } catch (...) {
      fn(begin, end);  // no thread to be had: do the band here
    }
  }
  fn(std::size_t{0}, std::min(count, step));
  for (std::size_t t = 0; t < started; ++t) extra[t].join();
}

}  // namespace mv::core
