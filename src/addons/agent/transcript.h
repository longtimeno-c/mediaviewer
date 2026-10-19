// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A clip's speech, laid on the timeline (docs/design/29 "Transcript"): the input
// for "cut the ums", "tighten the pauses", "find the line about pricing".
//
// The index keeps Whisper's phrases (segments, file time). For the range of a
// file a timeline item plays, this maps each phrase onto the timeline, refines
// its edges against the file's loudness (Whisper's segment times can be off
// by most of a pause), splits it into words whose times are estimated
// (shared out by length, each boundary moved to the quietest point near it),
// and finds the silences from the loudness itself: the cut candidates that
// do not depend on the transcript at all. Every time it gives is on the
// sequence's frame grid. Portable C++.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "addons/agent/environment.h"
#include "nle/rational.h"
#include "nle/timeline.h"

namespace mv::agent {

struct word {
  std::string text;
  nle::rational at, end;  // timeline
};

struct phrase {
  nle::rational at, end;  // timeline
  std::int64_t file_start_ms = 0, file_end_ms = 0;
  std::string text;
  bool refined = false;   // edges moved to where the sound starts and stops
  std::vector<word> words;
};

struct silence {
  nle::rational at, end;    // timeline: below the speech threshold for the whole of it
  nle::rational cut_from, cut_to;  // the same, less a breath at each end (what a tightening cut removes)
};

struct transcript_options {
  int hop_ms = 10;
  int min_silence_ms = 400;
  int keep_ms = 80;          // kept at each side of a silence when cutting it
  bool words = true;
};

struct laid_transcript {
  std::vector<phrase> phrases;
  std::vector<silence> silences;
  float floor_db = 0, speech_db = 0, threshold_db = 0;
  bool loudness = false;     // the silences and refinements came from the sound
};

// `loud` holds dBFS per hop from `loud_from_ms` (file time). `segments` in
// file time. Only what lies inside the item's used range is laid.
[[nodiscard]] laid_transcript lay(const nle::item& item, nle::rational frame, const std::vector<segment>& segments,
                                  const std::vector<float>& loud, std::int64_t loud_from_ms,
                                  const transcript_options& options);

}  // namespace mv::agent
