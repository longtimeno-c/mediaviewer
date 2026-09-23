// SPDX-License-Identifier: GPL-2.0-or-later
// Schema-2 frame-time report parser. No DXGI, no Metal — both present labs
// write the same keys. Unit tests cover Metal drop_source strings so a Darwin
// report cannot fail to parse on a Windows box.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>

namespace mv::frametime {

struct report {
  std::uint64_t frames = 0;
  std::uint64_t dropped_frames = 0;
  std::uint64_t missed_refreshes = 0;
  std::uint64_t displayed_presents = 0;
  std::uint64_t statistics_discontinuities = 0;
  std::uint64_t statistics_unavailable_frames = 0;
  double elapsed_seconds = 0.0;
  double refresh_interval_ms = 0.0;
  double mean_ms = 0.0;
  double p50_ms = 0.0;
  double p99_ms = 0.0;
  double max_ms = 0.0;
  double idle_elapsed_seconds = 0.0;
  double idle_cpu_percent = -1.0;
  std::uint64_t idle_presents = 0;
  std::uint64_t idle_input_events = 0;
  std::string drop_source;
  bool static_run = false;
  bool complete = false;
  bool meets_gate = false;

  // PR 7's no-pop instrument. Only the Windows lab writes these (the Metal lab
  // decodes nothing yet), so they are optional at parse time and default to
  // values that cannot pass no_pop_gate: a report without them proves nothing
  // about a preview → full swap and must not read as if it did.
  bool has_refine = false;
  std::uint64_t refinements = 0;
  std::uint64_t refine_fades_started = 0;
  std::uint64_t refine_fades_completed = 0;
  std::uint64_t refine_fades_cancelled = 0;
  std::uint64_t refine_fade_frames = 0;
  std::uint64_t refine_fade_dropped = 0;
  double refine_max_edge_shift_px = 0.0;
  double refine_max_scale_step = 1.0;
};

inline std::optional<std::string> field(const std::string& json, const char* key) {
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

inline bool scalar(const std::string& json, const char* key, double& out) {
  const auto value = field(json, key);
  if (!value) return false;
  char* end = nullptr;
  out = std::strtod(value->c_str(), &end);
  return end != value->c_str() && *end == '\0' && std::isfinite(out);
}

inline bool count(const std::string& json, const char* key, std::uint64_t& out) {
  double value = 0.0;
  if (!scalar(json, key, value) || value < 0.0 || value >= 9007199254740992.0 ||
      std::floor(value) != value)
    return false;
  out = static_cast<std::uint64_t>(value);
  return true;
}

inline bool boolean(const std::string& json, const char* key, bool& out) {
  const auto value = field(json, key);
  if (!value || (*value != "true" && *value != "false")) return false;
  out = *value == "true";
  return true;
}

inline bool known_drop_source(const std::string& quoted) {
  return quoted == "\"DXGI frame statistics\"" || quoted == "\"QPC intervals (weaker)\"" ||
         quoted == "\"Metal display-link\"" || quoted == "\"clock intervals (weaker)\"" ||
         quoted == "\"no data\"";
}

inline std::optional<report> parse(const std::string& json) {
  report r;
  double schema = 0.0;
  if (!scalar(json, "schema", schema) || schema != 2.0 ||
      !scalar(json, "elapsed_seconds", r.elapsed_seconds) ||
      !scalar(json, "refresh_interval_ms", r.refresh_interval_ms) ||
      !scalar(json, "mean_ms", r.mean_ms) || !scalar(json, "p50_ms", r.p50_ms) ||
      !scalar(json, "p99_ms", r.p99_ms) || !scalar(json, "max_ms", r.max_ms) ||
      !count(json, "frames", r.frames) || !count(json, "dropped_frames", r.dropped_frames) ||
      !count(json, "missed_refreshes", r.missed_refreshes) ||
      !count(json, "displayed_presents", r.displayed_presents) ||
      !count(json, "statistics_discontinuities", r.statistics_discontinuities) ||
      !count(json, "statistics_unavailable_frames", r.statistics_unavailable_frames) ||
      !scalar(json, "idle_elapsed_seconds", r.idle_elapsed_seconds) ||
      !scalar(json, "idle_cpu_percent", r.idle_cpu_percent) ||
      !count(json, "idle_presents", r.idle_presents) ||
      !count(json, "idle_input_events", r.idle_input_events) ||
      !boolean(json, "static", r.static_run) ||
      !boolean(json, "measurement_complete", r.complete) ||
      !boolean(json, "meets_pr1_gate", r.meets_gate))
    return std::nullopt;
  const auto source = field(json, "drop_source");
  if (!source || !known_drop_source(*source)) return std::nullopt;
  r.drop_source = source->substr(1, source->size() - 2);
  // All of the no-pop block, or none of it. A half-written block is a malformed
  // report, not a partial pass.
  const bool any_refine = field(json, "still_refinements").has_value() ||
                          field(json, "refine_fades_started").has_value() ||
                          field(json, "refine_max_edge_shift_px").has_value();
  if (any_refine) {
    if (!count(json, "still_refinements", r.refinements) ||
        !count(json, "refine_fades_started", r.refine_fades_started) ||
        !count(json, "refine_fades_completed", r.refine_fades_completed) ||
        !count(json, "refine_fades_cancelled", r.refine_fades_cancelled) ||
        !count(json, "refine_fade_frames", r.refine_fade_frames) ||
        !count(json, "refine_fade_dropped", r.refine_fade_dropped) ||
        !scalar(json, "refine_max_edge_shift_px", r.refine_max_edge_shift_px) ||
        !scalar(json, "refine_max_scale_step", r.refine_max_scale_step) ||
        r.refine_max_edge_shift_px < 0.0 || r.refine_max_scale_step < 1.0) {
      return std::nullopt;
    }
    r.has_refine = true;
  }
  return r;
}

inline bool idle_gate(const report& r) {
  return std::isfinite(r.idle_elapsed_seconds) && r.idle_elapsed_seconds >= 60.0 &&
         std::isfinite(r.idle_cpu_percent) && r.idle_cpu_percent >= 0.0 &&
         r.idle_cpu_percent <= 1.0 && r.idle_presents == 0 && r.idle_input_events == 0;
}

inline bool cadence_gate(const report& r, const char* required_source) {
  if (!std::isfinite(r.refresh_interval_ms) || r.refresh_interval_ms <= 0.0 ||
      !std::isfinite(r.elapsed_seconds) || r.elapsed_seconds < 60.0 ||
      !std::isfinite(r.mean_ms) || !std::isfinite(r.p50_ms) || !std::isfinite(r.max_ms))
    return false;
  if (r.drop_source != required_source) return false;
  const double expected = r.elapsed_seconds * 1000.0 / r.refresh_interval_ms;
  return r.frames >= expected * 0.98 && r.frames <= expected * 1.02 &&
         r.displayed_presents >= expected * 0.98 && r.displayed_presents <= expected * 1.02 &&
         std::abs(r.mean_ms - r.refresh_interval_ms) <= r.refresh_interval_ms * 0.02 &&
         std::abs(r.p50_ms - r.refresh_interval_ms) <= r.refresh_interval_ms * 0.02 + 0.05 &&
         r.max_ms <= r.refresh_interval_ms * 2.0 && r.dropped_frames == 0 &&
         r.missed_refreshes == 0 && r.statistics_discontinuities == 0 &&
         r.statistics_unavailable_frames == 0;
}

inline bool passes(const report& r, bool static_run, double seconds) {
  return r.complete && r.meets_gate && r.static_run == static_run &&
         (static_run ? r.idle_elapsed_seconds : r.elapsed_seconds) >= seconds &&
         (static_run ? idle_gate(r) : cadence_gate(r, "DXGI frame statistics"));
}

// PR 7: "the full decode replaces it without a visible pop". The measurable
// half, over a soak that opened a still with a preview (a RAW's embedded JPEG,
// a JPEG's DCT 1/4). Asserting, in order:
//   * a preview → full swap actually happened — a run that never refined
//     cannot pass by having nothing to measure;
//   * every fade reached alpha 1 and none was cut short (a cut fade IS a pop);
//   * the fade was drawn over more than one frame, so it is a fade and not a
//     one-frame dissolve that happens to satisfy the counters;
//   * no frame was dropped inside a fade window;
//   * the view did not jump: a corner of the picture moved under a pixel, and
//     its on-screen size changed by under 1 % (the preview and the full decode
//     differ by a pixel or two of aspect on some RAWs, which is not a pop).
inline bool no_pop_gate(const report& r) {
  return r.has_refine && r.refinements >= 1 && r.refine_fades_started >= 1 &&
         r.refine_fades_completed == r.refine_fades_started && r.refine_fades_cancelled == 0 &&
         r.refine_fade_frames >= r.refine_fades_started * 2 && r.refine_fade_dropped == 0 &&
         std::isfinite(r.refine_max_edge_shift_px) && r.refine_max_edge_shift_px <= 1.0 &&
         std::isfinite(r.refine_max_scale_step) && r.refine_max_scale_step <= 1.01;
}

inline bool passes_pr16(const report& r, bool static_run, double seconds) {
  return r.complete && r.meets_gate && r.static_run == static_run &&
         (static_run ? r.idle_elapsed_seconds : r.elapsed_seconds) >= seconds &&
         (static_run ? idle_gate(r) : cadence_gate(r, "Metal display-link"));
}

}  // namespace mv::frametime
