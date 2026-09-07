// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/job_system.h"

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

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
  std::deque<job_record> queue;
  std::vector<std::thread> workers;
  bool running = false;
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
  impl_->workers.reserve(worker_count);

  for (std::uint32_t i = 0; i < worker_count; ++i) {
    impl_->workers.emplace_back([this, i] {
      // Named so ETW traces and the debugger show which thread stalled.
      ::SetThreadDescription(::GetCurrentThread(), L"mv.worker");
      // Below normal: a decode must never outrank the frame it is feeding.
      ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

      for (;;) {
        job_record job;
        {
          std::unique_lock lock(impl_->mutex);
          impl_->cv.wait(lock, [this] { return !impl_->running || !impl_->queue.empty(); });
          if (!impl_->running && impl_->queue.empty()) return;
          job = std::move(impl_->queue.front());
          impl_->queue.pop_front();
        }

        // Cancellation check before doing any work: navigating away while a
        // hundred decodes are queued must cost approximately nothing.
        const generation now = generation_.load(std::memory_order_relaxed);
        if (job.gen != background_generation && job.gen != now) {
          trace::job_cancelled(job.id, job.gen);
          cancelled_.fetch_add(1, std::memory_order_relaxed);
          notify_done(job, status::cancelled);
          continue;
        }

        trace::job_begin(job.id, i);
        const job_context ctx(job.id, job.gen, &generation_, i);
        status result = status::invalid_arg;
        try {
          if (job.fn) result = job.fn(ctx);
        } catch (const std::bad_alloc&) {
          result = status::out_of_memory;
        } catch (...) {
          result = status::internal;
        }
        trace::job_end(job.id, static_cast<std::int32_t>(result));

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
    abandoned.swap(impl_->queue);
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
    impl_->queue.push_back(job_record{id, gen, std::move(fn), std::move(on_done)});
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
  return impl_->queue.size();
}

}  // namespace mv
