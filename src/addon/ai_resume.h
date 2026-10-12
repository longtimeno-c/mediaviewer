// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #299: the launch notice's half of "ask before resuming an interrupted
// index" (docs/design/17 "Unfinished roots wait for Resume").
//
// The AI pack keeps <ai data>/resume.json current while it runs:
//
//   {"version":1,"roots":[{"id","path","kind":"photos"|"folder","done","total","held"}]}
//
// the enabled roots whose pictures are not all indexed. A host reads it at
// launch, off the UI thread, WITHOUT loading the pack (no runtime, no model
// sessions), and asks "Resume indexing Photos?" in a bottom-right notice. The
// pack itself holds those roots when it starts until the user resumes them
// (engine_options::hold_unfinished), so the notice is the only prompt.
//
// Paths here are the user's own local folders, shown in their own window; they
// are never sent anywhere (rule 6). Portable (D9): the Mac host uses this; the
// Windows chrome reads the same file in C# (IslandHost.ResumeNotice.cs).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mv::addon {

struct unfinished_root {
  std::int64_t id = 0;
  std::string path;   // the folder, or the Photos library's key
  bool photos = false;
  std::int64_t done = 0;
  std::int64_t total = 0;
};

// The roots resume.json lists. Empty when the text is not a version-1 file
// (or lists nothing); a malformed row is skipped.
[[nodiscard]] std::vector<unfinished_root> parse_ai_resume(std::string_view text);

// <data_dir>/resume.json, read and parsed. Empty when there is none. [worker]
[[nodiscard]] std::vector<unfinished_root> read_ai_resume(const std::string& data_dir);

// How the notice names a root: "Photos" for the Photos library, else the
// folder's own name ("Holiday 2026"), else its path.
[[nodiscard]] std::string unfinished_name(const unfinished_root& root);

// The notice's question: "Resume indexing Photos?", "Resume indexing Photos
// and Holiday?", "Resume indexing Photos, Holiday and 2 more?". "" for none.
[[nodiscard]] std::string resume_question(const std::vector<unfinished_root>& roots);

}  // namespace mv::addon
