// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// What the timeline agent asks of the world (docs/design/29 "The agent"):
// Local search, transcripts, a file's loudness and facts, a frame, and the
// hand-off of a new version to Final Cut Pro. The add-on fills it from the
// host table and mv.agent.1's services; the tests fill it with fakes. Every
// path it is given has already passed the scope check (scope.h).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "nle/edit.h"

namespace mv::agent {

struct hit {
  std::string path;
  std::int64_t pts_ms = -1;  // -1: a still
  std::int64_t duration_ms = 0;
  float score = 0;
  std::uint32_t kind = 0;    // MV_AI_KIND_*
  std::vector<std::int64_t> moments;
};

struct segment {
  std::int64_t start_ms = 0;  // file time
  std::int64_t end_ms = 0;
  std::string text;
};

class environment {
 public:
  virtual ~environment() = default;

  // Local search in one folder (`tree`: and below). False with the reason
  // when Local search cannot answer.
  virtual bool search(std::string_view query, std::string_view dir, bool tree, std::uint32_t kinds,
                      std::uint32_t max, std::vector<hit>& out, std::string* why) = 0;
  virtual bool similar(std::string_view path, std::int64_t pts_ms, std::string_view dir, bool tree,
                       std::uint32_t kinds, std::uint32_t max, std::vector<hit>& out, std::string* why) = 0;
  // `indexed` false: the file has not been transcribed (yet).
  virtual bool transcript(std::string_view path, std::vector<segment>& out, bool& indexed, std::string* why) = 0;
  // The file's loudness, dBFS per `hop_ms` from `from_ms` (file time) for at
  // most `count` hops (fewer at the end of the file).
  virtual bool loudness(std::string_view path, std::int64_t from_ms, int hop_ms, std::size_t count,
                        std::vector<float>& db, std::string* why) = 0;
  virtual bool probe(std::string_view path, nle::media_probe& out, std::string* why) = 0;
  // The frame at `pts_ms` as JPEG bytes (long edge at most 512).
  virtual bool frame_jpeg(std::string_view path, std::int64_t pts_ms, std::vector<std::uint8_t>& jpeg,
                          std::string* why) = 0;
  // docs/design/29 "analyse": asks the window that hosts the add-ons to index
  // these folders (path, recursive). `state`: started, queued, refused or
  // no_app; `launched`: MediaViewer was started in the background for it.
  virtual bool request_index(const std::vector<std::pair<std::string, bool>>& folders, std::string& state,
                             bool& launched, std::string* why) = 0;
  // How far the index has each folder: 0 not covered, 1 indexing, 2 complete.
  virtual bool coverage(const std::vector<std::string>& folders, std::vector<int>& states, std::string* why) = 0;
  // Asks Final Cut Pro to import a new version. False with why where there is
  // no FCP to hand it to (Windows: the file is the result).
  virtual bool hand_to_editor(const std::string& fcpxml_path, std::string* why) = 0;
  // Settings > Add-ons > AI agents: "thumbnails" (pixels to the model).
  virtual bool thumbnails_allowed() = 0;
  virtual std::int64_t now_ms() = 0;
  virtual void sleep_ms(int ms) = 0;
  // Status names only, never a path, a name or a query (rule 6).
  virtual void log(const char* ascii) = 0;
};

}  // namespace mv::agent
