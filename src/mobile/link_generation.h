// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The installed generation as the link side reads it: tiny.pack, thumbs.idx and
// the catalogue's paths. Search has its own view of the same files (§9).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "addons/ai/catalog.h"
#include "core/result.h"

struct sqlite3;

namespace mv::mobile {

struct asset_file {
  std::string uuid;  // catalogue root id, the first part of assets.path
  std::string rel;   // relative to it, in the file's real bytes
  std::uint64_t size = 0;
  int kind = 0;
  std::int64_t duration_ms = 0;
};

struct moment_row {
  std::int64_t start_ms = 0;
  int kind = 0;  // 0 speech, 1 scene, 2 words on screen
  std::string text;
};

class generation_view {
 public:
  ~generation_view();
  [[nodiscard]] static result<std::unique_ptr<generation_view>> open(const std::string& dir);

  // Valid while this object lives. Any corrupt pack is reported as empty.
  [[nodiscard]] std::optional<std::span<const std::uint8_t>> tiny(std::int64_t asset) const;
  [[nodiscard]] std::optional<ai::catalog::thumb_location> thumb(std::int64_t asset) const;
  [[nodiscard]] std::optional<asset_file> file(std::int64_t asset) const;
  [[nodiscard]] std::vector<moment_row> moments(std::int64_t asset) const;

  // Until the search session's library store lands: every photo and clip, newest first.
  struct listed_asset {
    std::int64_t id = 0;
    int kind = 0;
    std::int64_t taken_at = 0;
    std::int64_t duration_ms = 0;
    std::string name;
  };
  void for_each_asset(const std::function<void(const listed_asset&)>& fn) const;

  [[nodiscard]] std::uint64_t created() const noexcept { return created_; }
  [[nodiscard]] std::uint64_t asset_count() const noexcept { return assets_; }
  [[nodiscard]] const std::string& dir() const noexcept { return dir_; }

  // uuid → (share, sub) from the manifest.
  struct root_ref {
    std::string share;
    std::string sub;
  };
  [[nodiscard]] const std::map<std::string, root_ref>& roots() const noexcept { return roots_; }

 private:
  generation_view() = default;
  struct mapped;
  std::unique_ptr<mapped> tiny_, thumbs_;
  sqlite3* db_ = nullptr;
  std::string dir_;
  std::uint64_t created_ = 0, assets_ = 0;
  std::map<std::string, root_ref> roots_;
};

}  // namespace mv::mobile
