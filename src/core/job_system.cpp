// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The job pool, one file for both hosts (issue #170). What differs per
// platform is behind core/job_system_platform.h: the worker's name and
// priority / QoS (job_system_win.cpp, job_system_posix.cpp) and the scope
// around each job body and completion (an autorelease pool on Apple).
#include "core/job_system.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#include "core/job_system_platform.h"
#include "core/trace.h"

namespace mv {

namespace {
struct job_record {
  job_id id;
  generation gen;
  job_fn fn;
  job_done_fn on_done;
};

void notify_done(const job_record& job, status result) noexcept {
  if (!job.on_done) return;
  // on_done may touch Metal/AppKit/Foundation APIs too. One scope here covers
  // all three call sites: post-run, pre-run cancellation and the shutdown drain.
  [[maybe_unused]] const detail::job_scope scope;
  try {
    job.on_done(job.id, job.gen, result);
  } catch (...) {
    // A callback may already have side effects; never invoke it a second time.
    MV_LOG_ERROR("job_system: completion callback threw for job %llu",
                 static_cast<unsigned long long>(job.id));
  }
}
}  // namespace

struct job_system::impl {
  std::mutex mutex;
  std::condition_variable cv;
  // Two queues, not one. A view-tied job (the image the user is looking at)
  // must never queue behind the thumbnail sweep of a 2000-file camera dump:
  // FIFO across both meant the first arrow press after opening a folder waited
  // for every thumb job ahead of it (docs/design/02, "nothing blocks the view").
  std::deque<job_record> foreground;
  std::deque<job_record> background;
  std::vector<std::thread> workers;
  bool running = false;
  // Issue #160: the lanes only ordered dequeues, so once every worker had
  // started a thumbnail or a scan (generation 0, never cancelled mid-run) the
  // photo the user just arrowed to waited for one of them to finish. At most
  // `background_cap` workers run background jobs at once; with more than one
  // worker that leaves one free for the foreground lane.
  std::uint32_t background_running = 0;
  std::uint32_t background_cap = 1;

  [[nodiscard]] bool empty() const noexcept {
    return foreground.empty() && background.empty();
  }

  // What a worker may take now: any foreground job, or a background one while
  // the background lane is under its cap.
  [[nodiscard]] bool runnable() const noexcept {
    return !foreground.empty() || (!background.empty() && background_running < background_cap);
  }

  job_record pop() noexcept {
    if (!foreground.empty()) {
      job_record job = std::move(foreground.front());
      foreground.pop_front();
      return job;
    }
    ++background_running;
    job_record job = std::move(background.front());
    background.pop_front();
    return job;
  }

  // A background job finished and its slot is free. One waiter is woken when
  // background work is queued: the worker that ran this job may take a
  // foreground job next, and a worker parked at the cap would otherwise
  // sleep with the slot open.
  void background_done() noexcept {
    bool wake = false;
    {
      std::lock_guard lock(mutex);
      --background_running;
      wake = !background.empty();
    }
    if (wake) cv.notify_one();
  }
};

job_system::job_system() noexcept = default;

job_system::~job_system() { shutdown(); }

status job_system::start(std::uint32_t worker_count) noexcept {
  if (impl_) return status::internal;  // already started

  impl_ = std::make_unique<impl>();
  if (!impl_) return status::out_of_memory;

  if (worker_count == 0) {
    const unsigned hw = std::thread::hardware_concurrency();
    // The UI thread and the render thread are not the pool's to spend.
    worker_count = hw > 3 ? hw - 2 : 1;
  }
  worker_count_ = worker_count;

  impl_->running = true;
  impl_->background_cap = worker_count > 1 ? worker_count - 1 : 1;
  impl_->workers.reserve(worker_count);

  for (std::uint32_t i = 0; i < worker_count; ++i) {
    impl_->workers.emplace_back([this, i] {
      // Named, so traces and the debugger show which thread stalled, and run
      // below the frame it feeds (priority on Windows, QoS on Apple).
      detail::worker_thread thread;

      for (;;) {
        job_record job;
        {
          std::unique_lock lock(impl_->mutex);
          impl_->cv.wait(lock, [this] { return !impl_->running || impl_->runnable(); });
          if (!impl_->running && impl_->empty()) return;
          job = impl_->pop();
        }
        const bool is_background = job.gen == background_generation;

        // Cancellation check before doing any work: navigating away while a
        // hundred decodes are queued must cost approximately nothing.
        const generation now = generation_.load(std::memory_order_relaxed);
        if (job.gen != background_generation && job.gen != now) {
          trace::job_cancelled(job.id, job.gen);
          cancelled_.fetch_add(1, std::memory_order_relaxed);
          notify_done(job, status::cancelled);
          continue;
        }

        thread.enter_lane(job.gen == background_generation);
        trace::job_begin(job.id, i);
        const job_context ctx(job.id, job.gen, &generation_, i);
        status result = status::invalid_arg;
        {
          [[maybe_unused]] const detail::job_scope scope;
          try {
            if (job.fn) result = job.fn(ctx);
          } catch (const std::bad_alloc&) {
            result = status::out_of_memory;
          } catch (...) {
            result = status::internal;
          }
        }
        trace::job_end(job.id, static_cast<std::int32_t>(result));
        if (is_background) impl_->background_done();

        if (result == status::cancelled) {
          cancelled_.fetch_add(1, std::memory_order_relaxed);
        } else {
          completed_.fetch_add(1, std::memory_order_relaxed);
        }
        notify_done(job, result);
      }
    });
  }

  MV_LOG_INFO("job_system: %u workers", worker_count);
  return status::ok;
}

void job_system::shutdown() noexcept {
  if (!impl_) return;

  std::deque<job_record> abandoned;
  {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->running) return;
    impl_->running = false;
    abandoned.swap(impl_->foreground);
    for (auto& job : impl_->background) abandoned.push_back(std::move(job));
    impl_->background.clear();
  }
  impl_->cv.notify_all();

  for (auto& t : impl_->workers) {
    if (t.joinable()) t.join();
  }
  impl_->workers.clear();

  // Report the never-started jobs so nobody is left waiting on a completion
  // that will not arrive.
  for (auto& job : abandoned) {
    cancelled_.fetch_add(1, std::memory_order_relaxed);
    notify_done(job, status::cancelled);
  }

  impl_.reset();
}

job_id job_system::submit(job_fn fn, job_done_fn on_done) noexcept {
  return submit_at(generation_.load(std::memory_order_relaxed), std::move(fn), std::move(on_done));
}

job_id job_system::submit_at(generation gen, job_fn fn, job_done_fn on_done) noexcept {
  if (!impl_ || !fn) return invalid_job;

  const job_id id = next_id_.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->running) return invalid_job;
    auto& q = gen == background_generation ? impl_->background : impl_->foreground;
    q.push_back(job_record{id, gen, std::move(fn), std::move(on_done)});
  }
  submitted_.fetch_add(1, std::memory_order_relaxed);
  trace::job_submit(id, gen);
  impl_->cv.notify_one();
  return id;
}

generation job_system::bump_generation() noexcept {
  return generation_.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::size_t job_system::queue_depth() const noexcept {
  if (!impl_) return 0;
  std::lock_guard lock(impl_->mutex);
  return impl_->foreground.size() + impl_->background.size();
}

}  // namespace mv
