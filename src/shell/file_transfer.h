// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The file work behind a drop, Paste files here and New Folder (docs/design/16
// "Drag and drop into folders"). Portable (std::filesystem + io::verified_copy),
// run on a worker by either host, never on the UI thread.
//
// Rules, every one a test in tests/test_file_transfer.cpp:
//   * nothing is ever replaced: each file or folder lands under a free name
//     (plan_transfer: "name (2).ext"), and the final rename refuses a name that
//     appeared meanwhile;
//   * a move inside one volume is a rename; across volumes it is a verified
//     copy (read back and compared) and only then the source is removed. A
//     move whose source will not go takes its copy back, so it is never two;
//   * a copy or move that fails part way removes what it wrote (the name was
//     free, so it was ours) and leaves the source as it was.
//
// transfer_target is the one seam: the folder being written to. local_target()
// is a folder on this computer; a remote folder (docs/plans/desktop-client.md
// §6) supplies its own over the service's add / mkdir / move calls.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/status.h"
#include "shell/file_drop.h"

namespace mv::shell {

class transfer_target {
 public:
  virtual ~transfer_target() = default;
  [[nodiscard]] virtual bool exists(const std::string& path) = 0;
  // A new, empty folder; fails if anything is at `path`.
  [[nodiscard]] virtual mv::status make_directory(const std::string& path) = 0;
  // Copies a local file to `dest` (which must not exist). `verify` reads the
  // written file back and compares it before it is renamed into place.
  [[nodiscard]] virtual mv::status put_file(const std::string& local_src, const std::string& dest,
                                            bool verify, const std::atomic<bool>* cancel,
                                            std::atomic<std::uint64_t>* done_bytes) = 0;
  // A move by rename: `taken` when something is at `dest` (never replaced),
  // `other_volume` when only a copy can get it there.
  enum class rename_outcome : std::uint8_t { done, other_volume, taken, failed };
  [[nodiscard]] virtual rename_outcome rename(const std::string& src, const std::string& dest) = 0;
  // Removes a file, or a folder and everything in it (a failed copy's own).
  [[nodiscard]] virtual mv::status remove(const std::string& path) = 0;
};

[[nodiscard]] std::unique_ptr<transfer_target> local_target();

// Live counters a job's row reads while it runs (any thread).
struct transfer_progress {
  std::atomic<std::uint64_t> total_bytes{0};
  std::atomic<std::uint64_t> done_bytes{0};
  std::atomic<std::uint32_t> total_items{0};
  std::atomic<std::uint32_t> done_items{0};
};

struct transfer_item {
  std::string source;
  std::string dest;
  mv::status status = mv::status::ok;
};

struct transfer_result {
  drop_verb verb = drop_verb::copy;
  std::vector<transfer_item> items;
  bool cancelled = false;
  [[nodiscard]] std::size_t succeeded() const noexcept {
    std::size_t n = 0;
    for (const auto& i : items) n += i.status == mv::status::ok ? 1 : 0;
    return n;
  }
  [[nodiscard]] std::size_t failed() const noexcept { return items.size() - succeeded(); }
};

// [worker] Copies or moves `sources` (local paths, files or folders) into
// `target_dir` through `target`. Sources in the folder already are skipped by a
// move (plan_transfer). Blocks until done or `cancel`.
[[nodiscard]] transfer_result run_transfer(drop_verb verb, const std::vector<std::string>& sources,
                                           const std::string& target_dir, transfer_target& target,
                                           transfer_progress& progress,
                                           const std::atomic<bool>* cancel = nullptr);

// [worker] A new folder in `parent` named `name`, or "name 2", "name 3"… as
// Finder names a second untitled folder. The full path made, or "" on failure.
[[nodiscard]] std::string make_new_folder(const std::string& parent, const std::string& name,
                                          transfer_target& target);

// One line for the notice when a transfer ends ("Copied 3 items to Trip").
[[nodiscard]] std::string transfer_summary(const transfer_result& r, const std::string& target_dir);

// The jobs both hosts list in their Jobs pane beside clip jobs: a row per drop
// or paste, with its progress, cancel and the folder it went to. Ids carry
// kTransferJobBit so they never meet a clip job's. Any thread.
inline constexpr std::uint64_t kTransferJobBit = 1ull << 62;

struct transfer_job_view {
  std::uint64_t id = 0;
  int state = 1;  // mv_clip_job_state's words: 1 waiting, 2 running, 3 done, 4 failed, 5 cancelled
  double fraction = 0;
  std::int64_t elapsed_ms = 0;
  std::int32_t failed = 0;
  std::int32_t done_items = 0;
  std::string title;   // "Copying 12 items" (the row shows the folder beside it)
  std::string target;  // the folder they went to (Reveal opens it)
};

class transfer_board {
 public:
  struct job {
    std::uint64_t id = 0;
    std::string title;
    std::string target;
    transfer_progress progress;
    std::atomic<bool> cancel{false};
    std::atomic<int> state{1};
    std::atomic<std::int32_t> failed{0};
    std::int64_t started_ms = 0;
    std::atomic<std::int64_t> ended_ms{0};
  };

  // A new row; the worker updates it through the pointer, which stays valid
  // until clear_finished() drops a finished row.
  std::shared_ptr<job> add(std::string title, std::string target, std::int64_t now_ms);
  void finish(const std::shared_ptr<job>& j, const transfer_result& r, std::int64_t now_ms);
  [[nodiscard]] std::vector<std::uint64_t> ids() const;  // oldest first
  [[nodiscard]] bool view(std::uint64_t id, std::int64_t now_ms, transfer_job_view& out) const;
  void cancel(std::uint64_t id);
  void clear_finished();
  [[nodiscard]] std::uint64_t generation() const noexcept { return generation_.load(); }
  [[nodiscard]] bool any_running() const;

 private:
  mutable std::mutex m_;
  std::vector<std::shared_ptr<job>> jobs_;
  std::uint64_t next_ = 1;
  std::atomic<std::uint64_t> generation_{0};
};

}  // namespace mv::shell
