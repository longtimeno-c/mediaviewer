// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 12 — the host side of metadata writes, shared by both hosts (plan/06
// "Writing", plan/16 "Rate").
//
// The keys `0`–`5` and the pane's comment field are UI-thread events; the write
// is a whole-file rewrite on the I/O pool (rule 1). This is the small state
// machine between them, pure and UI-thread only, like edit_session:
//
//   * requests for the same file coalesce (pressing 3 then 4 writes 4 once),
//     and a comment and a rating for one file share a write;
//   * one write at a time overall, oldest file first, so two writes never race
//     on one file and the pool is never flooded by a held key;
//   * what the user asked for is visible at once (`pending_rating`), before
//     the file catches up, so the pane and the toast never lag the keystroke.
//
// The host owns the debounce timer, the job submission and the marshalling of
// the completion back to the UI thread (plan/14: no dispatcher in here).
#pragma once

#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "meta/write.h"

namespace mv::shell {

struct meta_job {
  std::string path;
  meta::write_fields fields;
  bool revert = false;  // put the fields back to the session's first snapshot
};

struct meta_outcome {
  std::string path;
  bool revert = false;
  bool ok = false;
  status error = status::ok;
  // Where it landed. A JPEG write that Exiv2 declined is a sidecar write.
  meta::write_target target = meta::write_target::in_file;
  bool sidecar_touched = false;
  std::string sidecar_path;  // where the sidecar is (or would be), for the host's message
};

class meta_writer {
 public:
  // Asks for `f` on `path`. Fields already pending for the path are kept unless
  // `f` names them again. Empty `f` is ignored.
  void submit(std::string_view path, const meta::write_fields& f);
  void submit_revert(std::string_view path);

  // The next job to run, or nullopt while one is in flight or nothing is
  // pending. Call again from the completion.
  [[nodiscard]] std::optional<meta_job> take_next();
  // The job returned by take_next() finished. A failed job is dropped, not
  // retried: a file that will not take a write will not take it again.
  void finished(const meta_outcome& outcome);

  [[nodiscard]] bool in_flight() const noexcept { return in_flight_.has_value(); }
  [[nodiscard]] bool has_pending() const noexcept { return !pending_.empty(); }
  // True while a write for `path` is queued or running: a lossless rotation of
  // the same JPEG waits for it, and it for a rotation (host rule).
  [[nodiscard]] bool busy_for(std::string_view path) const noexcept;

  // The rating / comment most recently asked for on `path` and not yet on
  // disk. Empty when nothing is pending. `rating` follows write_fields:
  // 0 means "cleared".
  [[nodiscard]] std::optional<int> pending_rating(std::string_view path) const;
  [[nodiscard]] std::optional<std::string> pending_comment(std::string_view path) const;

  // The last failure, once (for a beep and a toast).
  [[nodiscard]] std::optional<meta_outcome> take_failure();

  // Exit: everything not known to have landed, in the order it must run, and
  // the queue left empty. The in-flight job comes first: the pool drops a job
  // that has not started when it shuts down, and a write sets absolute values,
  // so running one that did land again changes nothing. The host takes this on
  // its UI thread, joins the pool, then runs the jobs (run_meta_job) in order.
  [[nodiscard]] std::vector<meta_job> drain_for_exit();

 private:
  struct entry {
    std::string path;
    meta::write_fields fields;
    bool revert = false;
  };
  entry* find(std::string_view path);
  const entry* find(std::string_view path) const;

  std::deque<entry> pending_;
  std::optional<meta_job> in_flight_;
  std::optional<meta_outcome> failure_;
};

// ---- The job the host runs on its I/O pool (worker threads only) -----------

// Runs `job` through meta::write / meta::revert with the per-user snapshot
// store (`snapshot_dir` empty = io::metadata_snapshot_dir()). If the store
// cannot be opened nothing is written: a write with no way back is not made.
[[nodiscard]] meta_outcome run_meta_job(const meta_job& job, std::string_view snapshot_dir = {});

// The rating asked for by a command: 0 clears.
[[nodiscard]] meta::write_fields rating_fields(int stars);
[[nodiscard]] meta::write_fields comment_fields(std::string_view utf8);

}  // namespace mv::shell
