// SPDX-License-Identifier: GPL-2.0-or-later
// The clip transport's idle state, shared by both hosts (issue #38).
//
// While a clip plays, the transport floats over the bottom of the video and
// leaves after kTransportIdleMs of no activity; pointer movement, a click, or a
// transport key brings it straight back. Paused, ended, held (the pointer on
// it, a scrub, an open menu, keyboard focus in it) or under a screen reader, it
// stays. In fullscreen the pointer leaves with it while it is over the video.
//
// Hiding is only visual. Decode, audio, the canvas rectangle and every key keep
// working, and nothing here repaints: the host re-evaluates on an event or at
// due_in(), a one-shot timer, never per frame. Nothing here includes a platform
// header, so the Win32 and AppKit hosts run the same rule and it is tested
// headless (tests/test_transport_autohide.cpp).
#pragma once

#include <cstdint>

#include "shell/commands.h"

namespace mv::shell {

// Long enough to read the clock after a seek, short enough not to sit on the
// picture. Both hosts use this one value.
inline constexpr std::uint64_t kTransportIdleMs = 2500;

// What the host knows at the moment it asks. Plain facts, no policy.
struct transport_view {
  bool clip = false;               // a clip's transport is laid out (not gallery, settings, Live Photo)
  bool playing = false;            // false while paused or at the end
  bool held = false;               // pointer on it, scrubbing, a menu open, keyboard focus in it
  bool screen_reader = false;      // VoiceOver / Narrator: never take the controls away
  bool fullscreen = false;
  bool pointer_on_canvas = false;  // the pointer is over the video, not over chrome
};

// The keys that count as "reaching for the transport". The host runs the key
// as it always does; this only decides whether it also wakes the controls.
// ↑ ↓ are volume on a clip, so they count there too.
[[nodiscard]] constexpr bool is_transport_command(command_id c) noexcept {
  switch (c) {
    case command_id::play_pause:
    case command_id::pause:
    case command_id::jump_back:
    case command_id::jump_forward:
    case command_id::frame_back:
    case command_id::frame_forward:
    case command_id::rate_down:
    case command_id::rate_up:
    case command_id::skim_back:
    case command_id::skim_forward:
    case command_id::skim_settle:
    case command_id::mute:
    case command_id::pan_up:
    case command_id::pan_down:
      return true;
    default:
      return false;
  }
}

class transport_autohide {
 public:
  // Pointer moved, entered or clicked, or a transport key: restart the clock.
  // Call update() after it.
  void activity(std::uint64_t now_ms) noexcept { last_ms_ = now_ms; }

  // Re-evaluate. True when shown() or pointer_hidden() changed, so the host
  // only touches a view (or the cursor) on a real transition.
  bool update(const transport_view& v, std::uint64_t now_ms) noexcept {
    const bool was_shown = shown_;
    const bool was_hidden = pointer_hidden_;
    const bool keep = !v.clip || !v.playing || v.held || v.screen_reader;
    if (keep) {
      // No clip (arm it so the next one arrives with controls), or held open.
      shown_ = true;
      last_ms_ = now_ms;
    } else {
      // Letting go -- resume, end of a scrub, a menu closing -- starts a full
      // interval now, however long the hold lasted.
      if (kept_) last_ms_ = now_ms;
      shown_ = now_ms - last_ms_ < kTransportIdleMs;
    }
    kept_ = keep;
    // Outside fullscreen the system pointer is the user's; never hide it there.
    pointer_hidden_ = v.clip && !shown_ && v.fullscreen && v.pointer_on_canvas;
    return shown_ != was_shown || pointer_hidden_ != was_hidden;
  }

  [[nodiscard]] bool shown() const noexcept { return shown_; }
  [[nodiscard]] bool pointer_hidden() const noexcept { return pointer_hidden_; }

  // Milliseconds until update() is worth calling again; 0 when only an event
  // can change anything (no clip, paused, already hidden, screen reader).
  // Held re-checks every interval, because letting go of a hover or of focus
  // does not always arrive as an event the host sees.
  [[nodiscard]] std::uint64_t due_in(const transport_view& v, std::uint64_t now_ms) const noexcept {
    if (!v.clip || !v.playing || v.screen_reader) return 0;
    if (v.held) return kTransportIdleMs;
    if (!shown_) return 0;
    const std::uint64_t idle = now_ms - last_ms_;
    return idle >= kTransportIdleMs ? 1 : kTransportIdleMs - idle;
  }

 private:
  std::uint64_t last_ms_ = 0;
  bool shown_ = true;
  bool pointer_hidden_ = false;
  bool kept_ = true;
};

}  // namespace mv::shell
