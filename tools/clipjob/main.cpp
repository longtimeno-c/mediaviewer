// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// MediaViewerClipJob: runs one clip job for the viewer's queue, out of
// process (docs/design/08 "Execution & UX"; protocol in src/edit/clip_wire.h).
//
//   MediaViewerClipJob --job     request on stdin, progress and verdict on stdout
//
// The phone's conversion (docs/plans/phone-clip-convert.md, edit/clip_convert.h),
// started by the `driftwire` server, one call per process, at background priority:
//   --convert-check [OUT_JSON]               {"encoder","family"} into OUT_JSON (or on stdout); exit 3: none
//   --convert-plan SRC SEGMENT_MS OUT_JSON   the plan's JSON into OUT_JSON
//   --convert-segment SRC K SEGMENT_MS OUT   segment K's MPEG-TS into OUT
// Exit 0 done, 1 failed, 2 not a clip (or unreadable), 3 no hardware encoder.
//
// Everything that opens a decoder or a hardware encoder happens here, so a
// crash in a GPU driver's encoder or a decoder bug ends this process and one
// job, never the viewer. Nothing here logs a path (rule 6): stdout carries the
// output paths back to the queue and nothing else is written.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
// After windows.h: CommandLineToArgvW.
#include <shellapi.h>
#else
#include <sys/resource.h>
#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif
#endif

#include "edit/clip.h"
#include "edit/clip_convert.h"
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

// ---- the phone's conversion -----------------------------------------------------------

// Below the viewer and everything else the person is doing: the server's work.
void lower_priority() {
#if defined(_WIN32)
  (void)::SetPriorityClass(::GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#else
  (void)::setpriority(PRIO_PROCESS, 0, 10);
#if defined(__APPLE__)
  (void)::pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
#endif
}

// The arguments as UTF-8 (Windows hands main() the ANSI code page).
std::vector<std::string> utf8_args(int argc, char** argv) {
  std::vector<std::string> out;
#if defined(_WIN32)
  (void)argv;
  int n = 0;
  LPWSTR* wide = ::CommandLineToArgvW(::GetCommandLineW(), &n);
  for (int i = 0; wide != nullptr && i < n; ++i) {
    const int len = ::WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
    std::string a(len > 0 ? static_cast<std::size_t>(len - 1) : 0, '\0');
    if (len > 1) ::WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, a.data(), len, nullptr, nullptr);
    out.push_back(std::move(a));
  }
  if (wide != nullptr) ::LocalFree(wide);
#else
  for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
#endif
  return out;
}

bool write_file(const std::string& path, const std::string& text) {
  std::ofstream f(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size())),
                  std::ios::binary | std::ios::trunc);
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
  return f.good();
}

bool parse_u32(const std::string& s, std::uint32_t& out) {
  if (s.empty() || s.size() > 9) return false;
  std::uint32_t v = 0;
  for (const char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<std::uint32_t>(c - '0');
  }
  out = v;
  return true;
}

int convert_exit(mv::status e) {
  if (e == mv::status::not_found) return 3;
  if (e == mv::status::unsupported_format || e == mv::status::io || e == mv::status::corrupt) return 2;
  return 1;
}

int run_convert(const std::vector<std::string>& a) {
  namespace cv = clip::convert;
  lower_priority();
  cv::quiet_logs();
  const std::string& mode = a[1];
  if (mode == "--convert-check" && (a.size() == 2 || a.size() == 3)) {
    auto e = cv::check();
    if (!e) return 3;
    const std::string text = "{\"encoder\":\"" + e->name + "\",\"family\":\"" + e->family + "\"}";
    if (a.size() == 3) return write_file(a[2], text) ? 0 : 1;
    say(text);
    return 0;
  }
  std::uint32_t seg_ms = 0, k = 0;
  if (mode == "--convert-plan" && a.size() == 5 && parse_u32(a[3], seg_ms) && seg_ms > 0) {
    // A plan nobody could play is no plan: the encoder is asked first.
    if (!cv::check()) return 3;
    auto p = cv::plan(a[2], seg_ms);
    if (!p) return convert_exit(p.error());
    return write_file(a[4], cv::plan_json(*p)) ? 0 : 1;
  }
  if (mode == "--convert-segment" && a.size() == 6 && parse_u32(a[3], k) && parse_u32(a[4], seg_ms) && seg_ms > 0) {
    auto r = cv::segment(a[2], k, seg_ms, a[5], nullptr);
    return r ? 0 : convert_exit(r.error());
  }
  std::fputs("usage: MediaViewerClipJob --convert-check [OUT_JSON] | --convert-plan SRC SEGMENT_MS OUT_JSON | "
             "--convert-segment SRC K SEGMENT_MS OUT_TS\n",
             stderr);
  return 1;
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
  if (argc >= 2 && std::strncmp(argv[1], "--convert-", 10) == 0) return run_convert(utf8_args(argc, argv));
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
