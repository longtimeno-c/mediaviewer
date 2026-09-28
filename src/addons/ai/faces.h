// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// People (plan/17 PR 24): face vectors, their clusters, and the corrections a
// user makes, in their OWN file (faces.db, beside index.db) so one click
// deletes every face vector while the frame index stays intact.
//
//   faces(id, asset_id, path, pts_ms, x, y, w, h, score, person_id, emb)
//   people(id, name, created_at)
//   rejected(face_id, person_id)     -- "not this person": never rejoins it
//   no_merge(a, b)                   -- a split: never merged back automatically
//   scanned(asset_id, spec)          -- assets whose faces are done
//
// Boxes are fractions of the image (0..1), so a result can outline a face on
// any thumbnail without a crop ever being written to disk. Clustering is
// online: a face joins the nearest person whose centroid is within
// `same_person`, else starts one; an idle pass merges near-duplicate unnamed
// clusters. Nothing here leaves the machine or reaches a log (rule 6), and a
// crash report never carries this file (plan/13; the minidump filter excludes
// the add-on's heaps).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/result.h"

struct sqlite3;

namespace mv::ai {

struct face_in {
  float x = 0, y = 0, w = 0, h = 0;  // 0..1 of the image
  float score = 0;
  std::vector<float> emb;            // L2-normalised
};

struct face_row {
  std::int64_t id = 0;
  std::int64_t asset = 0;
  std::string path;
  std::int64_t pts_ms = -1;
  float x = 0, y = 0, w = 0, h = 0;
  float score = 0;
  std::int64_t person = 0;
};

struct person_row {
  std::int64_t id = 0;
  std::string name;
  std::uint32_t faces = 0;
  face_row cover;  // the clearest face
};

class faces_db {
 public:
  ~faces_db();
  faces_db(const faces_db&) = delete;
  faces_db& operator=(const faces_db&) = delete;

  [[nodiscard]] static result<std::unique_ptr<faces_db>> open(const std::string& path_utf8,
                                                              float same_person, std::uint32_t dim);
  // Closes and deletes the file and its WAL: every face vector, box and name.
  static void destroy(const std::string& path_utf8);

  [[nodiscard]] bool scanned(std::int64_t asset, const std::string& spec);
  [[nodiscard]] std::vector<std::int64_t> scanned_assets(const std::string& spec);
  // Stores an asset's faces (a photo, or one clip frame) and clusters them.
  [[nodiscard]] expected add(std::int64_t asset, const std::string& path, std::int64_t pts_ms,
                             std::span<const face_in> faces);
  [[nodiscard]] expected mark_scanned(std::int64_t asset, const std::string& spec);
  [[nodiscard]] expected forget_asset(std::int64_t asset);

  [[nodiscard]] std::vector<person_row> people(std::uint32_t min_faces);
  [[nodiscard]] std::vector<face_row> faces_of(std::int64_t person);
  [[nodiscard]] result<face_row> face(std::int64_t id);
  [[nodiscard]] expected rename(std::int64_t person, const std::string& name);
  [[nodiscard]] expected merge(std::int64_t into, std::int64_t from);
  [[nodiscard]] expected reject(std::int64_t face);
  [[nodiscard]] result<std::int64_t> split(std::span<const std::int64_t> faces);
  // Named people whose name matches (case-insensitive, whole name).
  [[nodiscard]] std::vector<std::int64_t> people_named(const std::string& name);
  // Every named person, (id, name): the query language's names (query.h).
  [[nodiscard]] std::vector<std::pair<std::int64_t, std::string>> names();
  // The person nearest a face vector, if within `same_person`.
  [[nodiscard]] std::int64_t nearest_person(std::span<const float> emb);
  // The idle pass: merges unnamed clusters whose centroids sit closer than
  // `merge_at` (unless a split said not to). Returns how many merged.
  [[nodiscard]] std::size_t consolidate(float merge_at);

  [[nodiscard]] std::uint64_t face_count();
  [[nodiscard]] std::uint32_t person_count(std::uint32_t min_faces);

 private:
  faces_db() = default;
  bool exec(const char* sql);
  void load_locked();
  std::int64_t assign_locked(std::span<const float> emb, const std::set<std::int64_t>& rejected);
  void recompute_locked(std::int64_t person);

  struct cluster {
    std::vector<float> sum;  // unnormalised
    std::uint32_t n = 0;
  };
  std::mutex m_;
  sqlite3* db_ = nullptr;
  std::string path_;
  float same_ = 0.4f;
  std::uint32_t dim_ = 128;
  std::map<std::int64_t, cluster> clusters_;
};

}  // namespace mv::ai
