// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// shell/trim_state.h and the PR 13 / 14 rows of the command table.
#include "catch_compat.h"

#include "shell/commands.h"
#include "shell/key_router.h"
#include "shell/trim_state.h"

using namespace mv::shell;
namespace clip = mv::edit::clip;

namespace {
constexpr std::int64_t kS = 1'000'000'000;
}

TEST_CASE("trim markers, ranges and the keyframe walk", "[trim][pr13]") {
  trim_state t;
  t.arm("a.mp4", 10 * kS);
  CHECK(t.armed());
  CHECK_FALSE(t.has_marker());
  CHECK(t.requested().in_ns == 0);
  CHECK(t.requested().out_ns == 10 * kS);

  t.mark_in(3 * kS + 100);
  t.mark_out(7 * kS);
  CHECK(t.in_ns() == 3 * kS + 100);
  // Before the index lands, the keyframe range is the requested one.
  CHECK(t.keyframe_range().in_ns == 3 * kS + 100);
  CHECK(t.label().find("reading keyframes") != std::string::npos);

  t.set_index("other.mp4", {0, 5 * kS}, 10 * kS);  // stale answer: ignored
  CHECK_FALSE(t.index_ready());
  t.set_index("a.mp4", {0, 2 * kS, 4 * kS, 6 * kS, 8 * kS}, 10 * kS);
  REQUIRE(t.index_ready());
  CHECK(t.keyframe_range().in_ns == 2 * kS);
  CHECK(t.keyframe_range().out_ns == 8 * kS);
  CHECK(t.label() == "In 0:03.000 · Out 0:07.000 · keyframe cut 0:02.000–0:08.000");

  CHECK(t.prev_keyframe(4 * kS) == 2 * kS);  // strictly before
  CHECK(t.next_keyframe(4 * kS) == 6 * kS);  // strictly after
  CHECK(t.prev_keyframe(0) == 0);            // none: stays
  CHECK(t.next_keyframe(9 * kS) == 9 * kS);

  // A marker crossing the other clears the other.
  t.mark_in(8 * kS);
  CHECK(t.out_ns() == -1);
  t.mark_out(1 * kS);
  CHECK(t.in_ns() == -1);
  CHECK(t.out_ns() == 1 * kS);

  const auto r = t.request(clip::op::trim_reencode);
  CHECK(r.kind == clip::op::trim_reencode);
  CHECK(r.source == "a.mp4");
  CHECK(r.in_ns == 0);
  CHECK(r.out_ns == 1 * kS);

  // Leaving and re-entering keeps the markers; another clip starts clean.
  t.disarm();
  t.arm("a.mp4", 0);
  CHECK(t.out_ns() == 1 * kS);
  t.arm("b.mp4", 4 * kS);
  CHECK_FALSE(t.has_marker());
  CHECK_FALSE(t.index_ready());
  CHECK(t.duration_ns() == 4 * kS);
}

TEST_CASE("preview only while armed", "[trim][pr13]") {
  trim_state t;
  CHECK_FALSE(t.toggle_preview());
  t.arm("a.mp4", kS);
  CHECK(t.toggle_preview());
  CHECK_FALSE(t.toggle_preview());
  CHECK(t.toggle_preview());
  t.disarm();
  CHECK_FALSE(t.previewing());
}

TEST_CASE("clip tools answers become requests", "[trim][pr14]") {
  clip::request r;
  CHECK(clip_tool_request(pack_clip_choice(clip::op::rotate, 2), "a.mp4", 0, nullptr, r));
  CHECK(r.kind == clip::op::rotate);
  CHECK(r.rotate_degrees == 270);
  CHECK(clip_tool_request(pack_clip_choice(clip::op::frame, 2), "a.mp4", 5 * kS, nullptr, r));
  CHECK(r.frame == clip::frame_format::jpeg);
  CHECK(r.in_ns == 5 * kS);
  CHECK(clip_tool_request(pack_clip_choice(clip::op::remux, 2), "a.mp4", 0, nullptr, r));
  CHECK(r.remux == clip::remux_target::mkv);
  // Range ops need markers on this clip.
  CHECK_FALSE(clip_tool_request(pack_clip_choice(clip::op::remove_middle, 0), "a.mp4", 0, nullptr, r));
  trim_state t;
  t.arm("a.mp4", 10 * kS);
  t.mark_in(2 * kS);
  CHECK_FALSE(clip_tool_request(pack_clip_choice(clip::op::remove_middle, 0), "b.mp4", 0, &t, r));
  CHECK(clip_tool_request(pack_clip_choice(clip::op::remove_middle, 0), "a.mp4", 0, &t, r));
  CHECK(r.in_ns == 2 * kS);
  CHECK(r.out_ns == -1);
  // An animation with no markers is a short one from the playhead.
  CHECK(clip_tool_request(pack_clip_choice(clip::op::animation, 1), "c.mp4", 3 * kS, &t, r));
  CHECK(r.in_ns == 3 * kS);
  CHECK(r.out_ns == 8 * kS);
  CHECK_FALSE(clip_tool_request(0, "a.mp4", 0, nullptr, r));
  CHECK_FALSE(clip_tool_request(42, "a.mp4", 0, nullptr, r));
}

TEST_CASE("trim mode layers over video", "[trim][keys][pr13]") {
  key_router router;
  view_state s;
  s.item = item_kind::clip;
  CHECK(resolve_mode(s) == mode::video);
  // Ctrl+T arms; `[` does nothing on a clip until then.
  CHECK(router.on_key({char_key('T'), mod_ctrl}, s).command == command_id::trim_mode);
  CHECK(router.on_key({char_key('['), mod_none}, s).command == command_id::none);
  s.trim = true;
  CHECK(resolve_mode(s) == mode::trim);
  CHECK(router.on_key({char_key('['), mod_none}, s).command == command_id::trim_in);
  CHECK(router.on_key({char_key(']'), mod_none}, s).command == command_id::trim_out);
  CHECK(router.on_key({key::enter, mod_none}, s).command == command_id::trim_keyframe);
  CHECK(router.on_key({key::enter, mod_shift}, s).command == command_id::trim_reencode);
  CHECK(router.on_key({key::left, mod_ctrl}, s).command == command_id::keyframe_prev);
  CHECK(router.on_key({char_key('P'), mod_none}, s).command == command_id::trim_preview);
  // Delete clears markers in trim; it never trashes the clip being trimmed.
  CHECK(router.on_key({key::del, mod_none}, s).command == command_id::trim_clear);
  CHECK(router.on_key({key::backspace, mod_none}, s).command == command_id::trim_clear);
  // The transport still works underneath.
  CHECK(router.on_key({key::space, mod_none}, s).command == command_id::play_pause);
  CHECK(router.on_key({char_key('J'), mod_none}, s).command == command_id::jump_back);
  // Esc disarms trim before anything else on the canvas.
  const route esc = router.on_key({key::escape, mod_none}, s);
  CHECK(esc.command == command_id::back);
  CHECK(esc.back == back_target::trim);
  // Trim is a clip mode only.
  s.item = item_kind::still;
  CHECK(resolve_mode(s) == mode::browse);
  CHECK(router.on_key({char_key('['), mod_none}, s).command == command_id::rotate_ccw);
  // In video (not trim) Ctrl+Left stays the folder walk.
  s.item = item_kind::clip;
  s.trim = false;
  CHECK(router.on_key({key::left, mod_ctrl}, s).command == command_id::folder_prev);
  CHECK(router.on_key({char_key('S'), mod_ctrl}, s).command == command_id::clip_tools);
  CHECK(router.on_key({char_key('J'), mod_ctrl}, s).command == command_id::jobs_pane);
}
