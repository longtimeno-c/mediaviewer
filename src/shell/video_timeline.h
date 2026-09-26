// SPDX-License-Identifier: GPL-2.0-or-later
// PR 30 — the Video Editor's timeline (plan/21), shared by both hosts.
//
// One clip, cut into pieces: the timeline is the kept ranges of the source,
// in source order, played back to back. Split, delete, set in / out and
// undo / redo edit the list; the export is clip::op::keep_ranges over it.
// Pure and UI-thread only, like edit_session: no I/O, no decode. The host
// plays the source and asks `next_play_start` where to jump when the
// playhead leaves a piece.
//
// Two clocks: *source* time is the player's timeline (clip::time_ns); the
// *timeline* time is the edited program, 0 .. length().
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "edit/clip.h"

namespace mv::shell {

class video_timeline {
 public:
  using time_ns = edit::clip::time_ns;

  // A new clip: one piece, the whole of it. Clears the history.
  void load(time_ns duration_ns);
  [[nodiscard]] bool loaded() const noexcept { return duration_ > 0; }
  [[nodiscard]] time_ns source_duration() const noexcept { return duration_; }

  // Kept source ranges, ascending, never empty once loaded.
  [[nodiscard]] const std::vector<edit::clip::range>& pieces() const noexcept { return pieces_; }
  [[nodiscard]] time_ns length() const noexcept;
  // Where piece `i` starts on the timeline.
  [[nodiscard]] time_ns piece_start(std::size_t i) const noexcept;
  // The piece under a timeline time (the last one at or past the end).
  [[nodiscard]] std::size_t piece_at(time_ns timeline_t) const noexcept;

  [[nodiscard]] time_ns to_source(time_ns timeline_t) const noexcept;
  // nullopt when that source time was cut out.
  [[nodiscard]] std::optional<time_ns> to_timeline(time_ns source_t) const noexcept;
  // Playback: the source time to be at, given where the player is now.
  // Inside a piece: `source_t` itself. In a cut, or within `lead` of a
  // piece's end: the next piece's start. Past the last piece: -1 (stop).
  [[nodiscard]] time_ns next_play_start(time_ns source_t, time_ns lead = 0) const noexcept;

  // Edits. Each returns false (and changes nothing) when it would not change
  // anything or would leave nothing. Pieces shorter than kMinPiece are not made.
  bool split(time_ns timeline_t);
  bool remove(std::size_t index);
  bool set_in(time_ns timeline_t);   // cut everything before
  bool set_out(time_ns timeline_t);  // cut everything after
  bool undo();
  bool redo();
  [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
  [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
  // True once anything was cut: the whole clip is not an edit worth exporting.
  [[nodiscard]] bool edited() const noexcept;

  // The export: clip::op::keep_ranges over the pieces.
  [[nodiscard]] edit::clip::request export_request(std::string source, bool exact) const;

  static constexpr time_ns kMinPiece = 40'000'000;  // 40 ms: about a frame at 25 fps
  static constexpr std::size_t kMaxHistory = 200;

 private:
  void push_history();
  bool replace(std::vector<edit::clip::range> next);

  time_ns duration_ = 0;
  std::vector<edit::clip::range> pieces_;
  std::vector<std::vector<edit::clip::range>> undo_;
  std::vector<std::vector<edit::clip::range>> redo_;
};

}  // namespace mv::shell
