// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Linux frame-time harness (Linux PR 1). Same schema-2 JSON as Windows and the
// Mac; the gate requires drop_source "Wayland presentation feedback", which
// only feedback with VSYNC and HW_CLOCK and a moving vblank counter earns. A
// DXGI or Metal report cannot pass, and neither can headless Weston's.

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "frametime/report.h"

namespace {

std::optional<std::string> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return in.bad() ? std::nullopt : std::optional{content};
}

std::string directory_of(const std::string& path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos ? "." : path.substr(0, slash);
}

std::string executable_directory() {
  char buf[4096]{};
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return ".";
  buf[n] = '\0';
  return directory_of(buf);
}

void sleep_ms(long ms) {
  timespec ts{ms / 1000, (ms % 1000) * 1'000'000L};
  while (::nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
}

// The lab's exit code, 2 when it died on a signal, -1 when it could not be
// run or timed out (soak + 30 s, then SIGKILL).
int run(const std::vector<std::string>& args, double seconds) {
  const pid_t pid = ::fork();
  if (pid < 0) {
    std::fprintf(stderr, "frametime: fork failed\n");
    return -1;
  }
  if (pid == 0) {
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    ::execv(argv[0], argv.data());
    ::_exit(127);
  }
  const long timeout_ms = static_cast<long>((seconds + 30.0) * 1000.0);
  long elapsed = 0;
  int status = 0;
  while (elapsed < timeout_ms) {
    const pid_t got = ::waitpid(pid, &status, WNOHANG);
    if (got == pid) {
      if (WIFEXITED(status)) return WEXITSTATUS(status) == 127 ? -1 : WEXITSTATUS(status);
      return 2;
    }
    sleep_ms(50);  // the harness, not the lab: it may poll
    elapsed += 50;
  }
  ::kill(pid, SIGKILL);
  ::waitpid(pid, &status, 0);
  std::fprintf(stderr, "frametime: lab timed out\n");
  return -1;
}

// Copies `from` to `to` through a temporary in the same directory, fsynced,
// then renamed over: a failed write cannot destroy the previous baseline.
bool replace_with_copy(const std::string& from, const std::string& to) {
  const auto content = read_file(from);
  if (!content) return false;
  const std::string temporary = to + ".tmp";
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  std::size_t done = 0;
  bool ok = true;
  while (done < content->size()) {
    const ssize_t n = ::write(fd, content->data() + done, content->size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      ok = false;
      break;
    }
    done += static_cast<std::size_t>(n);
  }
  ok = ok && ::fsync(fd) == 0;
  ok = ::close(fd) == 0 && ok;
  if (!ok || ::rename(temporary.c_str(), to.c_str()) != 0) {
    ::unlink(temporary.c_str());
    return false;
  }
  const int dir = ::open(directory_of(to).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir >= 0) {
    (void)::fsync(dir);
    ::close(dir);
  }
  return true;
}

void usage() {
  std::fprintf(stderr,
               "frametime - MediaViewer's frame-time harness on Linux (Linux PR 1)\n"
               "\n"
               "  frametime [--seconds N] [--baseline PATH] [--update-baseline] [--lab PATH]\n"
               "\n"
               "  --seconds N         soak duration; default 60, which is the verify line\n"
               "  --baseline PATH     rolling baseline JSON; default beside this executable\n"
               "  --update-baseline   save this run only after both gates and the regression\n"
               "                      check pass\n"
               "  --lab PATH          mediaviewer_lab; default: beside this executable\n"
               "\n"
               "Runs in the Wayland session the environment names (WAYLAND_DISPLAY).\n"
               "Exit codes: 0 pass, 1 gate or regression, 2 could not measure.\n");
}

}  // namespace

int main(int argc, char** argv) {
  double seconds = 60.0;
  std::string baseline_path;
  std::string lab_path;
  bool update_baseline = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (arg == "--seconds") {
      const std::string value = next();
      char* end = nullptr;
      seconds = std::strtod(value.c_str(), &end);
      if (value.empty() || *end != '\0') {
        std::fprintf(stderr, "frametime: invalid --seconds value\n");
        return 2;
      }
    } else if (arg == "--baseline") {
      baseline_path = next();
    } else if (arg == "--lab") {
      lab_path = next();
    } else if (arg == "--update-baseline") {
      update_baseline = true;
    } else if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "frametime: unrecognised argument %s\n", arg.c_str());
      return 2;
    }
  }
  if (!std::isfinite(seconds) || seconds <= 0.0 || seconds > 86400.0) {
    std::fprintf(stderr, "frametime: --seconds must be in (0, 86400]; gates require >= 60\n");
    return 2;
  }
  const char* wayland = std::getenv("WAYLAND_DISPLAY");
  if (!wayland || !*wayland) {
    std::fprintf(stderr, "frametime: needs a Wayland session (WAYLAND_DISPLAY is not set)\n");
    return 2;
  }

  const std::string here = executable_directory();
  if (lab_path.empty()) lab_path = here + "/mediaviewer_lab";
  if (baseline_path.empty()) baseline_path = here + "/frametime-baseline.json";
  const std::string report_path = here + "/frametime-report.json";
  const std::string idle_path = here + "/frametime-idle-report.json";

  char seconds_text[32]{};
  std::snprintf(seconds_text, sizeof(seconds_text), "%.6f", seconds);
  const auto measure = [&](bool static_run,
                           const std::string& path) -> std::optional<mv::frametime::report> {
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
      std::fprintf(stderr, "frametime: cannot clear the previous report\n");
      return std::nullopt;
    }
    // No still is opened: Linux PR 1 decodes nothing, so the sweep is the
    // workload, as on the Mac.
    std::vector<std::string> args = {lab_path, "--soak", seconds_text, "--json", path, "--gate"};
    if (static_run) args.emplace_back("--static");
    std::fprintf(stdout, "frametime: %s soak, %.3f s...\n", static_run ? "idle" : "animated",
                 seconds);
    std::fflush(stdout);
    const int code = run(args, seconds);
    const auto json = read_file(path);
    const auto measured = json ? mv::frametime::parse(*json) : std::nullopt;
    if (!measured || code < 0 || code > 1) {
      std::fprintf(stderr, "frametime: measurement failed (lab exit %d)\n", code);
      return std::nullopt;
    }
    auto r = *measured;
    // Even a convincing report cannot override a failing child exit code.
    if (code != 0) r.meets_gate = false;
    return r;
  };

  const auto current = measure(false, report_path);
  const auto idle = measure(true, idle_path);
  if (!current || !idle) return 2;

  std::fprintf(stdout,
               "  animated: %llu frames, %.3f s, refresh %.6f ms, p50 %.3f, p99 %.3f, max %.3f ms\n",
               static_cast<unsigned long long>(current->frames), current->elapsed_seconds,
               current->refresh_interval_ms, current->p50_ms, current->p99_ms, current->max_ms);
  std::fprintf(stdout, "  drops: %llu, displayed: %llu, not judged by the display: %llu, discontinuities: %llu\n",
               static_cast<unsigned long long>(current->dropped_frames),
               static_cast<unsigned long long>(current->displayed_presents),
               static_cast<unsigned long long>(current->statistics_unavailable_frames),
               static_cast<unsigned long long>(current->statistics_discontinuities));
  std::fprintf(stdout, "  drop_source: %s\n", current->drop_source.c_str());
  std::fprintf(stdout, "  idle: %.3f s, %.4f %% of one CPU core, %llu presents, %llu input events\n",
               idle->idle_elapsed_seconds, idle->idle_cpu_percent,
               static_cast<unsigned long long>(idle->idle_presents),
               static_cast<unsigned long long>(idle->idle_input_events));

  int result = 0;
  if (!mv::frametime::passes_linux_pr1(*current, false, seconds)) {
    std::fprintf(stderr, "FAIL: animated cadence, duration, or presentation-feedback gate\n");
    result = 1;
  }
  if (!mv::frametime::passes_linux_pr1(*idle, true, seconds)) {
    std::fprintf(stderr, "FAIL: idle duration, CPU, or zero-presentation gate\n");
    result = 1;
  }
  // The Windows harness's regression check (p99 regress > 10 % fails), on a
  // baseline that itself passed on this display mode.
  struct stat baseline_stat {};
  if (::stat(baseline_path.c_str(), &baseline_stat) == 0) {
    const auto json = read_file(baseline_path);
    const auto baseline = json ? mv::frametime::parse(*json) : std::nullopt;
    if (!baseline || !mv::frametime::passes_linux_pr1(*baseline, false, 60.0) ||
        baseline->p99_ms <= 0.0) {
      std::fprintf(stderr, "FAIL: baseline is invalid or not a Linux PR 1 pass; use a new baseline path\n");
      return 2;
    }
    if (std::abs(baseline->refresh_interval_ms - current->refresh_interval_ms) >
        current->refresh_interval_ms * 0.001) {
      std::fprintf(stderr, "FAIL: baseline refresh differs; use a baseline for this display mode\n");
      result = 1;
    } else if (current->p99_ms > baseline->p99_ms * 1.10) {
      std::fprintf(stderr, "FAIL: p99 regressed more than 10 percent\n");
      result = 1;
    }
  }
  if (update_baseline && result == 0) {
    if (!replace_with_copy(report_path, baseline_path)) {
      std::fprintf(stderr, "frametime: could not write the baseline\n");
      return 2;
    }
    std::fprintf(stdout, "baseline updated\n");
  }
  std::fprintf(stdout, result == 0 ? "PASS\n" : "FAILED\n");
  return result;
}
