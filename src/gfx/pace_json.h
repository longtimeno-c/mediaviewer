// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Schema-2 frame-time JSON used by both present labs and by frametime.
//
// The Windows lab writes this from DXGI statistics; the Metal lab writes the
// same keys with drop_source "Metal display-link". A parser that requires
// DXGI-only source strings cannot certify PR 16.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "gfx/metal_pacer.h"
#include "gfx/present_policy.h"

namespace mv::gfx {

struct pace_json {
  metal_pace_stats pace;
  metal_idle_stats idle;
  bool static_run = false;
  bool measurement_complete = false;
  bool meets_gate = false;
  double warmup_seconds = k_warmup_seconds;
};

inline std::string format_pace_json(const pace_json& r) {
  char buf[2048];
  const int n = std::snprintf(
      buf, sizeof(buf),
               "{\n"
               "  \"schema\": 2,\n"
               "  \"warmup_seconds_discarded\": %.3f,\n"
               "  \"frames\": %llu,\n"
               "  \"elapsed_seconds\": %.6f,\n"
               "  \"refresh_interval_ms\": %.6f,\n"
               "  \"dropped_frames\": %llu,\n"
               "  \"missed_refreshes\": %llu,\n"
               "  \"statistics_discontinuities\": %llu,\n"
               "  \"mean_ms\": %.6f,\n"
               "  \"p50_ms\": %.6f,\n"
               "  \"p99_ms\": %.6f,\n"
               "  \"max_ms\": %.6f,\n"
               "  \"cpu_mean_ms\": %.6f,\n"
               "  \"cpu_p99_ms\": %.6f,\n"
               "  \"cpu_max_ms\": %.6f,\n"
               "  \"drop_source\": \"%s\",\n"
               "  \"statistics_unavailable_frames\": %llu,\n"
               "  \"displayed_presents\": %llu,\n"
               "  \"static\": %s,\n"
               "  \"measurement_complete\": %s,\n"
               "  \"idle_elapsed_seconds\": %.6f,\n"
               "  \"idle_cpu_percent\": %.6f,\n"
               "  \"idle_presents\": %llu,\n"
               "  \"idle_input_events\": %llu,\n"
               "  \"meets_pr1_gate\": %s\n"
               "}\n",
               r.warmup_seconds,
               static_cast<unsigned long long>(r.pace.frames),
               r.pace.elapsed_seconds,
               r.pace.refresh_interval_ms,
               static_cast<unsigned long long>(r.pace.dropped_frames),
               static_cast<unsigned long long>(r.pace.missed_refreshes),
               static_cast<unsigned long long>(r.pace.statistics_discontinuities),
               r.pace.mean_ms, r.pace.p50_ms, r.pace.p99_ms, r.pace.max_ms,
               r.pace.cpu_mean_ms, r.pace.cpu_p99_ms, r.pace.cpu_max_ms,
               metal_drop_source_label(r.pace.source),
               static_cast<unsigned long long>(r.pace.statistics_unavailable_frames),
               static_cast<unsigned long long>(r.pace.displayed_presents),
               r.static_run ? "true" : "false",
               r.measurement_complete ? "true" : "false",
               r.idle.elapsed_seconds, r.idle.cpu_percent,
               static_cast<unsigned long long>(r.idle.presents),
               static_cast<unsigned long long>(r.idle.input_events),
               r.meets_gate ? "true" : "false");
  if (n < 0 || static_cast<std::size_t>(n) >= sizeof(buf)) return {};
  return std::string(buf, static_cast<std::size_t>(n));
}

inline bool write_pace_json(FILE* f, const pace_json& r) noexcept {
  if (!f) return false;
  const auto json = format_pace_json(r);
  if (json.empty()) return false;
  std::fwrite(json.data(), 1, json.size(), f);
  return std::ferror(f) == 0;
}

}  // namespace mv::gfx
