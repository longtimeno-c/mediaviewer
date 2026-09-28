// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Search results as FCPXML (plan/23 "Hand-off"): what the workflow extension
// puts on the pasteboard when tiles are dragged into Final Cut Pro, and what
// "Export results as FCPXML" writes on either platform (DaVinci Resolve and
// Premiere Pro import FCPXML too, which makes this the Windows half's
// hand-off). Portable C++, no XML library: the document is small and its
// shape is ours.
//
// Each result is one asset (its file, by file:// URL) and one clip of it in an
// event: a video moment as a range around the match (before_ms / after_ms,
// clamped to the clip) with a marker at the match, and a still at still_ms.
// The query becomes a keyword over each clip, so FCP files the drop into a
// keyword collection ("MV: birthday cake"); that is optional (plan/23 open
// question 8). Times are rational milliseconds ("8000/1000s"); FCP conforms a
// time that is not on a frame boundary of the clip, which Phase 0 measures.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "addons/fcp/search_wire.h"

namespace mv::nle {

struct fcpxml_options {
  std::string event_name = "MediaViewer";
  std::string keyword;                // "" = no keyword collection
  std::int64_t before_ms = 2000;
  std::int64_t after_ms = 3000;
  std::int64_t still_ms = 5000;
  std::string version = "1.10";       // FCP 10.6+, Resolve 18+, Premiere's importer
};

// The whole document (UTF-8), for the pasteboard or a .fcpxml file.
[[nodiscard]] std::string fcpxml(std::span<const row> rows, const fcpxml_options& options);

// A file:// URL for a local path: '/' separators, a drive letter as
// file:///C:/..., and every byte outside RFC 3986's unreserved set and '/'
// percent-encoded.
[[nodiscard]] std::string file_url(std::string_view path_utf8);

// The clip range a moment gets: [start, start + duration) in ms, `start` >= 0
// and inside a known clip length.
struct clip_range {
  std::int64_t start_ms = 0;
  std::int64_t duration_ms = 0;
};
[[nodiscard]] clip_range range_around(std::int64_t pts_ms, std::int64_t clip_ms, const fcpxml_options& options);

}  // namespace mv::nle
