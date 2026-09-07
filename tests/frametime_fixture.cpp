// SPDX-License-Identifier: GPL-2.0-or-later
// Deliberately synthetic child reports for testing the harness, never a GPU gate.
#include <windows.h>
#include <cstdio>
#include <string>

int wmain(int argc, wchar_t** argv) {
  std::wstring path;
  bool idle = false;
  for (int i = 1; i < argc; ++i) {
    if (std::wstring(argv[i]) == L"--json" && i + 1 < argc) path = argv[++i];
    else if (std::wstring(argv[i]) == L"--static") idle = true;
  }
  wchar_t mode_text[64]{};
  ::GetEnvironmentVariableW(L"MV_FRAMETIME_FIXTURE_MODE", mode_text, 64);
  const std::wstring mode = mode_text;
  FILE* f = nullptr;
  if (::_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return 2;
  if (mode == L"malformed") {
    std::fputs("{\"schema\": 2, \"meets_pr1_gate\": true}", f);
  } else {
    std::fprintf(f,
        "{\n\"schema\": 2,\n\"elapsed_seconds\": %.1f,\n"
        "\"refresh_interval_ms\": %.6f,\n\"mean_ms\": 16.666667,\n"
        "\"p50_ms\": %.3f,\n\"p99_ms\": %.3f,\n\"max_ms\": 20,\n"
        "\"frames\": 3600,\n\"displayed_presents\": 3600,\n"
        "\"dropped_frames\": %d,\n\"missed_refreshes\": 0,\n"
        "\"statistics_discontinuities\": 0,\n\"statistics_unavailable_frames\": %d,\n"
        "\"drop_source\": \"DXGI frame statistics\",\n"
        "\"idle_elapsed_seconds\": 60.0,\n\"idle_cpu_percent\": %.3f,\n"
        "\"idle_presents\": %d,\n\"idle_input_events\": 0,\n"
        "\"static\": %s,\n\"measurement_complete\": true,\n"
        "\"meets_pr1_gate\": true\n}\n",
        mode == L"short" ? 5.0 : 60.0,
        mode == L"unknown-refresh" ? 0.0 : 1000.0 / 60.0,
        mode == L"fast" ? 8.35 : 16.7,
        mode == L"regression" ? 19.0 : 17.0,
        mode == L"drop" ? 1 : 0, mode == L"gap" ? 1 : 0,
        mode == L"busy-idle" ? 10.0 : 0.05,
        mode == L"idle-present" ? 1 : 0, idle ? "true" : "false");
  }
  std::fclose(f);
  return mode == L"child-error" ? 2 : mode == L"child-fail" ? 1 : 0;
}
