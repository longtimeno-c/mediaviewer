// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Darwin frame-time harness. Same schema-2 JSON as Windows; the gate requires
// drop_source "Metal display-link". A DXGI report cannot pass.

#include <mach-o/dyld.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

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
  return in.fail() ? std::nullopt : std::optional{content};
}

std::string directory_of(const std::string& path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos ? "." : path.substr(0, slash);
}

std::string executable_directory() {
  char buf[4096]{};
  std::uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) != 0) return ".";
  char resolved[4096]{};
  if (realpath(buf, resolved)) return directory_of(resolved);
  return directory_of(buf);
}

int run(const std::vector<std::string>& args, double seconds) {
  pid_t pid = fork();
  if (pid < 0) {
    std::fprintf(stderr, "frametime: fork failed\n");
    return -1;
  }
  if (pid == 0) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    _exit(127);
  }
  const int timeout_ms = static_cast<int>((seconds + 30.0) * 1000.0);
  int elapsed = 0;
  int status = 0;
  while (elapsed < timeout_ms) {
    const pid_t got = waitpid(pid, &status, WNOHANG);
    if (got == pid) {
      if (WIFEXITED(status)) return WEXITSTATUS(status);
      return 2;
    }
    usleep(50 * 1000);
    elapsed += 50;
  }
  kill(pid, SIGKILL);
  waitpid(pid, &status, 0);
  std::fprintf(stderr, "frametime: lab timed out\n");
  return -1;
}

void usage() {
  std::fprintf(stderr,
               "frametime — MediaViewer Metal frame-time harness (PR 16)\n"
               "  frametime [--seconds N] [--baseline PATH] [--update-baseline] [--lab PATH]\n");
}

}  // namespace

int main(int argc, char** argv) {
  double seconds = 60.0;
  std::string baseline_path;
  std::string lab_path;
  bool update_baseline = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (arg == "--seconds") {
      seconds = std::strtod(next().c_str(), nullptr);
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
    std::fprintf(stderr, "frametime: --seconds must be in (0, 86400]\n");
    return 2;
  }

  const std::string here = executable_directory();
  if (lab_path.empty()) lab_path = here + "/mediaviewer_lab";
  if (baseline_path.empty()) baseline_path = here + "/frametime-baseline.json";
  const std::string report_path = here + "/frametime-report.json";
  const std::string idle_path = here + "/frametime-idle-report.json";

  const auto measure = [&](bool static_run, const std::string& path)
      -> std::optional<mv::frametime::report> {
    ::unlink(path.c_str());
    char seconds_text[32]{};
    std::snprintf(seconds_text, sizeof(seconds_text), "%.6f", seconds);
    std::vector<std::string> args = {lab_path, "--soak", seconds_text, "--json", path, "--gate"};
    if (static_run) args.emplace_back("--static");
    std::fprintf(stdout, "frametime: %s soak, %.3f s...\n", static_run ? "idle" : "animated",
                 seconds);
    const int code = run(args, seconds);
    const auto json = read_file(path);
    const auto measured = json ? mv::frametime::parse(*json) : std::nullopt;
    if (!measured || code < 0 || code > 1) {
      std::fprintf(stderr, "frametime: measurement failed (lab exit %d): %s\n", code, path.c_str());
      return std::nullopt;
    }
    auto r = *measured;
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
  std::fprintf(stdout, "  drop_source: %s\n", current->drop_source.c_str());
  std::fprintf(stdout, "  idle: %.3f s, %.4f %% of one CPU core, %llu presents, %llu input events\n",
               idle->idle_elapsed_seconds, idle->idle_cpu_percent,
               static_cast<unsigned long long>(idle->idle_presents),
               static_cast<unsigned long long>(idle->idle_input_events));

  int result = 0;
  if (!mv::frametime::passes_pr16(*current, false, seconds)) {
    std::fprintf(stderr, "FAIL: animated cadence or Metal display-link gate\n");
    result = 1;
  }
  if (!mv::frametime::passes_pr16(*idle, true, seconds)) {
    std::fprintf(stderr, "FAIL: idle duration, CPU, or zero-presentation gate\n");
    result = 1;
  }
  std::fprintf(stdout, result == 0 ? "PASS\n" : "FAILED\n");
  (void)update_baseline;
  (void)baseline_path;
  return result;
}
