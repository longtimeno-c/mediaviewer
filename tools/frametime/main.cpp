// SPDX-License-Identifier: GPL-2.0-or-later
// Runs the animated and static PR 1 gates, then optionally saves the passing report.

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "gfx/pacer.h"

namespace {

struct report {
  mv::gfx::pace_stats pace;
  mv::gfx::idle_stats idle;
  bool static_run = false;
  bool complete = false;
  bool meets_gate = false;
};

std::optional<std::string> read_file(const std::wstring& path) {
  FILE* f = nullptr;
  if (::_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return std::nullopt;
  std::string content;
  char buffer[4096];
  while (const auto n = std::fread(buffer, 1, sizeof(buffer), f)) content.append(buffer, n);
  const bool failed = std::ferror(f) != 0;
  std::fclose(f);
  return failed ? std::nullopt : std::optional{content};
}

// The lab writes a flat schema. Require each gate field once, a finite numeric
// value, and a delimiter; missing/malformed values must not turn into zero.
std::optional<std::string> field(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const auto at = json.find(needle);
  if (at == std::string::npos || json.find(needle, at + needle.size()) != std::string::npos)
    return std::nullopt;
  const auto start = json.find_first_not_of(" \r\n\t", at + needle.size());
  const auto end = json.find_first_of(",}", start);
  if (start == std::string::npos || end == std::string::npos) return std::nullopt;
  const auto last = json.find_last_not_of(" \r\n\t", end - 1);
  return json.substr(start, last - start + 1);
}

bool scalar(const std::string& json, const char* key, double& out) {
  const auto value = field(json, key);
  if (!value) return false;
  char* end = nullptr;
  out = std::strtod(value->c_str(), &end);
  return end != value->c_str() && *end == '\0' && std::isfinite(out);
}

bool count(const std::string& json, const char* key, std::uint64_t& out) {
  double value = 0.0;
  if (!scalar(json, key, value) || value < 0.0 || value >= 9007199254740992.0 ||
      std::floor(value) != value) return false;
  out = static_cast<std::uint64_t>(value);
  return true;
}

bool boolean(const std::string& json, const char* key, bool& out) {
  const auto value = field(json, key);
  if (!value || (*value != "true" && *value != "false")) return false;
  out = *value == "true";
  return true;
}

std::optional<report> parse(const std::string& json) {
  report r;
  auto& p = r.pace;
  double schema = 0.0;
  if (!scalar(json, "schema", schema) || schema != 2.0 ||
      !scalar(json, "elapsed_seconds", p.elapsed_seconds) ||
      !scalar(json, "refresh_interval_ms", p.refresh_interval_ms) ||
      !scalar(json, "mean_ms", p.mean_ms) || !scalar(json, "p50_ms", p.p50_ms) ||
      !scalar(json, "p99_ms", p.p99_ms) || !scalar(json, "max_ms", p.max_ms) ||
      !count(json, "frames", p.frames) || !count(json, "dropped_frames", p.dropped_frames) ||
      !count(json, "missed_refreshes", p.missed_refreshes) ||
      !count(json, "displayed_presents", p.displayed_presents) ||
      !count(json, "statistics_discontinuities", p.statistics_discontinuities) ||
      !count(json, "statistics_unavailable_frames", p.statistics_unavailable_frames) ||
      !scalar(json, "idle_elapsed_seconds", r.idle.elapsed_seconds) ||
      !scalar(json, "idle_cpu_percent", r.idle.cpu_percent) ||
      !count(json, "idle_presents", r.idle.presents) ||
      !count(json, "idle_input_events", r.idle.input_events) ||
      !boolean(json, "static", r.static_run) ||
      !boolean(json, "measurement_complete", r.complete) ||
      !boolean(json, "meets_pr1_gate", r.meets_gate)) return std::nullopt;
  const auto source = field(json, "drop_source");
  if (!source) return std::nullopt;
  if (*source == "\"DXGI frame statistics\"") p.source = mv::gfx::drop_source::frame_statistics;
  else if (*source == "\"QPC intervals (weaker)\"") p.source = mv::gfx::drop_source::interval_heuristic;
  else if (*source != "\"no data\"") return std::nullopt;
  return r;
}

bool passes(const report& r, bool static_run, double seconds) {
  return r.complete && r.meets_gate && r.static_run == static_run &&
         (static_run ? r.idle.elapsed_seconds : r.pace.elapsed_seconds) >= seconds &&
         (static_run ? r.idle.meets_pr1_gate() : r.pace.meets_pr1_gate());
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

int run(const std::wstring& command_line, double seconds) {
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

  const DWORD timeout = static_cast<DWORD>((seconds + 30.0) * 1000.0);
  const DWORD waited = ::WaitForSingleObject(pi.hProcess, timeout);
  DWORD exit_code = 2;
  if (waited != WAIT_OBJECT_0) {
    std::fwprintf(stderr, L"frametime: lab timed out or process wait failed\n");
    ::TerminateProcess(pi.hProcess, 2);
    ::WaitForSingleObject(pi.hProcess, 5000);
  } else if (!::GetExitCodeProcess(pi.hProcess, &exit_code)) {
    exit_code = 2;
  }
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
  return static_cast<int>(exit_code);
}

bool write_pattern_bmp(const std::wstring& path, std::uint32_t w, std::uint32_t h) {
  const std::uint32_t row = (w * 3u + 3u) & ~3u;
  const std::uint32_t off = 54;
  const std::uint32_t size = off + row * h;
  std::vector<std::uint8_t> out(size, 0);
  out[0] = 'B';
  out[1] = 'M';
  const auto u32 = [&](std::size_t o, std::uint32_t v) {
    out[o] = static_cast<std::uint8_t>(v);
    out[o + 1] = static_cast<std::uint8_t>(v >> 8);
    out[o + 2] = static_cast<std::uint8_t>(v >> 16);
    out[o + 3] = static_cast<std::uint8_t>(v >> 24);
  };
  const auto u16 = [&](std::size_t o, std::uint16_t v) {
    out[o] = static_cast<std::uint8_t>(v);
    out[o + 1] = static_cast<std::uint8_t>(v >> 8);
  };
  u32(2, size);
  u32(10, off);
  u32(14, 40);
  u32(18, w);
  u32(22, h);
  u16(26, 1);
  u16(28, 24);
  for (std::uint32_t y = 0; y < h; ++y) {
    std::uint8_t* dst = out.data() + off + static_cast<std::size_t>(y) * row;
    for (std::uint32_t x = 0; x < w; ++x) {
      dst[x * 3 + 0] = static_cast<std::uint8_t>(x);
      dst[x * 3 + 1] = static_cast<std::uint8_t>(y);
      dst[x * 3 + 2] = 160;
    }
  }
  FILE* f = nullptr;
  if (::_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
  const auto n = std::fwrite(out.data(), 1, out.size(), f);
  const bool ok = n == out.size() && std::ferror(f) == 0;
  return std::fclose(f) == 0 && ok;
}

void usage() {
  std::wprintf(
      L"frametime — MediaViewer frame-time regression harness\n"
      L"\n"
      L"  frametime [--seconds N] [--baseline PATH] [--update-baseline] [--lab PATH]\n"
      L"\n"
      L"  --seconds N         soak duration; default 60, which is PR 1's verify line\n"
      L"  --baseline PATH     rolling baseline JSON; default beside this executable\n"
      L"  --update-baseline   save this run only after both gates and regression checks pass\n"
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
    if (arg == L"--seconds") {
      const auto value = next();
      wchar_t* end = nullptr;
      seconds = std::wcstod(value.c_str(), &end);
      if (end == value.c_str() || *end != L'\0') {
        std::fwprintf(stderr, L"frametime: invalid --seconds value\n");
        return 2;
      }
    }
    else if (arg == L"--baseline") baseline_path = next();
    else if (arg == L"--lab") lab_path = next();
    else if (arg == L"--update-baseline") update_baseline = true;
    else if (arg == L"--help" || arg == L"-h") { usage(); return 0; }
    else { std::fwprintf(stderr, L"frametime: unrecognised argument %ls\n", arg.c_str()); return 2; }
  }

  if (!std::isfinite(seconds) || seconds <= 0.0 || seconds > 86400.0) {
    std::fwprintf(stderr, L"frametime: --seconds must be in (0, 86400]; gates require >= 60\n");
    return 2;
  }
  const std::wstring here = executable_directory();
  if (lab_path.empty()) lab_path = here + L"\\mediaviewer_lab.exe";
  if (baseline_path.empty()) baseline_path = here + L"\\frametime-baseline.json";
  const std::wstring report_path = here + L"\\frametime-report.json";
  const std::wstring idle_path = here + L"\\frametime-idle-report.json";
  const std::wstring open_path = here + L"\\frametime-open.bmp";
  if (!write_pattern_bmp(open_path, 512, 512)) {
    std::fwprintf(stderr, L"frametime: cannot write %ls\n", open_path.c_str());
    return 2;
  }

  wchar_t seconds_text[32]{};
  ::swprintf_s(seconds_text, L"%.6f", seconds);
  const auto measure = [&](bool static_run, const std::wstring& path) -> std::optional<report> {
    if (!::DeleteFileW(path.c_str()) && ::GetLastError() != ERROR_FILE_NOT_FOUND) {
      std::fwprintf(stderr, L"frametime: cannot clear report %ls\n", path.c_str());
      return std::nullopt;
    }
    // Animated soak opens a BMP so the blit path is what is paced, not the
    // sweep bar. Idle stays empty: a still that lands after warmup would
    // present and fail the zero-present gate.
    const std::wstring command = L"\"" + lab_path + L"\" --soak " + seconds_text +
        L" --json \"" + path + L"\" --gate" +
        (static_run ? L" --static" : L" --open \"" + open_path + L"\"");
    std::wprintf(L"frametime: %ls soak, %.3f s...\n", static_run ? L"idle" : L"animated", seconds);
    const int code = run(command, seconds);
    const auto json = read_file(path);
    const auto measured = json ? parse(*json) : std::nullopt;
    if (!measured || code < 0 || code > 1) {
      std::fwprintf(stderr, L"frametime: measurement failed (lab exit %d): %ls\n", code, path.c_str());
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
  const auto& p = current->pace;
  std::wprintf(L"  animated: %llu frames, %.3f s, refresh %.6f ms, p50 %.3f, p99 %.3f, max %.3f ms\n",
               static_cast<unsigned long long>(p.frames), p.elapsed_seconds,
               p.refresh_interval_ms, p.p50_ms, p.p99_ms, p.max_ms);
  std::wprintf(L"  drops: %llu, statistics gaps: %llu, discontinuities: %llu\n",
               static_cast<unsigned long long>(p.dropped_frames),
               static_cast<unsigned long long>(p.statistics_unavailable_frames),
               static_cast<unsigned long long>(p.statistics_discontinuities));
  std::wprintf(L"  idle: %.3f s, %.4f %% of one CPU core, %llu presents, %llu input events\n",
               idle->idle.elapsed_seconds, idle->idle.cpu_percent,
               static_cast<unsigned long long>(idle->idle.presents),
               static_cast<unsigned long long>(idle->idle.input_events));
  int result = 0;
  if (!passes(*current, false, seconds)) {
    std::fwprintf(stderr, L"FAIL: animated cadence, duration, or display-statistics gate\n");
    result = 1;
  }
  if (!passes(*idle, true, seconds)) {
    std::fwprintf(stderr, L"FAIL: idle duration, CPU, or zero-presentation gate\n");
    result = 1;
  }
  if (::GetFileAttributesW(baseline_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    const auto json = read_file(baseline_path);
    const auto baseline = json ? parse(*json) : std::nullopt;
    if (!baseline || !passes(*baseline, false, 60.0) || baseline->pace.p99_ms <= 0.0) {
      std::fwprintf(stderr, L"FAIL: baseline is invalid or predates schema 2; use a new baseline path\n");
      return 2;
    }
    if (std::abs(baseline->pace.refresh_interval_ms - p.refresh_interval_ms) >
        p.refresh_interval_ms * 0.001) {
      std::fwprintf(stderr, L"FAIL: baseline refresh differs; use a baseline for this display mode\n");
      result = 1;
    } else if (p.p99_ms > baseline->pace.p99_ms * 1.10) {
      std::fwprintf(stderr, L"FAIL: p99 regressed more than 10 percent\n");
      result = 1;
    }
  }
  if (update_baseline && result == 0) {
    // Publish exactly the already validated report. Replace atomically so a
    // failed write cannot destroy the preceding baseline.
    const std::wstring temporary = baseline_path + L".tmp";
    if (!::CopyFileW(report_path.c_str(), temporary.c_str(), FALSE) ||
        !::MoveFileExW(temporary.c_str(), baseline_path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      std::fwprintf(stderr, L"frametime: could not write baseline %ls\n", baseline_path.c_str());
      ::DeleteFileW(temporary.c_str());
      return 2;
    }
    std::wprintf(L"baseline updated: %ls\n", baseline_path.c_str());
  }
  std::wprintf(result == 0 ? L"PASS\n" : L"FAILED\n");
  return result;
}
