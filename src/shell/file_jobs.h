// SPDX-License-Identifier: GPL-2.0-or-later
// Copy / move / Recycle Bin jobs for marks (plan/16 "Marks, copy, move").
//
// The UI thread may open a picker and ask for confirmation; it never copies a
// byte (plan/16 speed rule 2). Work runs on one I/O worker of its own, at the
// background generation so navigating away does not abandon a copy, and the
// result comes back as one posted window message the host pumps — the core is
// not asked to marshal anything (plan/14). The file operations themselves are
// io/file_ops (portable header, Windows impl); this orchestration is host-side.
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/job_system.h"
#include "core/status.h"

namespace mv::shell {

enum class file_job_kind : std::uint8_t { copy, move, recycle };

struct file_job_item {
  std::string path;          // source, UTF-8
  mv::status status = mv::status::ok;
  bool refused = false;      // recycle only: no Recycle Bin there, nothing deleted
  std::string dest;          // copy / move: where it landed
};

struct file_job_result {
  file_job_kind kind = file_job_kind::copy;
  // Whatever the caller wants back to tell a stale completion from a current
  // one (the folder it was started in).
  std::uint64_t token = 0;
  std::vector<file_job_item> items;

  [[nodiscard]] std::size_t succeeded() const noexcept {
    std::size_t n = 0;
    for (const auto& it : items) n += it.status == mv::status::ok && !it.refused ? 1 : 0;
    return n;
  }
  [[nodiscard]] std::size_t refused() const noexcept {
    std::size_t n = 0;
    for (const auto& it : items) n += it.refused ? 1 : 0;
    return n;
  }
  [[nodiscard]] std::size_t failed() const noexcept {
    std::size_t n = 0;
    for (const auto& it : items) n += it.status != mv::status::ok ? 1 : 0;
    return n;
  }
};

// Posted to the window passed to submit(). lParam is a file_job_result* the
// handler takes ownership of (wrap it in a unique_ptr first thing).
inline constexpr UINT kFileJobDoneMessage = WM_APP + 0x61;

class file_jobs {
 public:
  file_jobs() = default;
  ~file_jobs();

  file_jobs(const file_jobs&) = delete;
  file_jobs& operator=(const file_jobs&) = delete;

  [[nodiscard]] bool start() noexcept;
  // Joins the worker. Jobs not yet started are dropped (process exit).
  void stop() noexcept;

  // [ui-thread] Queues one job over `paths`, in order. `dest_dir` is used by
  // copy and move. False if nothing was queued.
  [[nodiscard]] bool submit(HWND notify, file_job_kind kind, std::vector<std::string> paths,
                            std::string dest_dir, std::uint64_t token) noexcept;

 private:
  mv::job_system pool_;
  bool started_ = false;
};

}  // namespace mv::shell
