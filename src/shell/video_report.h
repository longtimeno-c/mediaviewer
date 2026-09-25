// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace mv::shell {
// Written alongside the display-pacing report by both native render loops.
// Selection skips and missed display refreshes are different measurements.
struct video_report {
  std::uint64_t selected = 0, dropped = 0, cadence = 0, starved = 0;
  double error_p99_ms = 0;
  bool audio_master = false;
};
inline bool write_video_report(std::filesystem::path path, const video_report& r) {
  path += ".video.json";
  std::ofstream out(path);
  out << "{\n  \"measurement\": \"native render loop frame selection\",\n"
      << "  \"selected\": " << r.selected << ",\n"
      << "  \"dropped\": " << r.dropped << ",\n"
      << "  \"cadence\": " << r.cadence << ",\n"
      << "  \"starved\": " << r.starved << ",\n"
      << "  \"error_p99_ms\": " << r.error_p99_ms << ",\n"
      << "  \"audio_master\": " << (r.audio_master ? "true" : "false") << "\n}\n";
  out.close();
  return !out.fail();
}
} // namespace mv::shell
