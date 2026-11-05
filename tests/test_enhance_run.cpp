// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// One-click Enhance (docs/design/29 "One click"): the host's half. The glow
// over the photo follows the run (on while it works, off the moment it ends,
// never for a download alone), the tool bar's band shows while a run has a
// line, and a result is opened once, only for a run this host saw working.
#include <catch2/catch_test_macros.hpp>

#include "shell/canvas_tool.h"
#include "shell/enhance_run.h"
#include "shell/tool_overlay.h"

using namespace mv::shell;

namespace {

enhance_run_report at(enhance_run_state s, std::uint64_t id, std::string result = {}) {
  enhance_run_report r;
  r.state = s;
  r.id = id;
  r.result = std::move(result);
  return r;
}

}  // namespace

TEST_CASE("enhance run: the glow and the band follow the run", "[enhance-run]") {
  enhance_run_follow f;
  // Idle: nothing.
  auto s = f.update(at(enhance_run_state::idle, 0), 1000);
  CHECK_FALSE(s.changed);
  CHECK(f.glow_on_ms == 0);
  CHECK_FALSE(f.band);
  // Downloading its model first: the bar's line, no glow yet.
  s = f.update(at(enhance_run_state::downloading, 1), 1100);
  CHECK(s.changed);
  CHECK(f.band);
  CHECK(f.glow_on_ms == 0);
  // Working: the glow comes on, and stays on (progress changes nothing).
  s = f.update(at(enhance_run_state::running, 2), 1200);
  CHECK(s.changed);
  CHECK(f.glow_on_ms == 1200);
  s = f.update(at(enhance_run_state::running, 2), 1300);
  CHECK_FALSE(s.changed);
  CHECK(f.glow_on_ms == 1200);
  // Failed: the glow goes at once (fading from then), the reason stays in the bar.
  s = f.update(at(enhance_run_state::failed, 2), 1400);
  CHECK(s.changed);
  CHECK(f.glow_on_ms == 0);
  CHECK(f.glow_off_ms == 1400);
  CHECK(f.band);
  CHECK(s.open.empty());
  // Dismissed: the band goes too.
  s = f.update(at(enhance_run_state::idle, 2), 1500);
  CHECK(s.changed);
  CHECK_FALSE(f.band);
}

TEST_CASE("enhance run: a result is opened once, only for a run seen working", "[enhance-run]") {
  enhance_run_follow f;
  // A tree read after the fact (the tab opened on a photo whose run had
  // finished): never opened.
  auto s = f.update(at(enhance_run_state::done, 3, "/p/IMG-enhance.jpg"), 10);
  CHECK(s.open.empty());
  // A run seen working, then done: opened once.
  (void)f.update(at(enhance_run_state::running, 4), 20);
  s = f.update(at(enhance_run_state::done, 4, "/p/IMG-enhance (2).jpg"), 30);
  CHECK(s.open == "/p/IMG-enhance (2).jpg");
  s = f.update(at(enhance_run_state::done, 4, "/p/IMG-enhance (2).jpg"), 40);
  CHECK(s.open.empty());
  // Cancelled or failed: nothing to open.
  (void)f.update(at(enhance_run_state::running, 5), 50);
  s = f.update(at(enhance_run_state::cancelled, 5), 60);
  CHECK(s.open.empty());
  // Done with nothing written (no result): nothing to open.
  (void)f.update(at(enhance_run_state::running, 6), 70);
  s = f.update(at(enhance_run_state::done, 6), 80);
  CHECK(s.open.empty());
  // A Photos-library result is opened by its key.
  (void)f.update(at(enhance_run_state::running, 7), 90);
  s = f.update(at(enhance_run_state::done, 7, "photos:ABC"), 100);
  CHECK(s.open == "photos:ABC");
  // The photo went away under a run: no glow, no band, and its result is
  // not opened later.
  (void)f.update(at(enhance_run_state::running, 8), 110);
  CHECK(f.reset(120));
  CHECK(f.glow_on_ms == 0);
  CHECK_FALSE(f.band);
  s = f.update(at(enhance_run_state::done, 8, "/p/x.jpg"), 130);
  CHECK(s.open.empty());
}

TEST_CASE("enhance run: Windows' wire", "[enhance-run]") {
  enhance_run_report r;
  REQUIRE(parse_enhance_run("2\t17\tC:\\p\\IMG-enhance.jpg", r));
  CHECK(r.state == enhance_run_state::done);
  CHECK(r.id == 17);
  CHECK(r.result == "C:\\p\\IMG-enhance.jpg");
  REQUIRE(parse_enhance_run("1\t18\t", r));
  CHECK(r.state == enhance_run_state::running);
  CHECK(r.result.empty());
  CHECK_FALSE(parse_enhance_run("", r));
  CHECK_FALSE(parse_enhance_run("9\t1\t", r));
  CHECK_FALSE(parse_enhance_run("x\t1\t", r));
  CHECK_FALSE(parse_enhance_run("1\t1", r));
  CHECK(enhance_run_state_from("downloading") == enhance_run_state::downloading);
  CHECK(enhance_run_state_from("running") == enhance_run_state::running);
  CHECK(enhance_run_state_from("whatever") == enhance_run_state::idle);
}

TEST_CASE("enhance run: the glow presents only while it moves", "[enhance-run]") {
  static_assert(kWorkGlowFade == kWorkGlowFadeMs, "tool_overlay.h and enhance_run.h fade alike");
  edit_view v;
  CHECK_FALSE(work_glow_live(v, 1000, false));
  CHECK_FALSE(tool_overlay_wanted(v));
  // Through the host's tool view, as the labs get it.
  tool_view_in in;
  in.work_glow_on_ms = 500;
  apply_tool(v, in, {}, {});
  CHECK(v.work_glow_on_ms == 500);
  CHECK(tool_overlay_wanted(v));
  CHECK(work_glow_live(v, 1000, false));
  // Reduce Motion: drawn still, never animated (0 presents while it shows).
  CHECK_FALSE(work_glow_live(v, 1000, true));
  // Off: it fades for kWorkGlowFade, then nothing presents.
  in.work_glow_on_ms = 0;
  in.work_glow_off_ms = 2000;
  apply_tool(v, in, {}, {});
  CHECK(work_glow_live(v, 2000 + kWorkGlowFade - 1, false));
  CHECK_FALSE(work_glow_live(v, 2000 + kWorkGlowFade, false));
  CHECK_FALSE(work_glow_live(v, 2100, true));
  // Crop mode draws its own overlay, never the glow over it.
  v.crop_overlay = true;
  CHECK_FALSE(tool_overlay_wanted(v));
}
