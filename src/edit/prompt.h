// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/edit-adjust.md slice 5b: a typed request ("a bit brighter and
// warmer", "less noisy", "black and white", "like an old film") turned into
// moves of the Adjust pane's own sliders. No model and no download: a fixed
// vocabulary, deterministic, instant, offline; every result is the sliders,
// so it can be read back and changed by hand, and it lands as one op.
//
//   * Each phrase names a slider and a direction ("brighter": exposure up,
//     "too warm": temperature down). "less" and "not so" flip it; "a bit",
//     "slightly" halve it; "much", "a lot", "very" double it.
//   * Moves add to the slider where it is now, land on its steps and are
//     clamped to its range.
//   * A few looks are several sliders at once ("punchy", "film", "matte",
//     "dramatic", "soft").
//   * "auto" with a section's name ("auto colour", "fix the light") asks for
//     that section's Auto; the host runs it after the moves land.
//   * Words it does not know are listed back, never guessed at.
// Pure; UI thread (a sentence is microseconds).
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "edit/adjust.h"

namespace mv::edit {

struct prompt_result {
  colour value;                       // `current` with the moves applied
  std::vector<std::string> applied;   // "Exposure +0.4", one per slider moved, in order
  std::vector<std::string> unknown;   // words that matched nothing
  std::vector<adjust_section> autos;  // sections whose Auto was asked for
  bool recognised = false;            // a word named a slider (moved or already at its end) or an Auto
  [[nodiscard]] bool changed(const colour& current) const noexcept { return !(value == current); }
};

[[nodiscard]] prompt_result parse_prompt(std::string_view text, const colour& current);

// What the pane's note says when no word of the request moved anything.
inline constexpr const char* kPromptNotRecognised =
    "Didn't recognise that — try 'warmer', 'brighter', 'less noise'";

// The pane's note: "Exposure +0.3 EV · Temperature +15 · Noise reduction +30"
// (and "· not understood: …" for words left over), kPromptNotRecognised when
// nothing was recognised, "Nothing to change: …" when the sliders are
// already where the words say.
[[nodiscard]] std::string prompt_summary(const prompt_result& r);

}  // namespace mv::edit
