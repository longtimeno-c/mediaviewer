// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Deliberately synthetic child reports for testing the Linux harness
// (tools/frametime/main_linux.cpp), never a GPU gate. The twin of
// frametime_fixture.cpp, writing the Linux PR 1 source, plus two modes only
// this harness needs: another platform's authoritative source, and the
// weaker label headless Weston earns.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
  std::string path;
  bool idle = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--json") == 0 && i + 1 < argc) path = argv[++i];
    else if (std::strcmp(argv[i], "--static") == 0) idle = true;
  }
  const char* env = std::getenv("MV_FRAMETIME_FIXTURE_MODE");
  const std::string mode = env ? env : "";
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return 2;
  if (mode == "malformed") {
    std::fputs("{\"schema\": 2, \"meets_pr1_gate\": true}", f);
  } else {
    const char* source = mode == "weak"   ? "clock intervals (weaker)"
                         : mode == "dxgi" ? "DXGI frame statistics"
                                          : "Wayland presentation feedback";
    std::fprintf(f,
        "{\n\"schema\": 2,\n\"elapsed_seconds\": %.1f,\n"
        "\"refresh_interval_ms\": %.6f,\n\"mean_ms\": 16.666667,\n"
        "\"p50_ms\": %.3f,\n\"p99_ms\": %.3f,\n\"max_ms\": 20,\n"
        "\"frames\": 3600,\n\"displayed_presents\": 3600,\n"
        "\"dropped_frames\": %d,\n\"missed_refreshes\": 0,\n"
        "\"statistics_discontinuities\": 0,\n\"statistics_unavailable_frames\": %d,\n"
        "\"drop_source\": \"%s\",\n"
        "\"idle_elapsed_seconds\": 60.0,\n\"idle_cpu_percent\": %.3f,\n"
        "\"idle_presents\": %d,\n\"idle_input_events\": 0,\n"
        "\"static\": %s,\n\"measurement_complete\": true,\n"
        "\"meets_pr1_gate\": true\n}\n",
        mode == "short" ? 5.0 : 60.0,
        mode == "unknown-refresh" ? 0.0 : 1000.0 / 60.0,
        mode == "fast" ? 8.35 : 16.7,
        mode == "regression" ? 19.0 : 17.0,
        mode == "drop" ? 1 : 0, mode == "gap" ? 1 : 0, source,
        mode == "busy-idle" ? 10.0 : 0.05,
        mode == "idle-present" ? 1 : 0, idle ? "true" : "false");
  }
  std::fclose(f);
  return mode == "child-error" ? 2 : mode == "child-fail" ? 1 : 0;
}
