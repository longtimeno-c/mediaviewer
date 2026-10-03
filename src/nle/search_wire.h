// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// What crosses between the search agent and its clients (docs/design/23): the
// workflow extension inside Final Cut Pro, the agent's test client, and on
// Windows any NLE bridge that asks the same questions. docs/design/14's rules for a
// process line: flat POD, a version, a correlation id, a status, bounds on
// every length. One reply is one byte buffer (an NSData over XPC):
//
//   reply_header | row[row_count] | moment[moment_count] | utf-8 blob
//
// Each row names its path and thumbnail as (offset, length) into the blob and
// its moments (the clip's other matches, for markers) as a slice of the
// moment array. decode() checks every offset before a byte is read, so a
// short or hostile buffer is status::corrupt, never a read past the end.
//
// Privacy (rule 6): paths travel only on this local connection between
// processes signed by our team (the agent checks); nothing here is logged.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/result.h"

namespace mv::nle {

inline constexpr std::uint32_t kWireVersion = 1;
inline constexpr std::uint32_t kMaxRows = 2000;          // the engine keeps 1,000
inline constexpr std::uint32_t kMaxMomentsPerRow = 512;
inline constexpr std::uint32_t kMaxReplyBytes = 16u << 20;

enum class request_kind : std::uint32_t { text = 0, similar = 1 };

// A request's fixed half; its strings (query, path, scope folder) travel as
// strings beside it.
struct request {
  std::uint32_t version = kWireVersion;
  request_kind kind = request_kind::text;
  std::uint64_t correlation_id = 0;
  std::uint32_t scope = 2;       // MV_AI_SCOPE_*; 2 = every indexed folder
  std::uint32_t kinds = 3;       // MV_AI_KIND_* | MV_AI_FIND_*
  std::uint32_t max_results = 200;
  std::int64_t pts_ms = -1;      // similar: the moment, -1 a still
};

struct moment {
  std::int64_t pts_ms = 0;
  float score = 0;
};

struct row {
  std::string path;
  std::string thumb;             // the viewer's JPEG-512, "" when it has none yet
  std::int64_t pts_ms = -1;      // -1: a photo
  std::int64_t duration_ms = 0;  // a clip's length when known, else 0
  float score = 0;
  std::uint32_t kind = 1;        // MV_AI_KIND_PHOTOS / _VIDEOS
  std::uint32_t more = 0;        // other matching moments in the clip
  std::uint32_t match = 0;       // MV_AI_MATCH_*
  std::vector<moment> moments;   // every match in the clip, time order
};

struct reply {
  std::uint32_t version = kWireVersion;
  std::uint64_t correlation_id = 0;
  status code = status::ok;      // why there are no rows, when there are none
  std::vector<row> rows;
};

[[nodiscard]] std::vector<std::uint8_t> encode(const reply& r);
[[nodiscard]] result<reply> decode(std::span<const std::uint8_t> bytes);

// A request's fixed half, the same way (a version a reader does not know is
// status::unsupported_format; a kind out of range is status::corrupt).
[[nodiscard]] std::vector<std::uint8_t> encode(const request& r);
[[nodiscard]] result<request> decode_request(std::span<const std::uint8_t> bytes);

}  // namespace mv::nle
