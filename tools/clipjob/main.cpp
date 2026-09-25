// SPDX-License-Identifier: GPL-2.0-or-later
// MediaViewerClipJob: runs one clip job for the viewer's queue, out of
// process (plan/08 "Execution & UX"; protocol in src/edit/clip_wire.h).
//
//   MediaViewerClipJob --job     request on stdin, progress and verdict on stdout
//
// Everything that opens a decoder or a hardware encoder happens here, so a
// crash in a GPU driver's encoder or a decoder bug ends this process and one
// job, never the viewer. Nothing here logs a path (rule 6): stdout carries the
// output paths back to the queue and nothing else is written.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#include "edit/clip.h"
#include "edit/clip_wire.h"
#if defined(MV_CLIPJOB_TEST_HOOKS)
#include <chrono>

#include "edit/clip_internal.h"
#endif

namespace clip = mv::edit::clip;

namespace {

std::mutex g_out_mu;
std::atomic<bool> g_cancel{false};
std::atomic<int> g_last_parts{-1};

void say(const std::string& line) {
  std::lock_guard lock(g_out_mu);
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

#if defined(MV_CLIPJOB_TEST_HOOKS)
// Test hooks only; the shipped helper reads no environment switches.
bool env_on(const char* name) {
  const char* v = std::getenv(name);
  return v != nullptr && v[0] == '1';
}
#endif

void on_progress(void*, double f) noexcept {
  const int parts = static_cast<int>(f * 10000.0 + 0.5);
  // A line per 0.2 %: enough for the pane's bar and ETA, not a flood.
  if (parts - g_last_parts.load(std::memory_order_relaxed) < 20 && parts < 10000) return;
  g_last_parts.store(parts, std::memory_order_relaxed);
#if defined(MV_CLIPJOB_TEST_HOOKS)
  // Tests of the queue's recovery: a helper that crashes, or one that hangs
  // and ignores the cancel, mid-job. Compiled into the test build only.
  if (env_on("MV_CLIPJOB_TEST_CRASH")) {
#if defined(_WIN32)
    // The debug CRT's abort() opens a modal "abort() has been called" box and
    // WER may hold the process; either leaves the job "running" on a CI runner.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    std::abort();
  }
  if (env_on("MV_CLIPJOB_TEST_HANG")) {
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
  }
#endif
  try {
    say(clip::wire::progress_line(f));
  } catch (...) {
  }
}

// Reads the request line, then watches stdin: "cancel" or end of stream
// (the viewer went away) cancels.
bool read_line(std::string& out) {
  out.clear();
  int c;
  while ((c = std::fgetc(stdin)) != EOF) {
    if (c == '\n') return true;
    out.push_back(static_cast<char>(c));
  }
  return !out.empty();
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  // A crash must end the process at once for the queue to see it, not wait on
  // a Windows Error Reporting dialog nobody can see.
  ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  if (argc != 2 || std::strcmp(argv[1], "--job") != 0) {
    std::fputs("MediaViewerClipJob is started by MediaViewer.\n", stderr);
    return 2;
  }
  std::string line;
  clip::request req;
  if (!read_line(line) || !clip::wire::decode_request(line, req)) {
    say(clip::wire::error_line(mv::status::invalid_arg));
    return 1;
  }
  std::thread([] {
    std::string cmd;
    while (read_line(cmd)) {
      if (cmd == "cancel") break;
    }
    g_cancel.store(true, std::memory_order_relaxed);
  }).detach();

  clip::control ctl;
  ctl.cancel = &g_cancel;
  ctl.progress = on_progress;
  mv::result<clip::outcome> r = mv::err(mv::status::internal);
#if defined(MV_CLIPJOB_TEST_HOOKS)
  if (env_on("MV_CLIPJOB_TEST_SOFTWARE")) {
    r = clip::detail::run_with_encoders(req, ctl, {"mpeg4"}, true);
  } else {
    r = clip::run(req, ctl);
  }
#else
  r = clip::run(req, ctl);
#endif
  if (!r) {
    say(clip::wire::error_line(r.error()));
    return 1;
  }
  if (!r->encoder.empty()) say("encoder " + r->encoder);
  for (const std::string& p : r->outputs) say(clip::wire::output_line(p));
  say(clip::wire::written_line(r->written));
  say("done");
  std::fflush(stdout);
  // The stdin watcher may still be blocked in a read: leave without joining it.
  std::_Exit(0);
}
