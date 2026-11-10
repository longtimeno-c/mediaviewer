// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// One installed generation of the phone's catalogue (docs/plans/mobile.md §5): its manifest
// checked against the files, catalog.db open read-only, tiny.pack mapped, and the rows the grid
// reads kept in one flat array so a large library is not copied into Swift values (§9's budget).
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/result.h"

struct sqlite3;

namespace mv::mobile {

struct model_file {
  std::string name;  // "text.onnx"
  std::string path;  // absolute
  std::uint64_t size = 0;
};

struct manifest {
  std::uint64_t generation = 0;
  std::int64_t created_unix = 0;
  std::string active_spec;
  std::string model_spec;
  std::vector<model_file> models;
  struct root {
    std::string uuid, share, sub;
  };
  std::vector<root> roots;
  std::map<std::string, std::uint64_t> files;  // name -> size
};

struct row {
  std::int64_t id = 0;
  std::int64_t wall_seconds = 0;
  std::int64_t duration_ms = 0;
  std::uint32_t name_offset = 0;
  std::uint32_t folder = 0;
  std::uint16_t name_length = 0;
  std::uint8_t kind = 0;  // 1 photo, 2 clip: the only kinds a phone's grid shows
};

struct folder {
  std::string key;
  std::string name;
  std::int32_t parent = -1;
  std::uint32_t items = 0;
  bool has_subfolders = false;
};

struct month {
  std::int32_t key = 0;  // yyyymm
  std::int64_t wall_seconds = 0;
  std::uint32_t first = 0;
  std::uint32_t count = 0;
};

// A library's order, or a part of it: indices into catalogue::rows().
struct listing {
  std::vector<std::uint32_t> indices;
  std::vector<month> months;
};

struct person {
  std::string key, name;
  std::uint32_t items = 0;
  std::int64_t face_asset = 0;
};

struct details {
  std::string place, camera, lens, folder, format;
  std::int64_t file_bytes = 0;
  std::int32_t iso = 0, aperture_x100 = 0, focal_mm = 0, shutter_den = 0;
  std::int64_t shutter_us = 0;
};

struct filter_options {
  std::vector<std::string> places, cameras;
  std::vector<std::int32_t> years;
};

// Reads the shape of a JPEG from its header; nullopt when `jpeg` is not one.
[[nodiscard]] std::optional<std::pair<std::uint16_t, std::uint16_t>> jpeg_size(std::span<const std::uint8_t> jpeg);

class catalogue {
 public:
  ~catalogue();
  catalogue(const catalogue&) = delete;
  catalogue& operator=(const catalogue&) = delete;

  // status::corrupt: a file is not the size the manifest says.
  [[nodiscard]] static result<std::unique_ptr<catalogue>> open(const std::string& generation_dir,
                                                               const std::string& library_dir);

  [[nodiscard]] const manifest& info() const noexcept { return manifest_; }
  [[nodiscard]] const std::string& dir() const noexcept { return dir_; }
  [[nodiscard]] std::span<const row> rows() const noexcept { return rows_; }
  [[nodiscard]] std::string_view name_of(const row& r) const noexcept {
    return std::string_view(names_).substr(r.name_offset, r.name_length);
  }
  // nullptr when the id is not one of the rows.
  [[nodiscard]] const row* row_of(std::int64_t id) const noexcept;
  [[nodiscard]] std::uint32_t index_of(const row* r) const noexcept {
    return static_cast<std::uint32_t>(r - rows_.data());
  }

  [[nodiscard]] std::shared_ptr<const listing> everything() const noexcept { return all_; }
  [[nodiscard]] result<std::shared_ptr<const listing>> in_folder(std::string_view key) const;
  [[nodiscard]] result<std::shared_ptr<const listing>> of_person(std::string_view key) const;

  // Children of `parent` (empty: the top level), in name order.
  [[nodiscard]] result<std::vector<folder>> folders_under(std::string_view parent) const;
  [[nodiscard]] result<std::vector<person>> people() const;
  [[nodiscard]] std::uint32_t people_count() const;
  [[nodiscard]] result<details> details_of(std::int64_t id) const;
  [[nodiscard]] filter_options filters() const;
  // Which transcript the catalogue holds, "" for none: speech needs no model.
  [[nodiscard]] std::string speech_spec() const;
  // The asset id at this path in the catalogue, for the engine's tile calls.
  [[nodiscard]] std::optional<std::int64_t> id_at(const std::string& path) const;

  [[nodiscard]] bool has_tiny() const noexcept { return tiny_ != nullptr; }
  [[nodiscard]] std::optional<std::span<const std::uint8_t>> tile(std::int64_t id) const;
  void fill_aspects();
  [[nodiscard]] std::pair<std::uint16_t, std::uint16_t> aspect(const row& r) const noexcept;

 private:
  catalogue() = default;
  [[nodiscard]] expected read_rows();
  [[nodiscard]] listing list_of(std::vector<std::uint32_t> indices) const;
  [[nodiscard]] std::uint32_t folder_of_dir(std::string_view dir);

  std::string dir_;
  manifest manifest_;
  sqlite3* db_ = nullptr;
  sqlite3* faces_ = nullptr;
  std::vector<row> rows_;
  std::string names_;
  std::vector<std::pair<std::int64_t, std::uint32_t>> by_id_;
  std::vector<folder> folders_;
  std::unordered_map<std::string, std::uint32_t> folder_by_key_;
  std::shared_ptr<const listing> all_;
  const std::uint8_t* tiny_ = nullptr;
  std::size_t tiny_size_ = 0;
  std::unique_ptr<std::atomic<std::uint32_t>[]> aspects_;  // width << 16 | height, by row
  std::atomic<bool> aspects_filled_{false};
};

}  // namespace mv::mobile
