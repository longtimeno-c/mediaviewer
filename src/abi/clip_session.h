// SPDX-License-Identifier: GPL-2.0-or-later
// The session's clip state behind mediaviewer_clip.h (PR 13 / 14): the job
// queue and the keyframe-index answers. Portable (no windows.h), so the ABI's
// clip half is tested on every platform; abi.cpp owns one per session and
// forwards the exports here.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/status.h"
#include "edit/clip.h"
#include "edit/clip_jobs.h"
#include "mediaviewer/mediaviewer_clip.h"

namespace mv::abi {

class clip_session {
 public:
  // `push` receives every completion; it is called on worker threads.
  explicit clip_session(std::function<void(const mv_completion&)> push);
  ~clip_session();  // cancels jobs, joins both workers
  clip_session(const clip_session&) = delete;
  clip_session& operator=(const clip_session&) = delete;

  // The MediaViewerClipJob executable; see mv_clip_set_helper.
  void set_helper(std::string helper_utf8) { queue_.set_helper(std::move(helper_utf8)); }

  [[nodiscard]] status request_index(std::string path, std::uint64_t& out_id);
  [[nodiscard]] status index_get(std::uint64_t id, std::int64_t* keyframes, std::uint32_t cap,
                                 std::uint32_t* out_count, std::int64_t* out_duration) const;

  [[nodiscard]] status submit(std::string source, const mv_clip_request& req, std::uint64_t& out_id);
  [[nodiscard]] status cancel(std::uint64_t id) noexcept;
  [[nodiscard]] status retry(std::uint64_t id, std::uint64_t& out_id);
  [[nodiscard]] status jobs(std::uint64_t* ids, std::uint32_t cap, std::uint32_t* out_count) const;
  [[nodiscard]] status progress(std::uint64_t id, mv_clip_progress& out) const;
  [[nodiscard]] status output(std::uint64_t id, std::uint32_t index, char* utf8, std::uint32_t cap,
                              std::uint32_t* out_bytes) const;
  void clear_finished() noexcept;

  // mv_clip_request -> edit::clip::request. False for a bad op or size.
  [[nodiscard]] static bool to_request(const mv_clip_request& in, std::string source,
                                       edit::clip::request& out) noexcept;

 private:
  struct index_answer {
    std::uint64_t id = 0;
    bool ready = false;
    status result = status::ok;
    std::vector<std::int64_t> keyframes;
    std::int64_t duration = 0;
  };

  static void on_job_event(void* user, std::uint64_t id, edit::clip::job_state state) noexcept;
  void index_worker() noexcept;

  std::function<void(const mv_completion&)> push_;
  edit::clip::job_queue queue_;

  mutable std::mutex index_mu_;
  std::condition_variable index_cv_;
  std::deque<std::pair<std::uint64_t, std::string>> index_pending_;
  std::deque<index_answer> index_answers_;  // the last few, oldest first
  std::uint64_t next_index_id_ = 1;
  bool stop_ = false;
  std::thread index_thread_;
};

}  // namespace mv::abi
