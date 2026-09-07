// SPDX-License-Identifier: GPL-2.0-or-later
// The MPMC job queue and its worker pool.
//
// plan/02-architecture.md: one MPMC job queue, N = cores-2 workers, and
// "Every job carries a generation counter tied to the current view intent.
// Navigating away bumps the generation; in-flight decodes check it at tile
// boundaries and abandon."
//
// The generation counter exists in PR 1, before there is anything to cancel,
// because retrofitting cancellation into a decode path that never expected it
// is how fast arrow-key scrubbing ends up queueing 200 dead decodes.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include "core/status.h"

namespace mv {

using job_id = std::uint64_t;
using generation = std::uint32_t;

inline constexpr job_id invalid_job = 0;

// Handed to every running job. The job checks `cancelled()` at whatever
// granularity it can abandon at — a tile boundary, a scanline block, a packet.
class job_context {
 public:
  job_context(job_id id, generation gen, const std::atomic<generation>* current,
              std::uint32_t worker) noexcept
      : id_(id), gen_(gen), current_(current), worker_(worker) {}

  [[nodiscard]] job_id id() const noexcept { return id_; }
  [[nodiscard]] generation gen() const noexcept { return gen_; }
  [[nodiscard]] std::uint32_t worker_index() const noexcept { return worker_; }

  // True once the view intent this job was submitted for has moved on.
  [[nodiscard]] bool cancelled() const noexcept {
    return current_->load(std::memory_order_relaxed) != gen_;
  }

 private:
  job_id id_;
  generation gen_;
  const std::atomic<generation>* current_;
  std::uint32_t worker_;
};

// A job body. Runs on a pool thread; must never touch the D3D11 immediate
// context and must never block on the UI or render thread.
// Exceptions become out_of_memory (bad_alloc) or internal completions.
using job_fn = std::function<status(const job_context&)>;

// Called on the pool thread when a job finishes, including when it was
// cancelled before it ran. This is where the ABI layer pushes a completion
// record — it does NOT marshal to a dispatcher (plan/14-abi.md).
// Must not throw. Violations are logged and contained; callbacks are not retried.
using job_done_fn = std::function<void(job_id, generation, status)>;

class job_system {
 public:
  // Both are defined out of line: `impl` is incomplete here, so an inline
  // constructor would need unique_ptr<impl>'s deleter at every call site.
  job_system() noexcept;
  ~job_system();

  job_system(const job_system&) = delete;
  job_system& operator=(const job_system&) = delete;

  // worker_count == 0 selects max(1, hardware_concurrency - 2): the UI thread
  // and the render thread are not the pool's to spend.
  [[nodiscard]] status start(std::uint32_t worker_count = 0) noexcept;

  // Drains the queue of not-yet-started jobs (reporting them cancelled), then
  // joins. Safe to call twice.
  void shutdown() noexcept;

  [[nodiscard]] std::uint32_t worker_count() const noexcept { return worker_count_; }

  // [any-thread] Submits at the current generation. Returns invalid_job if the
  // pool is not running.
  job_id submit(job_fn fn, job_done_fn on_done = {}) noexcept;

  // [any-thread] Submits explicitly at `gen`, for a caller that captured the
  // generation before doing preparatory work.
  job_id submit_at(generation gen, job_fn fn, job_done_fn on_done = {}) noexcept;

  // [any-thread, no-block] Bumps the view generation. Everything queued or
  // running at the old generation is abandoned at its next check.
  generation bump_generation() noexcept;

  [[nodiscard]] generation current_generation() const noexcept {
    return generation_.load(std::memory_order_relaxed);
  }

  // Diagnostics for the F3 overlay. Approximate by construction.
  [[nodiscard]] std::uint64_t submitted() const noexcept {
    return submitted_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t completed() const noexcept {
    return completed_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t cancelled() const noexcept {
    return cancelled_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::size_t queue_depth() const noexcept;

 private:
  struct impl;
  std::unique_ptr<impl> impl_;

  std::atomic<generation> generation_{1};
  std::atomic<job_id> next_id_{1};
  std::atomic<std::uint64_t> submitted_{0};
  std::atomic<std::uint64_t> completed_{0};
  std::atomic<std::uint64_t> cancelled_{0};
  std::uint32_t worker_count_{0};
};

}  // namespace mv
