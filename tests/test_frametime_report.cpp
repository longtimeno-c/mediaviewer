// SPDX-License-Identifier: GPL-2.0-or-later
// Schema-2 JSON: the Metal lab must write a report frametime will accept, and
// a DXGI report must not pass the PR 16 gate.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "gfx/pace_json.h"
#include "gfx/present_policy.h"

#include "frametime/report.h"

namespace {

std::string write_to_string(const mv::gfx::pace_json& r) {
  auto json = mv::gfx::format_pace_json(r);
  REQUIRE_FALSE(json.empty());
  return json;
}

mv::gfx::pace_json passing_metal_animated() {
  mv::gfx::pace_json r;
  r.pace.elapsed_seconds = 60.0;
  r.pace.refresh_interval_ms = 16.666667;
  r.pace.frames = 3600;
  r.pace.displayed_presents = 3600;
  r.pace.mean_ms = 16.666667;
  r.pace.p50_ms = 16.666667;
  r.pace.p99_ms = 16.7;
  r.pace.max_ms = 16.7;
  r.pace.source = mv::gfx::metal_drop_source::display_link;
  r.idle.elapsed_seconds = 60.0;
  r.idle.cpu_percent = 0.2;
  r.static_run = false;
  r.measurement_complete = true;
  r.meets_gate = true;
  r.warmup_seconds = mv::gfx::k_warmup_seconds;
  return r;
}

}  // namespace

TEST_CASE("Metal schema-2 JSON round-trips and passes PR 16", "[frametime][pr16]") {
  const auto json = write_to_string(passing_metal_animated());
  const auto parsed = mv::frametime::parse(json);
  REQUIRE(parsed);
  REQUIRE(parsed->drop_source == "Metal display-link");
  REQUIRE(parsed->frames == 3600);
  REQUIRE(parsed->complete);
  REQUIRE(mv::frametime::passes_pr16(*parsed, false, 60.0));
  REQUIRE_FALSE(mv::frametime::passes(*parsed, false, 60.0));  // DXGI source required
}

TEST_CASE("a DXGI drop_source cannot pass PR 16", "[frametime][pr16]") {
  const char* dxgi =
      "{\n"
      "  \"schema\": 2,\n"
      "  \"warmup_seconds_discarded\": 1.000,\n"
      "  \"frames\": 3600,\n"
      "  \"elapsed_seconds\": 60.000000,\n"
      "  \"refresh_interval_ms\": 16.666667,\n"
      "  \"dropped_frames\": 0,\n"
      "  \"missed_refreshes\": 0,\n"
      "  \"statistics_discontinuities\": 0,\n"
      "  \"mean_ms\": 16.666667,\n"
      "  \"p50_ms\": 16.666667,\n"
      "  \"p99_ms\": 16.700000,\n"
      "  \"max_ms\": 16.700000,\n"
      "  \"cpu_mean_ms\": 0.4,\n"
      "  \"cpu_p99_ms\": 0.8,\n"
      "  \"cpu_max_ms\": 1.0,\n"
      "  \"drop_source\": \"DXGI frame statistics\",\n"
      "  \"statistics_unavailable_frames\": 0,\n"
      "  \"displayed_presents\": 3600,\n"
      "  \"static\": false,\n"
      "  \"measurement_complete\": true,\n"
      "  \"idle_elapsed_seconds\": 60.0,\n"
      "  \"idle_cpu_percent\": 0.2,\n"
      "  \"idle_presents\": 0,\n"
      "  \"idle_input_events\": 0,\n"
      "  \"meets_pr1_gate\": true\n"
      "}\n";
  const auto parsed = mv::frametime::parse(dxgi);
  REQUIRE(parsed);
  REQUIRE(mv::frametime::passes(*parsed, false, 60.0));
  REQUIRE_FALSE(mv::frametime::passes_pr16(*parsed, false, 60.0));
}

TEST_CASE("clock-interval Metal source is parsed and fails the gate", "[frametime][pr16]") {
  auto r = passing_metal_animated();
  r.pace.source = mv::gfx::metal_drop_source::interval_heuristic;
  const auto json = write_to_string(r);
  const auto parsed = mv::frametime::parse(json);
  REQUIRE(parsed);
  REQUIRE(parsed->drop_source == "clock intervals (weaker)");
  REQUIRE_FALSE(mv::frametime::passes_pr16(*parsed, false, 60.0));
}

TEST_CASE("an unknown drop_source is rejected, not zeroed", "[frametime][pr16]") {
  const char* bad =
      "{ \"schema\": 2, \"elapsed_seconds\": 60, \"refresh_interval_ms\": 16.6,"
      " \"mean_ms\": 16.6, \"p50_ms\": 16.6, \"p99_ms\": 16.6, \"max_ms\": 16.6,"
      " \"frames\": 3600, \"dropped_frames\": 0, \"missed_refreshes\": 0,"
      " \"displayed_presents\": 3600, \"statistics_discontinuities\": 0,"
      " \"statistics_unavailable_frames\": 0, \"idle_elapsed_seconds\": 60,"
      " \"idle_cpu_percent\": 0.1, \"idle_presents\": 0, \"idle_input_events\": 0,"
      " \"static\": false, \"measurement_complete\": true, \"meets_pr1_gate\": true,"
      " \"drop_source\": \"made up\" }";
  REQUIRE_FALSE(mv::frametime::parse(bad));
}

TEST_CASE("idle JSON with a present fails the idle gate", "[frametime][pr16]") {
  mv::gfx::pace_json r;
  r.static_run = true;
  r.measurement_complete = true;
  r.meets_gate = true;
  r.idle.elapsed_seconds = 60.0;
  r.idle.cpu_percent = 0.3;
  r.idle.presents = 1;
  r.pace.source = mv::gfx::metal_drop_source::none;
  const auto parsed = mv::frametime::parse(write_to_string(r));
  REQUIRE(parsed);
  REQUIRE_FALSE(mv::frametime::passes_pr16(*parsed, true, 60.0));
}

TEST_CASE("duplicate keys are rejected", "[frametime][pr16]") {
  const char* dup =
      "{ \"schema\": 2, \"schema\": 2, \"elapsed_seconds\": 60,"
      " \"refresh_interval_ms\": 16.6, \"mean_ms\": 16.6, \"p50_ms\": 16.6,"
      " \"p99_ms\": 16.6, \"max_ms\": 16.6, \"frames\": 3600, \"dropped_frames\": 0,"
      " \"missed_refreshes\": 0, \"displayed_presents\": 3600,"
      " \"statistics_discontinuities\": 0, \"statistics_unavailable_frames\": 0,"
      " \"idle_elapsed_seconds\": 60, \"idle_cpu_percent\": 0.1, \"idle_presents\": 0,"
      " \"idle_input_events\": 0, \"static\": false, \"measurement_complete\": true,"
      " \"meets_pr1_gate\": true, \"drop_source\": \"Metal display-link\" }";
  REQUIRE_FALSE(mv::frametime::parse(dup));
}
