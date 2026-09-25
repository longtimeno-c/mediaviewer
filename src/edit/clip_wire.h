// SPDX-License-Identifier: GPL-2.0-or-later
// Clip jobs out of process (plan/08 "Execution & UX": "as a child process for
// encode jobs. A child process means a crash in an encode can't take the
// viewer down, and cancelling is a clean kill"; owner's call, plan/12
// 2026-09-25).
//
// The queue (edit/clip_jobs) starts tools/clipjob (MediaViewerClipJob) for
// every job that decodes or encodes, and speaks this line protocol with it:
//
//   queue -> helper, stdin:  one line of JSON, the request; then "cancel",
//                            or end of stream, to cancel
//   helper -> queue, stdout: "progress <0..1>", "encoder <name>",
//                            "output <JSON string>" (one per file),
//                            "written <in_ns> <out_ns>", and last "done" or
//                            "error <mv::status>"
//
// A helper that dies, crashes or hangs past a cancel fails its job; the queue
// removes the `.mvpart` temporaries it left, so nothing partial survives.
// Stream-copy jobs (Path 1, rotate, split, remove-middle, remux, audio copy)
// stay in process: they open no codec.
#pragma once

#include <string>
#include <string_view>

#include "core/status.h"
#include "edit/clip.h"

namespace mv::edit::clip::wire {

// True for the requests that open a decoder or an encoder.
[[nodiscard]] bool runs_in_helper(const request& req) noexcept;

// One line of JSON, and back. decode is strict: an unknown op, a bad type or
// a missing source is false.
[[nodiscard]] std::string encode_request(const request& req);
[[nodiscard]] bool decode_request(std::string_view line, request& out);

[[nodiscard]] std::string progress_line(double fraction);
[[nodiscard]] std::string output_line(std::string_view utf8_path);
[[nodiscard]] std::string written_line(const range& r);
[[nodiscard]] std::string error_line(status s);

// What the helper has said so far. `progress` lines go straight to `ctl`.
struct reply {
  bool done = false;
  bool failed = false;
  status error = status::internal;
  outcome result;
};
void apply_line(std::string_view line, reply& into, const control& ctl);

}  // namespace mv::edit::clip::wire
