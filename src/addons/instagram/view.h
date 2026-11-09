// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Instagram Post window as view trees (docs/design/25 §10, docs/design/30).
//
// Its composition follows the Stageshow rules the owner asked for: one focal
// object (the carousel, at the shape it will be posted), real words only on
// what is being done (the shape, the count, the search, Export), a full
// gallery beneath it, and controls that stay where they are and change on
// themselves. Motion is the hosts': the strip is an "animate" stack, so a tile
// arrives where it was chosen and its neighbours make room.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "addons/instagram/post.h"

namespace mv::instagram {

inline constexpr std::string_view kScreen = "compose";
inline constexpr std::size_t kGalleryPage = 3000;

struct gallery_row {
  std::string path;
  std::string name;
  bool video = false;
  std::int64_t when = 0;
};

enum class source : std::uint8_t { folder, library };
// What the lower half of the window is doing.
enum class tool_mode : std::uint8_t { choose, adjust, enhance };

// The big preview beside the tools, in points.
[[nodiscard]] tile_size stage_of(shape s) noexcept;
inline constexpr std::string_view kStageNode = "big";
inline constexpr std::string_view kStageSurface = "stage";

struct view_state {
  const post* p = nullptr;
  source from = source::folder;
  bool has_folder = false;
  bool has_library = false;
  std::string query;
  // What the gallery shows, already filtered, and how much of it.
  const std::vector<const gallery_row*>* rows = nullptr;
  std::size_t shown = 0;
  bool loading = false;
  bool exporting = false;
  double progress = 0;  // 0..1
  std::string progress_words;
  std::string note;      // what the last export did, or why a press did nothing
  bool can_show = false;  // the last export's files can be shown
  tool_mode mode = tool_mode::choose;
  bool enhance_missing = false;  // Enhance is not installed, or too old
  // A tile whose picture could not be read says so under it.
  std::vector<std::uint64_t> unreadable;
};

[[nodiscard]] std::string tree_json(const view_state& v);
// One [{"op":...}] of what changes with each kind of event.
[[nodiscard]] std::string strip_diff(const view_state& v);
[[nodiscard]] std::string gallery_diff(const view_state& v);
[[nodiscard]] std::string progress_diff(const view_state& v);
[[nodiscard]] std::string panel_diff(const view_state& v);
// Reset alone, as the first slider moves or the last returns to zero.
[[nodiscard]] std::string reset_diff(bool disabled);

[[nodiscard]] std::string surface_id(std::uint64_t uid);
[[nodiscard]] std::string tile_node_id(std::uint64_t uid);
// The tile's uid, or 0 for any other node.
[[nodiscard]] std::uint64_t uid_of_tile_node(std::string_view node) noexcept;

}  // namespace mv::instagram
