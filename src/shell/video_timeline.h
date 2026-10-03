// SPDX-License-Identifier: GPL-3.0-or-later
// PR 30 — the Video Editor's timeline (docs/design/21), shared by both hosts.
//
// One clip, cut into pieces: the timeline is the kept ranges of the source,
// in source order, played back to back. Split, delete, set in / out, a marked
// range, dragged piece edges and undo / redo edit the list; the export is
// clip::op::keep_ranges over it.
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

  // A marked range (I / O, as in every editor): marking cuts nothing. Timeline
  // times; an unset end is -1 and means the program's start (in) or end (out).
  // A mark past the other one clears it. Any edit clears both, since the
  // times they name have moved.
  void mark_in(time_ns timeline_t);
  void mark_out(time_ns timeline_t);
  void clear_marks() noexcept { mark_in_ = mark_out_ = -1; }
  [[nodiscard]] bool has_marks() const noexcept { return mark_in_ >= 0 || mark_out_ >= 0; }
  [[nodiscard]] time_ns marked_in() const noexcept { return mark_in_; }
  [[nodiscard]] time_ns marked_out() const noexcept { return mark_out_; }
  // Cut the marked range out of the program: one edit, one undo.
  bool remove_marked();
  // Cut the program range [from, to): slivers under kMinPiece are not left.
  bool remove_range(time_ns from, time_ns to);

  // Dragging a piece's edge: grab it, move it in source time as often as the
  // pointer moves, let go. The whole drag is one undo step. The edge stays
  // inside its neighbours (pieces never overlap) and the piece keeps kMinPiece.
  enum class edge { in, out };
  bool begin_trim(std::size_t index, edge which);
  // Returns the source time the edge is at now (clamped); -1 when not trimming.
  time_ns trim_to(time_ns source_t);
  void end_trim();
  [[nodiscard]] bool trimming() const noexcept { return trim_index_ != kNoTrim; }

  bool undo();
  bool redo();
  [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
  [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
  // True once anything was cut: the whole clip is not an edit worth exporting.
  [[nodiscard]] bool edited() const noexcept;
  // Bumps on every change to the pieces (edits, trims, undo, redo). A host
  // compares it with the revision it last exported to know whether closing
  // would lose work.
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

  // The export: clip::op::keep_ranges over the pieces.
  [[nodiscard]] edit::clip::request export_request(std::string source, bool exact) const;

  static constexpr time_ns kMinPiece = 40'000'000;  // 40 ms: about a frame at 25 fps
  static constexpr std::size_t kMaxHistory = 200;

 private:
  static constexpr std::size_t kNoTrim = static_cast<std::size_t>(-1);
  void push_history();
  bool replace(std::vector<edit::clip::range> next);

  time_ns duration_ = 0;
  time_ns mark_in_ = -1;
  time_ns mark_out_ = -1;
  std::uint64_t revision_ = 0;
  std::size_t trim_index_ = kNoTrim;
  edge trim_edge_ = edge::in;
  bool trim_pushed_ = false;  // the drag's one history entry is in
  std::vector<edit::clip::range> pieces_;
  std::vector<std::vector<edit::clip::range>> undo_;
  std::vector<std::vector<edit::clip::range>> redo_;
};

// J K L (docs/design/16, docs/design/21): L plays forward, faster on each press (1x, 2x,
// 4x: the player cannot run backwards); K stops; J skims back, each press in
// a quick burst going further than the last (1, 2, 4, 8 s). Pure: the host
// owns the clock and the player.
class editor_shuttle {
 public:
  using time_ns = edit::clip::time_ns;
  // L: the rate to play at now.
  double forward() noexcept;
  // K, Space, or anything that takes the playhead elsewhere.
  void stop() noexcept;
  // J at monotonic time `now`: the program time to skim back to, from
  // `playhead` or, inside a burst, from where the last J was going. A held
  // key's auto-repeat (`repeat`) keeps the step it has instead of doubling it.
  time_ns back(time_ns playhead, time_ns now, bool repeat = false) noexcept;
  [[nodiscard]] double rate() const noexcept { return kRates[level_]; }

  static constexpr double kRates[] = {1.0, 1.0, 2.0, 4.0};  // [0] is "not shuttling"
  static constexpr time_ns kBurst = 600'000'000;  // presses closer than this build up
  static constexpr time_ns kFirstSkim = 1'000'000'000;
  static constexpr time_ns kMaxSkim = 8'000'000'000;

 private:
  int level_ = 0;
  time_ns last_back_ = -1;  // monotonic time of the last J; -1 none
  time_ns skim_ = 0;        // the last J's step
  time_ns target_ = 0;      // where it was going
};

}  // namespace mv::shell
