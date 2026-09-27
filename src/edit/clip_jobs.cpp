// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit/clip_jobs.h"

#include <algorithm>
#include <chrono>

#include "edit/clip_helper.h"
#include "edit/clip_wire.h"
#include "edit/hwencode.h"
#include "io/file_port.h"

namespace mv::edit::clip {

struct job_queue::job {
  std::uint64_t id = 0;
  request req;
  job_state state = job_state::queued;
  std::atomic<bool> cancel{false};
  std::atomic<double> fraction{0.0};
  std::chrono::steady_clock::time_point started{};
  std::int64_t elapsed_ms = 0;  // final, once finished
  status error = status::ok;
  outcome result;
};

namespace {

void on_progress(void* user, double f) noexcept {
  auto* a = static_cast<std::atomic<double>*>(user);
  a->store(std::clamp(f, 0.0, 1.0), std::memory_order_relaxed);
}

[[nodiscard]] bool finished(job_state s) noexcept {
  return s == job_state::done || s == job_state::failed || s == job_state::cancelled;
}

}  // namespace

job_queue::job_queue() : thread_([this] { worker(); }) {}

job_queue::~job_queue() {
  {
    std::lock_guard lock(mu_);
    stop_ = true;
    for (auto& j : jobs_) j->cancel.store(true, std::memory_order_relaxed);
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void job_queue::set_helper(std::string helper_utf8) {
  std::lock_guard lock(mu_);
  helper_ = std::move(helper_utf8);
}

void job_queue::set_listener(job_listener fn, void* user) noexcept {
  std::lock_guard lock(mu_);
  listener_ = fn;
  listener_user_ = user;
}

void job_queue::notify(std::uint64_t id, job_state s) noexcept {
  job_listener fn = nullptr;
  void* user = nullptr;
  {
    std::lock_guard lock(mu_);
    fn = listener_;
    user = listener_user_;
  }
  if (fn != nullptr) fn(user, id, s);
}

std::uint64_t job_queue::submit(request req) {
  std::uint64_t id = 0;
  {
    std::lock_guard lock(mu_);
    auto j = std::make_unique<job>();
    id = j->id = next_id_++;
    j->req = std::move(req);
    jobs_.push_back(std::move(j));
    active_.fetch_add(1, std::memory_order_relaxed);
  }
  cv_.notify_one();
  notify(id, job_state::queued);
  return id;
}

bool job_queue::cancel(std::uint64_t id) noexcept {
  bool was_queued = false;
  {
    std::lock_guard lock(mu_);
    auto it = std::find_if(jobs_.begin(), jobs_.end(), [id](const auto& j) { return j->id == id; });
    if (it == jobs_.end() || finished((*it)->state)) return false;
    (*it)->cancel.store(true, std::memory_order_relaxed);
    if ((*it)->state == job_state::queued) {
      // Never started: nothing to clean up, so it is cancelled now rather
      // than when the worker reaches it.
      (*it)->state = job_state::cancelled;
      (*it)->error = status::cancelled;
      active_.fetch_sub(1, std::memory_order_relaxed);
      was_queued = true;
    }
  }
  if (was_queued) notify(id, job_state::cancelled);
  return true;
}

std::uint64_t job_queue::retry(std::uint64_t id) {
  request again;
  {
    std::lock_guard lock(mu_);
    auto it = std::find_if(jobs_.begin(), jobs_.end(), [id](const auto& j) { return j->id == id; });
    if (it == jobs_.end()) return 0;
    if ((*it)->state != job_state::failed && (*it)->state != job_state::cancelled) return 0;
    again = (*it)->req;
  }
  return submit(std::move(again));
}

bool job_queue::snapshot(std::uint64_t id, job_snapshot& out) const {
  std::lock_guard lock(mu_);
  auto it = std::find_if(jobs_.begin(), jobs_.end(), [id](const auto& j) { return j->id == id; });
  if (it == jobs_.end()) return false;
  const job& j = **it;
  out.id = j.id;
  out.state = j.state;
  out.kind = j.req.kind;
  out.error = j.error;
  out.source_name = std::string(io::file_name_of(j.req.source));
  out.outputs = j.result.outputs;
  out.encoder = j.result.encoder;
  out.fraction = j.state == job_state::done ? 1.0 : j.fraction.load(std::memory_order_relaxed);
  if (j.state == job_state::running) {
    out.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - j.started)
                         .count();
    // An ETA only once there is enough of a run to extrapolate from.
    out.eta_ms = out.fraction > 0.02 && out.elapsed_ms > 500
                     ? static_cast<std::int64_t>(static_cast<double>(out.elapsed_ms) * (1.0 - out.fraction) / out.fraction)
                     : -1;
  } else {
    out.elapsed_ms = j.elapsed_ms;
    out.eta_ms = j.state == job_state::queued ? -1 : 0;
  }
  return true;
}

std::vector<std::uint64_t> job_queue::ids() const {
  std::lock_guard lock(mu_);
  std::vector<std::uint64_t> out;
  out.reserve(jobs_.size());
  for (const auto& j : jobs_) out.push_back(j->id);
  return out;
}

void job_queue::clear_finished() noexcept {
  std::lock_guard lock(mu_);
  jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [](const auto& j) { return finished(j->state); }),
              jobs_.end());
}

bool job_queue::busy() const noexcept { return active_.load(std::memory_order_relaxed) > 0; }

void job_queue::worker() noexcept {
  while (true) {
    job* next = nullptr;
    {
      std::unique_lock lock(mu_);
      cv_.wait(lock, [this] {
        if (stop_) return true;
        return std::any_of(jobs_.begin(), jobs_.end(), [](const auto& j) { return j->state == job_state::queued; });
      });
      if (stop_) return;
      for (auto& j : jobs_) {
        if (j->state == job_state::queued) {
          next = j.get();
          break;
        }
      }
      if (next == nullptr) continue;
      next->state = job_state::running;
      next->started = std::chrono::steady_clock::now();
    }
    // Jobs are only erased once finished (clear_finished), so `next` stays
    // valid while it runs without the lock.
    notify(next->id, job_state::running);
    control ctl;
    ctl.cancel = &next->cancel;
    ctl.progress = on_progress;
    ctl.user = &next->fraction;
    std::string helper;
    {
      std::lock_guard lock(mu_);
      helper = helper_;
    }
    auto r = !helper.empty() && wire::runs_in_helper(next->req) ? run_in_helper(helper, next->req, ctl)
                                                                 : run(next->req, ctl);
    job_state final_state = job_state::done;
    const std::uint64_t id = next->id;  // `next` may be cleared once finished
    {
      std::lock_guard lock(mu_);
      next->elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - next->started)
                             .count();
      if (r) {
        next->result = std::move(*r);
        next->state = job_state::done;
      } else {
        next->error = r.error();
        next->state = r.error() == status::cancelled || next->cancel.load() ? job_state::cancelled : job_state::failed;
      }
      final_state = next->state;
      active_.fetch_sub(1, std::memory_order_relaxed);
    }
    notify(id, final_state);
  }
}

std::string job_title(op kind, const std::string& encoder) {
  switch (kind) {
    case op::trim_keyframe: return "Trim (keyframe)";
    case op::trim_reencode:
      return encoder.empty() ? std::string("Trim (re-encode, slower)")
                             : "Trim (re-encode, " + std::string(hwencode::family_label(encoder.c_str())) + ")";
    case op::rotate: return "Rotate (lossless)";
    case op::split: return "Split";
    case op::remove_middle: return "Remove in–out";
    case op::remux: return "Convert container";
    case op::frame: return "Save frame";
    case op::audio: return "Extract audio";
    case op::animation: return "GIF / WebP";
  }
  return "Clip job";
}

}  // namespace mv::edit::clip
