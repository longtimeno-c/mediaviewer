// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Edits on a Final Cut Pro timeline model (docs/design/29 "Edits").
//
// The agent's `edit` tool and the Script Editor change a timeline through
// this: ripple and lift cuts of a range, trims, splits, removals, moves,
// another range of the same file, clips inserted or connected from the scope,
// markers, title text, volume, enable. Each operation changes the lossless
// tree (xml.h) in as few places as it can, so everything it does not touch is
// written back as Final Cut Pro wrote it.
//
// Rules every operation keeps:
//   - Times are snapped to the sequence's frame grid.
//   - Only `editable` items are changed (timeline.h): a compound, multicam,
//     sync clip or audition is trimmed, split and moved as a unit and never
//     opened; an element the model does not know is never touched.
//   - Connected clips follow the content they are attached to. A cut removes
//     the part of a connected clip that lies inside it (splitting one that
//     spans it); a music clip (role "music", or audio only below the
//     storyline) plays on through a cut instead, shortened by what was cut.
//     A connected storyline that a cut crosses refuses the cut.
//   - Transitions a cut or a move touches are removed (their handles may not
//     exist any more).
//   - J and L cuts keep their lead and tail when a clip's edge moves.
//   - Every change can leave a marker ("Claude: …") where it happened, so the
//     editor finds each one in FCP.
//
// A batch is atomic: if one operation fails, the tree is put back as it was
// before the batch. Item ids and times in a batch refer to the timeline as it
// was before the batch (later times are mapped through the earlier cuts and
// inserts), so "cut 0:12-0:13, 0:40-0:41.5" means what it says. A move
// reorders the storyline, so a batch with a move holds no time-addressed
// operation. Portable C++.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nle/rational.h"
#include "nle/timeline.h"

namespace mv::nle {

// What the agent knows about a file it inserts or connects (from the host's
// probe). A field left 0 is not written; FCP reads the file itself.
struct media_probe {
  rational duration;
  bool has_video = false;
  bool has_audio = false;
  int width = 0;
  int height = 0;
  rational frame;  // frame duration, 0 when unknown or a still
  int audio_channels = 0;
  int audio_rate = 0;
};

struct edit_op {
  enum class kind : std::uint8_t {
    cut,             // remove [from, to) of the timeline; ripple closes the gap, else a gap is left
    trim,            // an item keeps [from, to) of the timeline (shorter, or longer into its media)
    split,           // an item at `at`
    remove,          // an item; a storyline item ripples unless !ripple
    move,            // a storyline item before `before` (-1: to the end)
    source,          // an item plays another range (`source_in`, file time) of its file, or of `path`
    insert,          // `path` [source_in, +duration) into the storyline before `before` (-1: the end)
    connect,         // `path` [source_in, +duration) connected at `at` on `lane` (video only unless `with_audio`)
    marker,          // a marker at `at` saying `text` (`todo`: a to-do)
    unmarker,        // an item's marker `marker`
    title,           // a title's or caption's text
    volume,          // an item's volume, in dB
    enable,          // an item on or off
  };
  kind k = kind::cut;
  int item = -1;           // an item id (timeline::items index before the batch)
  rational from, to, at;   // timeline times (before the batch)
  bool has_from = false, has_to = false;
  // trim by amounts instead: seconds off the head / tail (negative: longer, into the media's handles)
  rational head, tail;
  bool has_head = false, has_tail = false;
  bool ripple = true;
  int before = -1;         // an item id, or -1
  std::string path;        // a file in the scope
  rational source_in;      // file time
  rational duration;
  int lane = 1;
  bool with_audio = false;
  std::string text;
  bool todo = false;
  int marker = -1;
  double db = 0;
  bool on = true;
};

struct change {
  std::string what;  // "cut 1.2 s at 0:42.04"
  rational at;       // where it is on the edited timeline
  rational removed;  // timeline time taken out (0 for most)
  rational added;    // timeline time put in
};

struct edit_options {
  bool mark_changes = true;      // a marker on every change ("Claude: …")
  bool music_plays_through = true;
};

class editor {
 public:
  explicit editor(timeline& t, edit_options options = {}) : t_(t), o_(options) {}

  // Applies `ops` in order as one batch. On failure returns false with the
  // failing op's index and why, and the timeline is as it was.
  // `probes` answers insert / connect: the file's facts, by path.
  bool apply(const std::vector<edit_op>& ops, const std::vector<std::pair<std::string, media_probe>>& probes,
             std::string* error, std::size_t* failed_op = nullptr);

  [[nodiscard]] const std::vector<change>& changes() const noexcept { return changes_; }
  void clear_changes() { changes_.clear(); }

 private:
  struct map_step {
    bool remove;  // remove [a, b) / insert at a of length `len`
    rational a, b, len;
  };
  rational map_time(rational t) const;

  bool op_cut(rational from, rational to, bool ripple, std::string* error, std::string_view why = "cut");
  bool op_trim(std::uint32_t node, const edit_op& op, std::string* error);
  bool op_split(std::uint32_t node, rational at, std::string* error);
  bool op_remove(std::uint32_t node, bool ripple, std::string* error);
  bool op_move(std::uint32_t node, std::uint32_t before, std::string* error);
  bool op_source(std::uint32_t node, const edit_op& op, const media_probe* probe, std::string* error);
  bool op_insert(const edit_op& op, std::uint32_t before, const media_probe& probe, std::string* error);
  bool op_connect(const edit_op& op, const media_probe& probe, std::string* error);
  bool op_marker(rational at, const std::string& text, bool todo, std::string* error);
  bool op_unmarker(std::uint32_t node, int marker, std::string* error);
  bool op_title(std::uint32_t node, const std::string& text, std::string* error);
  bool op_volume(std::uint32_t node, double db, std::string* error);
  bool op_enable(std::uint32_t node, bool on, std::string* error);

  // Helpers over the current tree.
  [[nodiscard]] const item* item_of(std::uint32_t node) const;
  [[nodiscard]] std::vector<int> storyline() const;  // primary storyline item ids, in order
  [[nodiscard]] int storyline_at(rational t) const;  // the primary item that plays at t (-1 none)
  [[nodiscard]] rational snap(rational t) const;
  void relay();                                      // primary offsets one after another, sequence duration
  std::string ensure_asset(const std::string& path, const media_probe& probe);
  void insert_anchor(std::uint32_t host, std::uint32_t child);
  void insert_marker_node(std::uint32_t host, std::uint32_t marker);
  void mark(rational at, const std::string& text);
  [[nodiscard]] std::string clock(rational t) const;  // "0:42.04"

  timeline& t_;
  edit_options o_;
  std::vector<map_step> steps_;  // this batch's cuts and inserts, in order
  std::vector<change> changes_;
};

// The ops a JSON array names, for the agent and the tests. Times are "12.5"
// (seconds), "0:12.5", "1:02:03.25", "750/25s" (FCPXML), or a JSON number of
// seconds. False with the reason on the first bad op.
//   {"op":"cut","from":"0:12.4","to":"0:13.1"}                (ripple; "ripple":false lifts)
//   {"op":"trim","item":3,"from":"0:10","to":"0:20"}            (or "head":"0.5","tail":"-1": seconds off each end)
//   {"op":"split","item":3,"at":"0:15"}
//   {"op":"remove","item":3}                                    ("ripple":false leaves a gap)
//   {"op":"move","item":3,"before":1}                           ("before":-1 or absent: the end)
//   {"op":"source","item":3,"source_in":"12.0","path":"…"}     (path optional)
//   {"op":"insert","path":"…","source_in":"4","duration":"3","before":2}
//   {"op":"connect","path":"…","source_in":"4","duration":"3","at":"0:21","lane":1,"with_audio":false}
//   {"op":"marker","at":"0:30","text":"check this","todo":true}
//   {"op":"unmarker","item":3,"marker":0}
//   {"op":"title","item":5,"text":"Anna Weber\nMountain guide"}
//   {"op":"volume","item":3,"db":-6}
//   {"op":"enable","item":3,"on":false}
[[nodiscard]] bool parse_ops(std::string_view json_array, std::vector<edit_op>& out, std::string* error);
[[nodiscard]] std::optional<rational> parse_clock(std::string_view s);
[[nodiscard]] std::string format_clock(rational t);  // "1:02.04", "1:02:03.50"

}  // namespace mv::nle
