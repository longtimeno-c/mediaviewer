// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #345: the canvas's AI-generated badge (docs/design/06 "The AI badge", docs/design/16
// "Settings"), what both hosts show. Pure: no platform header, no I/O.
//
// The badge is chrome, not canvas: SwiftUI and WinUI draw it over the swapchain
// like the transport strip, so showing, hiding or hovering it never presents.
// It appears only on positive evidence (meta::provenance::ai_generated); a file
// with no provenance gets nothing, and there is no "not AI" mark.
#pragma once

#include <string>

#include "meta/generators.h"

namespace mv::shell {

// The accessible name both hosts give the badge (VoiceOver, Narrator).
inline constexpr const char* kAiBadgeName = "AI-generated";

struct ai_badge {
  bool shown = false;
  std::string title;     // "Made with ChatGPT", or "AI-generated" when nothing names the tool
  std::string evidence;  // the deciding fact (provenance::evidence); may be empty

  friend bool operator==(const ai_badge&, const ai_badge&) = default;
};

// `enabled` is Settings -> Viewer -> "Show a badge on AI-generated images".
[[nodiscard]] inline ai_badge make_ai_badge(const meta::provenance& p, bool enabled) {
  ai_badge b;
  if (!enabled || !p.ai_generated) return b;
  b.shown = true;
  b.title = p.made_with.empty() ? std::string(kAiBadgeName) : "Made with " + p.made_with;
  b.evidence = p.evidence;
  return b;
}

// The hover / focus text: the title, then the evidence on its own line.
[[nodiscard]] inline std::string ai_badge_tooltip(const ai_badge& b) {
  if (!b.shown) return {};
  return b.evidence.empty() ? b.title : b.title + "\n" + b.evidence;
}

// What a screen reader says after the name: the same facts as one sentence.
[[nodiscard]] inline std::string ai_badge_description(const ai_badge& b) {
  if (!b.shown) return {};
  std::string out = b.title;
  if (!b.evidence.empty()) out += ". " + b.evidence;
  return out;
}

}  // namespace mv::shell
