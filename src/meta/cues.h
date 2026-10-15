// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #406 — a clip's own text, for Local search (docs/design/17 "Cues"): its chapter
// titles and its text subtitle cues (mov_text, SubRip, ASS / SSA, WebVTT).
//
// A demux through the FFmpeg the viewer already links: chapters come from the
// header, and a subtitle cue is read from its packet as text, so no subtitle
// decoder is needed. Bitmap subtitles (DVD, PGS) are pictures and are skipped;
// a cue whose bytes are not text is skipped. Worker thread only; nothing read
// here leaves the machine (rule 6).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "core/result.h"

namespace mv::meta {

enum class cue_kind : std::uint8_t { chapter = 1, subtitle = 2 };

struct cue {
  std::int64_t start_ms = 0;  // on the player's timeline (container origin removed)
  std::int64_t end_ms = 0;
  cue_kind kind = cue_kind::subtitle;
  std::string text;           // one line, UTF-8, markup removed
};

struct cue_limits {
  std::int64_t start_ms = 0;               // nothing that starts before this (a resume)
  std::int64_t max_ms = 2 * 3600 * 1000;   // nothing that starts after this
  std::uint32_t max_cues = 8000;
};

// Calls `visit` for each cue in start order (chapters interleaved with the
// subtitles); `visit` returning false stops the read (status::cancelled). A
// file with neither chapters nor a text subtitle stream returns at once,
// having read only the header. status::io when the file cannot be opened.
[[nodiscard]] expected read_cues(std::string_view utf8_path, const cue_limits& limits,
                                 const std::function<bool(const cue&)>& visit);

// A subtitle packet's text as one clean line (exposed for the tests):
// `codec` is "mov_text", "subrip", "ass", "webvtt" or "text". Empty when the
// bytes are not UTF-8 text or hold nothing but markup.
[[nodiscard]] std::string cue_text(std::string_view codec, std::string_view packet);

}  // namespace mv::meta
