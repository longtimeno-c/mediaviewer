// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A Final Cut Pro project as a timeline model (docs/design/29 "The model").
//
// The editor drags a project from FCP onto the MediaViewer panel; FCP hands
// over FCPXML of the whole timeline. This reads it into a list of items in
// timeline order (clips, gaps, transitions, titles, connected clips and
// storylines, markers) over the lossless tree (xml.h), so an edit changes a
// few attributes or child lists and everything else is written back as FCP
// wrote it. A new version of the project is the same document with only that
// project in its event, renamed, without its uid, so FCP imports it beside
// the original and never over it.
//
// What is not understood is not touched: an element this model does not know
// is listed as `other` and marked not editable; compound clips (ref-clip),
// multicam clips (mc-clip), synchronised clips and auditions are opaque (moved
// and trimmed as a unit, never opened); effects, colour, keyframes, retiming,
// audio components and metadata are only carried.
//
// Portable C++: the FCP panel (Mac), the MCP server (both platforms, on a
// dragged project or an FCPXML file) and mv-timeline use it.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "nle/rational.h"
#include "nle/xml.h"

namespace mv::nle {

enum class item_kind : std::uint8_t {
  asset_clip,
  clip,        // a container clip (FCP's "clip": a compound made in an event, or an older export)
  ref_clip,    // a compound clip
  mc_clip,     // a multicam clip
  sync_clip,   // a synchronised clip
  title,
  gap,
  transition,
  audio,
  video,
  audition,
  caption,
  live_drawing,
  storyline,   // a connected storyline (a spine with a lane)
  other,       // an element this model does not know: carried, never edited
};

[[nodiscard]] const char* kind_name(item_kind k);

struct marker {
  std::uint32_t node = xml::kNone;
  std::string kind;      // "marker" or "chapter-marker"
  rational start;        // in the clip's own time, as written
  rational duration;
  rational at;           // on the timeline
  std::string value;
  bool todo = false;     // a to-do marker (completed="0" / "1")
  bool completed = false;
};

struct item {
  std::uint32_t node = xml::kNone;
  item_kind kind = item_kind::other;
  int parent = -1;       // the item this one is anchored to / sits in; -1 = the project's primary storyline
  int lane = 0;          // 0 = in a storyline; > 0 above its parent, < 0 below
  bool in_storyline = false;  // a child of a spine (the primary one or a connected storyline)
  int storyline = -1;    // the storyline item holding it; -1 = primary (or anchored, not in a storyline)
  rational offset;       // as written: where it sits in its parent's time
  rational start;        // as written: its own local start (source in for media clips)
  rational duration;
  rational at;           // where it starts on the timeline (sequence time from the sequence's start)
  bool enabled = true;
  std::string name;
  std::string ref;       // the resource it uses (asset, media, effect)
  std::string role;      // audioRole / videoRole / role, as written ("dialogue", "video.B-roll")
  std::string source;    // the media file it plays (a path), when it has one
  rational source_in;    // file time of its first frame (start minus the asset's start)
  std::string text;      // a title's or caption's text
  std::vector<marker> markers;
  bool has_effects = false;  // video / audio filters, adjustments other than conform
  bool retimed = false;      // a timeMap: the file time is not 1:1 with the timeline
  bool split_audio = false;  // audioStart / audioDuration: a J or L cut
  rational audio_start;
  rational audio_duration;
  bool opaque = false;       // compound, multicam, sync clip, audition: a unit, not opened
  bool editable = false;     // what an edit may change
  std::string not_editable;  // why not, when it is not
};

struct asset {
  std::string id;
  std::string name;
  std::string uid;
  std::string path;      // from media-rep src (or the asset's own src in FCPXML <= 1.8)
  rational start;
  rational duration;
  bool has_video = false;
  bool has_audio = false;
  std::string format;
};

struct project_info {
  std::uint32_t node = xml::kNone;
  std::uint32_t sequence = xml::kNone;
  std::uint32_t spine = xml::kNone;
  std::uint32_t event = xml::kNone;
  std::string name;
  std::string uid;
  std::string event_name;
  std::string library;   // the library's location (a file URL), when the document names it
  rational tc_start;
  rational duration;
  std::string format;    // the sequence's format id
  rational frame;        // its frame duration (0 when the format names none)
  int width = 0;
  int height = 0;
};

class timeline {
 public:
  // Parses FCPXML and reads its first project (or the one named `project`).
  // False with a reason when the text is not XML, not FCPXML, or holds no
  // project.
  [[nodiscard]] bool load(std::string fcpxml, std::string* error, std::string_view project = {});

  // Re-reads the items after the tree was edited.
  void reindex();

  [[nodiscard]] const xml::document& doc() const noexcept { return doc_; }
  [[nodiscard]] xml::document& doc() noexcept { return doc_; }
  [[nodiscard]] const std::string& version() const noexcept { return version_; }
  [[nodiscard]] const project_info& project() const noexcept { return project_; }
  [[nodiscard]] const std::vector<std::string>& project_names() const noexcept { return project_names_; }
  [[nodiscard]] const std::vector<item>& items() const noexcept { return items_; }
  [[nodiscard]] const std::map<std::string, asset>& assets() const noexcept { return assets_; }
  // Resource id -> name for media (compounds, multicams) and effects (titles, filters).
  [[nodiscard]] const std::map<std::string, std::string>& resource_names() const noexcept { return names_; }
  // The frame duration of a format id (0/1 when unknown).
  [[nodiscard]] rational frame_of(std::string_view format) const;

  // The program length: the primary storyline's end.
  [[nodiscard]] rational length() const;

  // Every media file the timeline plays (primary storyline, connected clips,
  // and inside compounds and multicams, which are not opened for editing but
  // are the timeline's media). Sorted, unique.
  [[nodiscard]] std::vector<std::string> media_paths() const;

  // A line per item and marker, in timeline order: what a round trip must
  // keep (kind, name, ref, file, lane, times, enabled, role, text, markers,
  // effects). Two documents with the same description import as the same
  // clip list with the same timing.
  [[nodiscard]] std::string describe() const;

  // The document a new version of this project is: the whole tree with only
  // this project left in its event (and only its event in its library), the
  // project renamed and its uid and modDate dropped (FCP gives it new ones),
  // and resources nothing in it references removed. Every other byte is the
  // source's.
  [[nodiscard]] std::string write_version(std::string_view new_name) const;

 private:
  void read_resources();
  void walk_spine(std::uint32_t spine, int parent, int storyline, rational base_at, rational origin);
  int add_item(std::uint32_t node, int parent, int storyline, bool in_storyline, rational at);
  void walk_anchored(int self);
  bool find_project(std::string_view wanted, std::string* error);
  bool read_project(std::uint32_t node, std::string* error);

  xml::document doc_;
  std::string version_;
  project_info project_;
  std::vector<std::string> project_names_;
  std::vector<item> items_;
  std::map<std::string, asset> assets_;
  std::map<std::string, std::string> names_;
  std::map<std::string, rational> frames_;  // format id -> frame duration
  std::map<std::string, std::pair<int, int>> sizes_;
  std::map<std::string, std::vector<std::string>> media_files_;  // media id -> files it plays
  std::string wanted_project_;
};

// A file:// URL for a local path: '/' separators, a drive letter as
// file:///C:/..., and every byte outside RFC 3986's unreserved set and '/'
// percent-encoded. (Also declared by fcpxml.h, which writes search results.)
[[nodiscard]] std::string file_url(std::string_view path_utf8);

// A file:// URL's path ("file:///Users/a%20b/x.mov" -> "/Users/a b/x.mov",
// "file:///C:/x" -> "C:/x", "file://server/share/x" -> "//server/share/x").
// Empty when it is not a file URL.
[[nodiscard]] std::string path_of_file_url(std::string_view url);

// "Interview (Claude 2)": the first "(Claude N)" name not in `taken`, after
// stripping a "(Claude N)" the base name already carries.
[[nodiscard]] std::string next_version_name(std::string_view base, const std::vector<std::string>& taken);

}  // namespace mv::nle
