// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/mobile.md §9: runs a query set through the phone's engine and writes the answers, one
// JSON line per query, for tools/ios/compare_parity.py. tools/ios/run-parity.sh runs this inside a
// Simulator of its own; numbers it prints are Simulator numbers, not phone numbers.
#include <mach/mach.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/json.h"
#include "mobile/mv_mobile_search.h"

namespace {

std::vector<std::string> read_lines(const char* path) {
  std::vector<std::string> out;
  std::ifstream in(path);
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line[0] != '#') out.push_back(line);
  }
  return out;
}

// The figure iOS holds an app to (jetsam): what task_info calls the physical footprint.
double footprint_mb() {
  task_vm_info_data_t info{};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return -1;
  return static_cast<double>(info.phys_footprint) / (1024.0 * 1024.0);
}

double ms_since(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 7) {
    std::fprintf(stderr, "usage: %s <gen dir> <library dir> <work dir> <kinds> <queries> <out.jsonl>\n", argv[0]);
    return 2;
  }
  const uint32_t kinds = static_cast<uint32_t>(std::stoul(argv[4]));
  // MV_PARITY_SOUND_DIR: the sound text tower's folder (src/mobile/sound_tower.h), verified.
  const char* sound = std::getenv("MV_PARITY_SOUND_DIR");
  mv_mobile_open_options options{sizeof options, argv[1], argv[2], argv[3], sound && *sound ? sound : nullptr};
  std::printf("footprint before open: %.0f MB\n", footprint_mb());
  mv_mobile_library* library = nullptr;
  auto t0 = std::chrono::steady_clock::now();
  mv_mobile_status st = mv_mobile_library_open(&options, &library);
  if (st != MV_MOBILE_OK) {
    std::fprintf(stderr, "open failed: %s\n", mv_mobile_status_message(st));
    return 1;
  }
  std::printf("open: %.1f ms (Simulator)\n", ms_since(t0));
  mv_mobile_library_info info{};
  mv_mobile_library_info_get(library, &info);
  std::printf("generation %llu, %u assets, %u people, tiny %u, sound %u, spec %s\n",
              static_cast<unsigned long long>(info.generation), info.assets, info.people, info.has_tiny, info.has_sound,
              info.active_spec);
  t0 = std::chrono::steady_clock::now();
  st = mv_mobile_search_start(library);
  if (st != MV_MOBILE_OK) {
    std::fprintf(stderr, "search start failed: %s\n", mv_mobile_status_message(st));
    mv_mobile_library_close(library);
    return 1;
  }
  std::printf("engine start: %.1f ms (Simulator)\n", ms_since(t0));
  while (mv_mobile_search_state(library) == MV_MOBILE_SEARCH_WARMING) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  std::printf("model ready: %.1f ms after start (Simulator); footprint %.0f MB\n", ms_since(t0), footprint_mb());
  if (mv_mobile_search_state(library) != MV_MOBILE_SEARCH_READY) {
    std::fprintf(stderr, "the model did not load\n");
    mv_mobile_library_close(library);
    return 1;
  }

  std::ofstream out(argv[6]);
  std::vector<double> times;
  double peak = footprint_mb();
  int bad = 0;
  for (const std::string& q : read_lines(argv[5])) {
    t0 = std::chrono::steady_clock::now();
    mv_mobile_results* r = nullptr;
    st = mv_mobile_search_run(library, q.c_str(), kinds, 120000, &r);
    const double ms = ms_since(t0);
    if (st != MV_MOBILE_OK) {
      ++bad;
      std::fprintf(stderr, "query failed (%s): %s\n", q.c_str(), mv_mobile_status_message(st));
      continue;
    }
    times.push_back(ms);
    mv::json::writer w;
    w.begin_object();
    w.key("query").string(q);
    w.key("ms").number(ms);
    w.key("rows").begin_array();
    const uint32_t n = mv_mobile_results_count(r);
    for (uint32_t i = 0; i < n; ++i) {
      mv_mobile_hit h{};
      mv_mobile_results_at(r, i, &h);
      w.begin_array();
      w.string(mv_mobile_results_path(r, i));
      w.integer(h.pts_ms);
      w.integer(h.kind);
      w.integer(h.match);
      w.integer(h.more_in_clip);
      {
        char score[32];
        std::snprintf(score, sizeof score, "%.7f", h.score);
        w.raw(score);
      }
      w.end_array();
    }
    w.end_array();
    w.end_object();
    out << w.take() << '\n';
    mv_mobile_results_close(r);
    peak = std::max(peak, footprint_mb());
  }
  std::sort(times.begin(), times.end());
  if (!times.empty()) {
    std::printf("submit -> results over %zu queries (Simulator, not a phone): median %.1f ms, p95 %.1f ms, max %.1f ms\n",
                times.size(), times[times.size() / 2], times[times.size() * 95 / 100], times.back());
  }
  std::printf("footprint after the queries: %.0f MB, highest seen between queries %.0f MB\n", footprint_mb(), peak);
  mv_mobile_library_close(library);
  return bad ? 1 : 0;
}
