// SPDX-License-Identifier: GPL-2.0-or-later
//
// tools/frametime — the frame-time regression harness (plan/09-build-and-test.md).
//
//   "Scripted sessions, captured with PresentMon/ETW. CI fails the build if p99
//    frame time regresses > 10 % or any frame exceeds 2x the refresh interval.
//    Treat a dropped frame as a test failure, not a nuisance."
//
// It drives mediaviewer_lab's soak mode, reads the JSON report, and compares it
// against a rolling baseline. Rolling, not absolute: silicon and drivers drift,
// and an absolute number becomes a nuisance everyone learns to ignore.
//
// The lab is a separate process on purpose. The measurement must include
// process startup, the compositor handshake, and the real swapchain — running
// the loop in-process here would measure a different program.
//
// PR 1 runs one scripted session (a static window presenting at refresh).
// PR 2 onward add theirs; the shape does not change.

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

struct report {
  double elapsed_seconds = 0.0;
  double refresh_interval_ms = 0.0;
  double mean_ms = 0.0;
  double p50_ms = 0.0;
  double p99_ms = 0.0;
  double max_ms = 0.0;
  double cpu_mean_ms = 0.0;
  double cpu_p99_ms = 0.0;
  double cpu_max_ms = 0.0;
  unsigned long long frames = 0;
  unsigned long long dropped_frames = 0;
  unsigned long long missed_refreshes = 0;
  unsigned long long statistics_discontinuities = 0;
  double warmup_seconds_discarded = 0.0;
  std::string drop_source;
  bool meets_gate = false;
};

// A deliberately small JSON reader. The report is written by us, in a fixed
// shape, and pulling a JSON library into a build tool to read eleven scalars is
// how a toolchain acquires dependencies nobody remembers agreeing to.
std::optional<std::string> read_file(const std::wstring& path) {
  FILE* f = nullptr;
  if (::_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr) return std::nullopt;
  std::string content;
  char buffer[4096];
  while (const std::size_t n = std::fread(buffer, 1, sizeof(buffer), f)) {
    content.append(buffer, n);
  }
  std::fclose(f);
  return content;
}

bool scalar(const std::string& json, const char* key, double& out) {
  const std::string needle = std::string("\"") + key + "\":";
  const auto at = json.find(needle);
  if (at == std::string::npos) return false;
  out = std::strtod(json.c_str() + at + needle.size(), nullptr);
  return true;
}

bool text(const std::string& json, const char* key, std::string& out) {
  const std::string needle = std::string("\"") + key + "\": \"";
  const auto at = json.find(needle);
  if (at == std::string::npos) return false;
  const auto start = at + needle.size();
  const auto end = json.find('"', start);
  if (end == std::string::npos) return false;
  out = json.substr(start, end - start);
  return true;
}

std::optional<report> parse(const std::string& json) {
  report r;
  double value = 0.0;
  if (!scalar(json, "elapsed_seconds", r.elapsed_seconds)) return std::nullopt;
  if (!scalar(json, "refresh_interval_ms", r.refresh_interval_ms)) return std::nullopt;
  if (!scalar(json, "p99_ms", r.p99_ms)) return std::nullopt;
  scalar(json, "mean_ms", r.mean_ms);
  scalar(json, "p50_ms", r.p50_ms);
  scalar(json, "max_ms", r.max_ms);
  scalar(json, "cpu_mean_ms", r.cpu_mean_ms);
  scalar(json, "cpu_p99_ms", r.cpu_p99_ms);
  scalar(json, "cpu_max_ms", r.cpu_max_ms);
  if (scalar(json, "frames", value)) r.frames = static_cast<unsigned long long>(value);
  if (scalar(json, "dropped_frames", value))
    r.dropped_frames = static_cast<unsigned long long>(value);
  if (scalar(json, "missed_refreshes", value))
    r.missed_refreshes = static_cast<unsigned long long>(value);
  if (scalar(json, "statistics_discontinuities", value))
    r.statistics_discontinuities = static_cast<unsigned long long>(value);
  scalar(json, "warmup_seconds_discarded", r.warmup_seconds_discarded);
  text(json, "drop_source", r.drop_source);
  r.meets_gate = json.find("\"meets_pr1_gate\": true") != std::string::npos;
  return r;
}

std::wstring directory_of(const std::wstring& path) {
  const auto slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::wstring executable_directory() {
  wchar_t buffer[MAX_PATH]{};
  ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  return directory_of(buffer);
}

int run(const std::wstring& command_line) {
  std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back(L'\0');

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};

  if (!::CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &si, &pi)) {
    std::fwprintf(stderr, L"frametime: cannot launch %ls (error %lu)\n", command_line.c_str(),
                  ::GetLastError());
    return -1;
  }

  ::WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exit_code = 0;
  ::GetExitCodeProcess(pi.hProcess, &exit_code);
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
  return static_cast<int>(exit_code);
}

void usage() {
  std::wprintf(
      L"frametime — MediaViewer frame-time regression harness\n"
      L"\n"
      L"  frametime [--seconds N] [--baseline PATH] [--update-baseline] [--lab PATH]\n"
      L"\n"
      L"  --seconds N         soak duration; default 60, which is PR 1's verify line\n"
      L"  --baseline PATH     rolling baseline JSON; default tools/frametime/baseline.json\n"
      L"  --update-baseline   write this run's numbers as the new baseline and exit 0\n"
      L"  --lab PATH          mediaviewer_lab.exe; default: beside this executable\n"
      L"\n"
      L"Exit codes: 0 pass, 1 gate or regression, 2 could not measure.\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  double seconds = 60.0;
  std::wstring baseline_path;
  std::wstring lab_path;
  bool update_baseline = false;

  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    const auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
    if (arg == L"--seconds") seconds = ::_wtof(next().c_str());
    else if (arg == L"--baseline") baseline_path = next();
    else if (arg == L"--lab") lab_path = next();
    else if (arg == L"--update-baseline") update_baseline = true;
    else if (arg == L"--help" || arg == L"-h") { usage(); return 0; }
    else { std::fwprintf(stderr, L"frametime: unrecognised argument %ls\n", arg.c_str()); return 2; }
  }

  const std::wstring here = executable_directory();
  if (lab_path.empty()) lab_path = here + L"\\mediaviewer_lab.exe";
  if (baseline_path.empty()) baseline_path = here + L"\\frametime-baseline.json";

  if (::GetFileAttributesW(lab_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    std::fwprintf(stderr, L"frametime: %ls not found. Build mediaviewer_lab first.\n",
                  lab_path.c_str());
    return 2;
  }

  const std::wstring report_path = here + L"\\frametime-report.json";
  ::DeleteFileW(report_path.c_str());

  wchar_t seconds_text[32]{};
  ::swprintf_s(seconds_text, L"%.3f", seconds);

  const std::wstring command = L"\"" + lab_path + L"\" --soak " + seconds_text + L" --json \"" +
                               report_path + L"\" --gate";

  std::wprintf(L"frametime: soaking %.0f s...\n", seconds);
  const int lab_exit = run(command);
  if (lab_exit < 0) return 2;

  const auto json = read_file(report_path);
  if (!json) {
    std::fwprintf(stderr, L"frametime: no report at %ls (lab exited %d)\n", report_path.c_str(),
                  lab_exit);
    return 2;
  }
  const auto current = parse(*json);
  if (!current) {
    std::fwprintf(stderr, L"frametime: report at %ls is not readable\n", report_path.c_str());
    return 2;
  }

  std::wprintf(L"\n  frames            %llu over %.1f s\n", current->frames,
               current->elapsed_seconds);
  std::wprintf(L"  refresh           %.3f ms (%.2f Hz)\n", current->refresh_interval_ms,
               current->refresh_interval_ms > 0.0 ? 1000.0 / current->refresh_interval_ms : 0.0);
  std::wprintf(L"  present-to-present  mean %.3f  p50 %.3f  p99 %.3f  max %.3f ms\n",
               current->mean_ms, current->p50_ms, current->p99_ms, current->max_ms);
  std::wprintf(L"  dropped           %llu (%llu missed refreshes)\n", current->dropped_frames,
               current->missed_refreshes);
  std::wprintf(L"  cpu frame           mean %.3f  p99 %.3f  max %.3f ms\n",
               current->cpu_mean_ms, current->cpu_p99_ms, current->cpu_max_ms);
  std::wprintf(L"  drop source       %hs\n", current->drop_source.c_str());
  std::wprintf(L"  discontinuities   %llu\n", current->statistics_discontinuities);
  std::wprintf(L"  warm-up discarded %.1f s before measuring\n\n",
               current->warmup_seconds_discarded);

  int result = 0;

  // --- The PR 1 gate, and every later PR's inherited one -----------------
  if (!current->meets_gate) {
    std::fwprintf(stderr,
                  L"FAIL: PR 1's present-loop verify does not hold "
                  L"(%llu dropped frames, source: %hs).\n",
                  current->dropped_frames, current->drop_source.c_str());
    result = 1;
  }

  // --- A measurement DXGI could not keep continuous is not a measurement ---
  if (current->statistics_discontinuities > 0) {
    std::fwprintf(stderr,
                  L"FAIL: %llu frame-statistics discontinuities; this run cannot be "
                  L"trusted to have measured the gate.\n",
                  current->statistics_discontinuities);
    result = 1;
  }

  // --- No frame may exceed 2x the refresh interval -----------------------
  if (current->refresh_interval_ms > 0.0 &&
      current->max_ms > current->refresh_interval_ms * 2.0) {
    std::fwprintf(stderr, L"FAIL: worst frame %.3f ms exceeds 2x refresh (%.3f ms).\n",
                  current->max_ms, current->refresh_interval_ms * 2.0);
    result = 1;
  }

  // --- Rolling p99 baseline ----------------------------------------------
  if (const auto baseline_json = read_file(baseline_path)) {
    if (const auto baseline = parse(*baseline_json)) {
      if (baseline->p99_ms > 0.0) {
        const double change = (current->p99_ms - baseline->p99_ms) / baseline->p99_ms;
        std::wprintf(L"  p99 vs baseline   %+.1f %% (%.3f -> %.3f ms)\n", change * 100.0,
                     baseline->p99_ms, current->p99_ms);
        if (change > 0.10) {
          std::fwprintf(stderr, L"FAIL: p99 regressed %.1f %% against the baseline.\n",
                        change * 100.0);
          result = 1;
        }
      }
    }
  } else {
    std::wprintf(L"  (no baseline at %ls — run with --update-baseline to set one)\n",
                 baseline_path.c_str());
  }

  if (update_baseline) {
    if (::CopyFileW(report_path.c_str(), baseline_path.c_str(), FALSE)) {
      std::wprintf(L"\nbaseline updated: %ls\n", baseline_path.c_str());
      return 0;
    }
    std::fwprintf(stderr, L"frametime: could not write baseline %ls\n", baseline_path.c_str());
    return 2;
  }

  std::wprintf(result == 0 ? L"PASS\n" : L"FAILED\n");
  return result;
}
