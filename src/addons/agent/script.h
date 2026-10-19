// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Script Editor's cut list (docs/design/29 "Script Editor", issue #485 slice 6):
// lines of what was said, each a range of a source file, in the order the cut
// should play them. The editor builds it in the Script Editor window from the
// timeline's transcript; an agent builds the same list through the MCP tools
// (`script`, `script_edit`), so either can start and the other carry on. It
// becomes a new version of the timeline (the lines one after another on the
// storyline: nle/edit.h inserts), or a document: TXT, CSV, DOCX, PDF.
//
// The list is a small JSON file beside the timeline it was made for
// (script_path): the window and `MediaViewer --mcp` are two processes, and
// each reads it before a change and writes it whole after. Portable C++.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.h"
#include "nle/edit.h"
#include "nle/timeline.h"

namespace mv::agent {

struct script_line {
  std::string id;         // "l7": stable while the line is in the list
  std::string path;       // the source file
  std::string name;       // its clip's name on the timeline, for the reader
  std::int64_t from_ms = 0;  // file time
  std::int64_t to_ms = 0;
  std::string text;       // what is said in it
};

struct script {
  std::string project;    // the project it was made from ("" none yet)
  std::vector<script_line> lines;
  std::uint64_t next_id = 1;

  [[nodiscard]] std::int64_t length_ms() const noexcept;
  [[nodiscard]] int find(std::string_view id) const noexcept;  // index, -1 none
};

// Where a timeline's script lives: beside a --timeline file
// ("<file>.script.json"), else in the panel's shared folder ("script.json").
[[nodiscard]] std::string script_path(const std::string& bridge_dir, const std::string& timeline_file);

[[nodiscard]] std::string to_json(const script& s);
// False with why for a file that is not a script (an absent file is an empty script: load()).
[[nodiscard]] bool from_json(std::string_view text, script& out, std::string* why);
// Reads `path`; an absent or unreadable file is an empty script.
[[nodiscard]] script load_script(const std::string& path);
[[nodiscard]] bool save_script(const std::string& path, const script& s);

// One change to the list (the window's buttons and `script_edit`'s ops):
//   add     `path` [from_ms, to_ms) with `text` at `index` (-1: the end); `name` for the reader
//   remove  the line `id`
//   move    the line `id` to `index` (0-based, after the move; -1: the end)
//   text    the line `id` reads `text` (what the document says; the cut is the range)
//   clear   every line
struct script_op {
  enum class kind : std::uint8_t { add, remove, move, text, clear };
  kind k = kind::add;
  std::string id;
  std::string path, name, text;
  std::int64_t from_ms = 0, to_ms = 0;
  int index = -1;
};

// Applies `ops` in order, all or none. `added`: the ids given to the lines added.
[[nodiscard]] bool apply(script& s, const std::vector<script_op>& ops, std::string* why,
                         std::vector<std::string>* added = nullptr);
// The tool's JSON ops ([{"op":"add","path":..,"from":s,"to":s,"text":..,"at":i}, ...]).
[[nodiscard]] bool parse_script_ops(const json::value& ops, std::vector<script_op>& out, std::string* why);

// The documents. Times are "m:ss.cc" of the file (TXT, DOCX, PDF) and seconds (CSV).
[[nodiscard]] std::string export_txt(const script& s);
[[nodiscard]] std::string export_csv(const script& s);
[[nodiscard]] std::vector<std::uint8_t> export_docx(const script& s);
[[nodiscard]] std::vector<std::uint8_t> export_pdf(const script& s);

// The edit batch that makes `t`'s primary storyline the script: every
// storyline item removed (what is connected to it goes with it), then each
// line inserted at the end, in order. The caller probes the files (all in the
// scope: each line's file came from the timeline or the given folders).
[[nodiscard]] std::vector<nle::edit_op> to_edit_ops(const script& s, const nle::timeline& t);

}  // namespace mv::agent
