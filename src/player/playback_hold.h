// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #44: a clip does not play while an overlay covers the canvas.
//
// Both hosts run this rule on their render thread: the ABI's video_session on
// Windows, present_lab_mac on the Mac. It never touches a media_source; it says
// what to do and the owner does it, so it is headless and testable.
//
// `view` is whatever the owner already uses to tie a clip to the item that asked
// for it and only ever moves forward: the view generation on Windows, the item
// id on the Mac. Holding pauses a playing clip and stops a newly adopted one from
// autoplaying. Releasing resumes only the clip that was playing (or was about to
// start) when the hold began; a clip selected under the hold stays paused until
// an explicit Play. A clip that was already paused is never touched.
#pragma once

#include <cstdint>

#include "player/media_source.h"

namespace mv::player {

enum class hold_action : std::uint8_t { none, pause, play };

// The adopted clip as the owner sees it at the moment of the transition.
struct held_clip {
  bool open = false;          // a clip is adopted
  std::uint64_t view = 0;     // the view it belongs to
  play_state state = play_state::stopped;
};

class playback_hold {
 public:
  [[nodiscard]] bool held() const noexcept { return held_; }

  // `live` is the view on screen now.
  [[nodiscard]] hold_action hold(std::uint64_t live, const held_clip& clip) noexcept {
    if (held_) return hold_action::none;
    held_ = true;
    resume_ = false;
    quiet_ = false;
    const bool on_screen = clip.open && clip.view == live;
    if (clip.open && clip.state == play_state::playing) {
      // A stale clip is about to be retired; silence it all the same.
      if (on_screen) remember(live);
      return hold_action::pause;
    }
    // Nothing adopted for this view yet: if it turns out to be a clip it was
    // going to autoplay, so it does once the hold lifts.
    if (!on_screen) remember(live);
    return hold_action::none;
  }

  // `resume` false: the host is leaving the overlay for something new (an open,
  // Settings), so even the held clip stays paused.
  [[nodiscard]] hold_action release(std::uint64_t live, const held_clip& clip,
                                    bool resume) noexcept {
    if (!held_) return hold_action::none;
    held_ = false;
    const bool back = resume && resume_ && resume_view_ == live;
    resume_ = false;
    if (!back) {
      quiet_ = true;
      quiet_view_ = live;
      return hold_action::none;
    }
    quiet_ = false;
    if (clip.open && clip.view == live && clip.state == play_state::paused) {
      return hold_action::play;
    }
    return hold_action::none;
  }

  // A clip for `view` has just been adopted: start it?
  [[nodiscard]] bool autoplay(std::uint64_t view) const noexcept {
    if (held_) return false;
    return !(quiet_ && view == quiet_view_);
  }

  // Play / pause pressed while held: the user's choice outranks the one made
  // on entry, so release leaves the clip as it is.
  void user_transport() noexcept {
    if (held_) resume_ = false;
  }

 private:
  void remember(std::uint64_t view) noexcept {
    resume_ = true;
    resume_view_ = view;
  }

  bool held_ = false;
  bool resume_ = false;
  bool quiet_ = false;
  std::uint64_t resume_view_ = 0;
  std::uint64_t quiet_view_ = 0;
};

}  // namespace mv::player
