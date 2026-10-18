// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// What the Final Cut Pro panel shares with the timeline agent (docs/design/29
// "The bridge"): the project the editor last dragged onto the panel, what FCP
// has open now, the folders the editor gave, and the versions written so far.
//
// The panel (a sandboxed FCP extension) writes it into its app group's
// container; the agent (`MediaViewer --mcp`, not sandboxed) reads it there.
// One folder:
//
//   <group container>/Timeline/project.fcpxml   the dragged FCPXML, its bytes
//   <group container>/Timeline/state.json       this struct
//   <group container>/Timeline/versions/*.fcpxml the new versions written
//
// On Windows (no FCP) the same struct and layout live in the agent add-on's
// data folder, filled from an FCPXML file the editor opens. Portable C++.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mv::nle {

struct bridge_version {
  std::string name;   // "Interview (Claude 1)"
  std::string file;   // its FCPXML, a file name in versions/
  std::int64_t at_ms = 0;
  std::string base;   // the project it was made from
};

struct bridge_state {
  std::string project;          // the dragged project's name ("" = nothing dragged yet)
  std::string event;
  std::string library;          // the library's location URL, as the FCPXML gave it
  std::string fcpxml_version;   // "1.13"
  std::int64_t dragged_ms = 0;  // when it was dragged (Unix ms)
  std::int64_t changed_ms = 0;  // FCP said that project changed after the drag (0 = not since)
  std::string active_project;   // what FCP has open now, "" when unknown
  double playhead_s = -1;       // FCP's playhead in the active project, < 0 unknown
  std::int64_t playhead_ms_at = 0;  // when it was read
  std::vector<std::string> folders;  // the folders the editor gave, absolute paths
  std::vector<bridge_version> versions;
};

inline constexpr std::string_view kBridgeFolder = "Timeline";
inline constexpr std::string_view kBridgeProject = "project.fcpxml";
inline constexpr std::string_view kBridgeState = "state.json";
inline constexpr std::string_view kBridgeVersions = "versions";

[[nodiscard]] std::string to_json(const bridge_state& s);
// nullopt when the text is not this struct's JSON.
[[nodiscard]] std::optional<bridge_state> bridge_state_from_json(std::string_view text);

// A file name for a version: the name with path separators, control
// characters and the few characters Windows refuses replaced, ".fcpxml" added.
[[nodiscard]] std::string version_file_name(std::string_view name);

// "dragged 2 min ago", "dragged just now", "dragged 3 h ago", "dragged yesterday".
[[nodiscard]] std::string dragged_ago(std::int64_t dragged_ms, std::int64_t now_ms);

}  // namespace mv::nle
