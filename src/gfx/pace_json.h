// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Schema-2 frame-time JSON used by the present labs and by frametime.
//
// The Windows lab writes this from DXGI statistics; the Metal lab writes the
// same keys with drop_source "Metal display-link", and the Linux lab with
// "Wayland presentation feedback". A parser that requires DXGI-only source
// strings cannot certify Mac PR 1 or Linux PR 1. The writer is one template over
// the pacer's stats, so the key list cannot drift between the two portable
// labs; drop_source_label() is the only thing a pacer adds.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "gfx/metal_pacer.h"
#include "gfx/presentation_pacer.h"
#include "gfx/present_policy.h"

namespace mv::gfx {

[[nodiscard]] inline const char* drop_source_label(metal_drop_source s) noexcept {
  return metal_drop_source_label(s);
}
[[nodiscard]] inline const char* drop_source_label(presentation_drop_source s) noexcept {
  return presentation_drop_source_label(s);
}

template <class PaceStats, class IdleStats>
struct basic_pace_json {
  PaceStats pace;
  IdleStats idle;
  bool static_run = false;
  bool measurement_complete = false;
  bool meets_gate = false;
  double warmup_seconds = k_warmup_seconds;
  // Seconds from the render thread's start to the opened still's first pixel
  // and to its full-resolution image; -1 when none landed (the same fields
  // and clock as the Windows lab's report, read by tools/perf).
  double still_first_pixel_s = -1.0;
  double still_full_s = -1.0;
};

// The Metal lab's report (Mac PR 1) and the Linux lab's (Linux PR 1).
using pace_json = basic_pace_json<metal_pace_stats, metal_idle_stats>;
using presentation_pace_json = basic_pace_json<presentation_pace_stats, presentation_idle_stats>;

template <class PaceStats, class IdleStats>
inline std::string format_pace_json(const basic_pace_json<PaceStats, IdleStats>& r) {
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
               "  \"still_first_pixel_s\": %.3f,\n"
               "  \"still_full_s\": %.3f,\n"
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
               drop_source_label(r.pace.source),
               static_cast<unsigned long long>(r.pace.statistics_unavailable_frames),
               static_cast<unsigned long long>(r.pace.displayed_presents),
               r.static_run ? "true" : "false",
               r.measurement_complete ? "true" : "false",
               r.idle.elapsed_seconds, r.idle.cpu_percent,
               static_cast<unsigned long long>(r.idle.presents),
               static_cast<unsigned long long>(r.idle.input_events),
               r.still_first_pixel_s, r.still_full_s,
               r.meets_gate ? "true" : "false");
  if (n < 0 || static_cast<std::size_t>(n) >= sizeof(buf)) return {};
  return std::string(buf, static_cast<std::size_t>(n));
}

template <class PaceStats, class IdleStats>
inline bool write_pace_json(FILE* f, const basic_pace_json<PaceStats, IdleStats>& r) noexcept {
  if (!f) return false;
  const auto json = format_pace_json(r);
  if (json.empty()) return false;
  std::fwrite(json.data(), 1, json.size(), f);
  return std::ferror(f) == 0;
}

}  // namespace mv::gfx
