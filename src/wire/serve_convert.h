// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The server's half of `convert` (docs/plans/phone-clip-convert.md): a bounded cache of
// converted segments and the one worker that makes them. The conversion itself is the
// host's `runner` (a helper process on the graphics hardware); here is only what to make,
// in what order, and where it is kept.
//
// One job at a time across every session. A segment a phone asks for goes before the ones
// made ahead (the next `ahead` after it, from the second segment asked: the first alone is
// as often a poster), and asking for another segment drops what was queued ahead of the old
// position or for another clip (a seek, or the next clip). The cache is a folder of
// folders named by a hash of the source (its folder id, relative path, size and mtime and
// kFormat), so an edited file converts again and the names say nothing of the library.
// Oldest-used segments go past `max_bytes`. Nothing is ever written beside an original.
//
// Callbacks run on the worker thread, never under a lock of this service's. Portable (D9).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "wire/serve_wire.h"

namespace mv::driftwire::serve::convert {

inline constexpr std::uint32_t kSegmentMs = 4000;
inline constexpr int kAhead = 2;
inline constexpr std::uint64_t kDefaultCacheBytes = 2ull << 30;
// Bumped when the converted output changes (encoder settings): old entries become misses.
inline constexpr int kFormat = 1;

struct source {
  std::string path;     // absolute, for the runner; checked beneath its folder by the caller
  std::string root_id;  // with rel, size and mtime: the cache key
  std::string rel;
  std::uint64_t size = 0;
  std::int64_t mtime = 0;
};

// What a runner returns.
enum class ran : std::uint8_t { ok, not_a_clip, failed };

struct runner {
  // Writes the plan's JSON (serve_wire.h convert_plan_json's shape) to `out`.
  std::function<ran(const std::string& src, std::uint32_t segment_ms, const std::string& out)> plan;
  // Writes segment k (MPEG-TS) to `out`.
  std::function<ran(const std::string& src, std::uint32_t k, std::uint32_t segment_ms, const std::string& out)> segment;
};

enum class outcome : std::uint8_t { ok, not_a_clip, failed, off };

struct stats {
  std::uint64_t made = 0, hits = 0, failed = 0;
  std::uint64_t cached_bytes = 0;
  bool busy = false;  // a job running now
};

class service {
 public:
  struct options {
    std::string cache_dir;  // made if missing (owner-only)
    std::uint64_t max_bytes = kDefaultCacheBytes;
    runner run;
    int ahead = kAhead;
    std::uint32_t segment_ms = kSegmentMs;
  };
  [[nodiscard]] static std::unique_ptr<service> start(options o);
  // Stops the worker after the job running now; every waiting callback gets outcome::off.
  ~service();
  service(const service&) = delete;
  service& operator=(const service&) = delete;

  using plan_cb = std::function<void(outcome, const convert_plan&)>;
  using segment_cb = std::function<void(outcome, const std::string& file)>;
  // From the cache at once (on this thread), else queued.
  void plan(const source& s, plan_cb done);
  // `file` stays at least until the next segment is made past the cache's bound.
  void segment(const source& s, std::uint32_t k, segment_cb done);
  // Empties the cache (the off switch); jobs queued are dropped (outcome::off).
  void purge();
  [[nodiscard]] stats now() const;

 private:
  service() = default;
  struct state;
  std::shared_ptr<state> st_;
};

// The cache folder's name for a source: 32 lowercase hex digits.
[[nodiscard]] std::string cache_key(const source& s);

}  // namespace mv::driftwire::serve::convert
