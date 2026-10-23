// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Schema-2 JSON: the Metal and Linux labs must write reports frametime will
// accept, and no platform's report may pass another platform's gate.

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

// --- PR 7: the preview -> full swap, as a gate rather than a squint ----------

namespace {

// A Windows-lab animated report that passes PR 1, plus the refinement block.
// `refine` is spliced in the way the lab writes it.
std::string windows_report_with(const std::string& refine) {
  return std::string(
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
             "  \"idle_input_events\": 0,\n") +
         refine + "  \"meets_pr1_gate\": true\n}\n";
}

std::string refine_block(const char* refinements, const char* started, const char* completed,
                         const char* cancelled, const char* frames, const char* dropped,
                         const char* shift, const char* scale) {
  return std::string("  \"still_refinements\": ") + refinements +
         ",\n  \"refine_fades_started\": " + started +
         ",\n  \"refine_fades_completed\": " + completed +
         ",\n  \"refine_fades_cancelled\": " + cancelled +
         ",\n  \"refine_fade_frames\": " + frames +
         ",\n  \"refine_fade_dropped\": " + dropped +
         ",\n  \"refine_max_edge_shift_px\": " + shift +
         ",\n  \"refine_max_scale_step\": " + scale + ",\n";
}

// One RAW opened: embedded preview, then LibRaw's frame, fading over five
// frames at 60 Hz with the view held.
std::string clean_swap() { return refine_block("1", "1", "1", "0", "5", "0", "0.0021", "1.000400"); }

}  // namespace

TEST_CASE("a clean preview -> full swap passes the no-pop gate", "[frametime][pr7][nopop]") {
  const auto parsed = mv::frametime::parse(windows_report_with(clean_swap()));
  REQUIRE(parsed);
  REQUIRE(parsed->has_refine);
  REQUIRE(parsed->refinements == 1);
  REQUIRE(mv::frametime::no_pop_gate(*parsed));
  REQUIRE(mv::frametime::passes(*parsed, false, 60.0));
}

TEST_CASE("a report with no refinement block cannot pass the no-pop gate",
          "[frametime][pr7][nopop]") {
  // The PR 1 soak on a BMP, and the Metal lab's report: both pass PR 1 and
  // neither says anything about a preview -> full swap.
  const auto pr1 = mv::frametime::parse(windows_report_with(""));
  REQUIRE(pr1);
  REQUIRE_FALSE(pr1->has_refine);
  REQUIRE(mv::frametime::passes(*pr1, false, 60.0));
  REQUIRE_FALSE(mv::frametime::no_pop_gate(*pr1));

  const auto metal = mv::frametime::parse(write_to_string(passing_metal_animated()));
  REQUIRE(metal);
  REQUIRE_FALSE(mv::frametime::no_pop_gate(*metal));
}

TEST_CASE("each shape of a pop fails the gate", "[frametime][pr7][nopop]") {
  const auto fails = [](const std::string& block) {
    const auto parsed = mv::frametime::parse(windows_report_with(block));
    REQUIRE(parsed);
    return !mv::frametime::no_pop_gate(*parsed);
  };
  // Nothing refined: the run proves nothing, so it does not pass by default.
  CHECK(fails(refine_block("0", "0", "0", "0", "0", "0", "0.0", "1.0")));
  // A refinement that never faded: a hard cut between two textures.
  CHECK(fails(refine_block("1", "0", "0", "0", "0", "0", "0.0", "1.0")));
  // A fade cut short by navigation, or one still running at the end.
  CHECK(fails(refine_block("2", "2", "1", "1", "8", "0", "0.001", "1.0002")));
  CHECK(fails(refine_block("1", "1", "0", "0", "3", "0", "0.001", "1.0002")));
  // A one-frame "fade" is a dissolve, not a fade.
  CHECK(fails(refine_block("1", "1", "1", "0", "1", "0", "0.001", "1.0002")));
  // A dropped frame inside the fade window: the swap stuttered.
  CHECK(fails(refine_block("1", "1", "1", "0", "5", "1", "0.001", "1.0002")));
  // The view jumped: a pixel and a half, and a 3 % size step.
  CHECK(fails(refine_block("1", "1", "1", "0", "5", "0", "1.5", "1.0002")));
  CHECK(fails(refine_block("1", "1", "1", "0", "5", "0", "0.001", "1.03")));
}

TEST_CASE("a half-written or impossible refinement block is malformed, not a pass",
          "[frametime][pr7][nopop]") {
  // Started but the rest of the block missing.
  const std::string partial = "  \"refine_fades_started\": 1,\n";
  REQUIRE_FALSE(mv::frametime::parse(windows_report_with(partial)));
  // Values that cannot come from the instrument.
  REQUIRE_FALSE(
      mv::frametime::parse(windows_report_with(refine_block("1", "1", "1", "0", "5", "0", "-1.0", "1.0"))));
  REQUIRE_FALSE(
      mv::frametime::parse(windows_report_with(refine_block("1", "1", "1", "0", "5", "0", "0.0", "0.5"))));
}

// --- Linux PR 1: the Wayland presentation-feedback source -------------------

namespace {

mv::gfx::presentation_pace_json passing_linux_animated() {
  mv::gfx::presentation_pace_json r;
  r.pace.elapsed_seconds = 60.0;
  r.pace.refresh_interval_ms = 16.666667;
  r.pace.frames = 3600;
  r.pace.displayed_presents = 3600;
  r.pace.mean_ms = 16.666667;
  r.pace.p50_ms = 16.666667;
  r.pace.p99_ms = 16.7;
  r.pace.max_ms = 16.7;
  r.pace.source = mv::gfx::presentation_drop_source::presentation_feedback;
  r.idle.elapsed_seconds = 60.0;
  r.idle.cpu_percent = 0.2;
  r.static_run = false;
  r.measurement_complete = true;
  r.meets_gate = true;
  return r;
}

}  // namespace

TEST_CASE("Linux schema-2 JSON round-trips and passes Linux PR 1 only",
          "[frametime][linux_pr1]") {
  const auto parsed = mv::frametime::parse(mv::gfx::format_pace_json(passing_linux_animated()));
  REQUIRE(parsed);
  REQUIRE(parsed->drop_source == "Wayland presentation feedback");
  REQUIRE(parsed->frames == 3600);
  REQUIRE(mv::frametime::passes_linux_pr1(*parsed, false, 60.0));
  REQUIRE_FALSE(mv::frametime::passes(*parsed, false, 60.0));       // DXGI required
  REQUIRE_FALSE(mv::frametime::passes_pr16(*parsed, false, 60.0));  // display link required
  // And the other platforms' authoritative reports cannot pass Linux PR 1.
  const auto metal = mv::frametime::parse(write_to_string(passing_metal_animated()));
  REQUIRE(metal);
  REQUIRE_FALSE(mv::frametime::passes_linux_pr1(*metal, false, 60.0));
}

TEST_CASE("headless Weston's report parses, is labelled weaker and fails Linux PR 1",
          "[frametime][linux_pr1]") {
  // What the smoke job under headless Weston sees: flags 0, seq 0, and a
  // cadence slower than the advertised 60 Hz.
  auto r = passing_linux_animated();
  r.pace.source = mv::gfx::presentation_drop_source::interval_heuristic;
  r.pace.displayed_presents = 0;
  r.pace.statistics_unavailable_frames = 3600;
  r.pace.mean_ms = 25.7;
  r.meets_gate = false;
  const auto parsed = mv::frametime::parse(mv::gfx::format_pace_json(r));
  REQUIRE(parsed);
  REQUIRE(parsed->drop_source == "clock intervals (weaker)");
  REQUIRE_FALSE(parsed->meets_gate);
  REQUIRE_FALSE(mv::frametime::passes_linux_pr1(*parsed, false, 60.0));
  // Even if the lab had claimed a pass, the source decides.
  r.meets_gate = true;
  r.pace.mean_ms = 16.666667;
  r.pace.displayed_presents = 3600;
  r.pace.statistics_unavailable_frames = 0;
  const auto claimed = mv::frametime::parse(mv::gfx::format_pace_json(r));
  REQUIRE(claimed);
  REQUIRE_FALSE(mv::frametime::passes_linux_pr1(*claimed, false, 60.0));
}

TEST_CASE("the Linux idle report is judged by the idle gate", "[frametime][linux_pr1]") {
  mv::gfx::presentation_pace_json r;
  r.static_run = true;
  r.measurement_complete = true;
  r.meets_gate = true;
  r.idle.elapsed_seconds = 60.0;
  r.idle.cpu_percent = 0.05;
  const auto parsed = mv::frametime::parse(mv::gfx::format_pace_json(r));
  REQUIRE(parsed);
  REQUIRE(parsed->drop_source == "no data");
  REQUIRE(mv::frametime::passes_linux_pr1(*parsed, true, 60.0));
  r.idle.presents = 1;
  const auto presented = mv::frametime::parse(mv::gfx::format_pace_json(r));
  REQUIRE(presented);
  REQUIRE_FALSE(mv::frametime::passes_linux_pr1(*presented, true, 60.0));
}

TEST_CASE("the Metal lab's report bytes do not move", "[frametime][pr16]") {
  // pace_json.h became one writer for the Metal and Linux labs (2026-10-23).
  // The Metal report is pinned to the bytes the Metal-only writer produced.
  mv::gfx::pace_json r;
  r.pace.elapsed_seconds = 60.25;
  r.pace.refresh_interval_ms = 16.666667;
  r.pace.frames = 3615;
  r.pace.displayed_presents = 3614;
  r.pace.dropped_frames = 1;
  r.pace.missed_refreshes = 2;
  r.pace.statistics_discontinuities = 3;
  r.pace.statistics_unavailable_frames = 4;
  r.pace.mean_ms = 16.7;
  r.pace.p50_ms = 16.65;
  r.pace.p99_ms = 17.25;
  r.pace.max_ms = 33.4;
  r.pace.cpu_mean_ms = 0.5;
  r.pace.cpu_p99_ms = 0.95;
  r.pace.cpu_max_ms = 1.25;
  r.pace.source = mv::gfx::metal_drop_source::display_link;
  r.idle.elapsed_seconds = 60.5;
  r.idle.cpu_percent = 0.125;
  r.idle.presents = 5;
  r.idle.input_events = 6;
  r.static_run = true;
  r.measurement_complete = true;
  r.meets_gate = false;
  r.still_first_pixel_s = 0.25;
  r.still_full_s = 0.5;
  const char* expected =
      "{\n"
      "  \"schema\": 2,\n"
      "  \"warmup_seconds_discarded\": 1.000,\n"
      "  \"frames\": 3615,\n"
      "  \"elapsed_seconds\": 60.250000,\n"
      "  \"refresh_interval_ms\": 16.666667,\n"
      "  \"dropped_frames\": 1,\n"
      "  \"missed_refreshes\": 2,\n"
      "  \"statistics_discontinuities\": 3,\n"
      "  \"mean_ms\": 16.700000,\n"
      "  \"p50_ms\": 16.650000,\n"
      "  \"p99_ms\": 17.250000,\n"
      "  \"max_ms\": 33.400000,\n"
      "  \"cpu_mean_ms\": 0.500000,\n"
      "  \"cpu_p99_ms\": 0.950000,\n"
      "  \"cpu_max_ms\": 1.250000,\n"
      "  \"drop_source\": \"Metal display-link\",\n"
      "  \"statistics_unavailable_frames\": 4,\n"
      "  \"displayed_presents\": 3614,\n"
      "  \"static\": true,\n"
      "  \"measurement_complete\": true,\n"
      "  \"idle_elapsed_seconds\": 60.500000,\n"
      "  \"idle_cpu_percent\": 0.125000,\n"
      "  \"idle_presents\": 5,\n"
      "  \"idle_input_events\": 6,\n"
      "  \"still_first_pixel_s\": 0.250,\n"
      "  \"still_full_s\": 0.500,\n"
      "  \"meets_pr1_gate\": false\n"
      "}\n";
  REQUIRE(mv::gfx::format_pace_json(r) == expected);
}
