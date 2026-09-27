// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The clip job queue (plan/08 "Execution & UX", plan/10 PR 13): one worker,
// FIFO, every job cancellable, queued or running. The job panel on both hosts
// is a view of this: it polls snapshot() and never waits on a job.
//
// One worker on purpose. A keyframe trim is disk-bound and a re-encode owns
// the GPU's encoder; two at once would each run at half speed and both finish
// later than one after the other. The decode pool is not used, so a long
// export never delays a thumbnail or a photo decode.
//
// Thread rules: submit / cancel / retry / snapshot / ids / clear_finished are
// [any-thread][no-block] — the lock is only ever held for bookkeeping, never
// across a job's I/O. The listener is called on the worker (or the calling
// thread for a job cancelled while queued); the host marshals (plan/14).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/status.h"
#include "edit/clip.h"

namespace mv::edit::clip {

enum class job_state : std::uint8_t { queued = 1, running = 2, done = 3, failed = 4, cancelled = 5 };

struct job_snapshot {
  std::uint64_t id = 0;
  job_state state = job_state::queued;
  op kind = op::trim_keyframe;
  double fraction = 0.0;       // 0..1
  std::int64_t elapsed_ms = 0; // running time so far (or total, once finished)
  std::int64_t eta_ms = -1;    // -1 until there is a measurement
  status error = status::ok;   // failed: why
  std::string source_name;     // file name only, for display (never logged)
  std::vector<std::string> outputs;  // done: the published paths
  std::string encoder;         // trim_reencode: what ran ("h264_nvenc")
};

// "Trim (keyframe)", "Trim (re-encode, NVENC)", "Save frame": what the Jobs
// pane calls a job on both hosts.
[[nodiscard]] std::string job_title(op kind, const std::string& encoder);

using job_listener = void (*)(void* user, std::uint64_t id, job_state state) noexcept;

class job_queue {
 public:
  job_queue();
  ~job_queue();  // cancels everything and joins; nothing partial is left behind
  job_queue(const job_queue&) = delete;
  job_queue& operator=(const job_queue&) = delete;

  // Set once, before the first submit.
  void set_listener(job_listener fn, void* user) noexcept;

  // The MediaViewerClipJob executable (UTF-8). Once set, every job that opens
  // a decoder or an encoder (clip_wire.h runs_in_helper) runs in it, never in
  // this process; a missing helper fails those jobs rather than running them
  // here (plan/12 2026-09-25, owner: "the main app shouldn't be affected").
  // Unset (the core tests), everything runs in process.
  void set_helper(std::string helper_utf8);

  // Returns the job id (never 0).
  std::uint64_t submit(request req);
  // False if the id is unknown or already finished.
  bool cancel(std::uint64_t id) noexcept;
  // A failed or cancelled job again, as a new job. 0 if `id` cannot be retried.
  std::uint64_t retry(std::uint64_t id);
  [[nodiscard]] bool snapshot(std::uint64_t id, job_snapshot& out) const;
  // Oldest first.
  [[nodiscard]] std::vector<std::uint64_t> ids() const;
  // Forgets done / failed / cancelled jobs.
  void clear_finished() noexcept;
  // True while any job is queued or running (the panel's poll timer).
  [[nodiscard]] bool busy() const noexcept;

 private:
  struct job;
  void worker() noexcept;
  void notify(std::uint64_t id, job_state s) noexcept;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::unique_ptr<job>> jobs_;
  std::uint64_t next_id_ = 1;
  bool stop_ = false;
  std::atomic<int> active_{0};
  job_listener listener_ = nullptr;
  void* listener_user_ = nullptr;
  std::string helper_;  // guarded by mu_
  std::thread thread_;
};

}  // namespace mv::edit::clip
