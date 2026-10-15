// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The one "Press T" hint (docs/design/16 "Filmstrip hint"). Some input does
// something different while the filmstrip is up, and nothing on screen says so:
// over a PDF or DOCX the wheel zooms with the strip shown and scrolls with it
// hidden (docs/design/04 "Scrolling a document"). The host shows this text in
// a small card in the bottom-right corner, above the filmstrip (the resume
// notice's place and look: Windows IslandHost.HintNotice.cs, Mac
// HintNoticeView.swift), which fades after eight seconds. It stops for
// good once the user has pressed T over a subject with the strip up, or
// clicked the card's "Got it".
//
// One hint, not one per feature: a new subject (issue #416, a clip whose
// ←/→ walk the folder with the strip up and skip ±2 s with it hidden) is one
// enum value and one string here, and shares the learned flag. Shared by both
// hosts (D9); pure, header-only.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace mv::shell {

enum class strip_hint_subject : std::uint8_t {
  none,
  document,  // a PDF or DOCX on the canvas
};

[[nodiscard]] constexpr std::string_view strip_hint_text(strip_hint_subject s) noexcept {
  switch (s) {
    case strip_hint_subject::document: return "Press T to hide the filmstrip and scroll";
    case strip_hint_subject::none: break;
  }
  return {};
}

// Show the hint now? Only over a subject, only with the strip up, and never
// once learned.
[[nodiscard]] constexpr bool strip_hint_due(strip_hint_subject s, bool filmstrip_visible,
                                            bool learned) noexcept {
  return s != strip_hint_subject::none && filmstrip_visible && !learned;
}

// T was pressed: does that teach the hint? Only when it hid the strip over a
// subject (the strip was visible before the toggle).
[[nodiscard]] constexpr bool strip_hint_learned_by_toggle(strip_hint_subject s,
                                                          bool filmstrip_visible_before) noexcept {
  return s != strip_hint_subject::none && filmstrip_visible_before;
}

// "Page n of m": the page notice. The hint is a card of its own, not part of it.
[[nodiscard]] inline std::string page_notice(std::uint32_t page, std::uint32_t count) {
  return "Page " + std::to_string(page + 1) + " of " + std::to_string(count);
}

}  // namespace mv::shell
