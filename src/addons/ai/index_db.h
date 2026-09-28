// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// index.db (plan/17 "Index store"): remembered roots, the assets under them,
// per-model progress (resumable per asset), and the sampled frames with their
// embeddings. A separate SQLite file in the add-on's data folder, so clearing
// the index can never touch the thumbnail cache, and vice versa.
//
//   roots(id, path, recursive, enabled, last_scan_at)
//   assets(id, path, root_id, mtime, size, kind, duration_ms, seen)   key: path;
//        a changed (mtime, size) drops the asset's frames and re-queues it
//   progress(asset_id, spec, state, resume_ms, tries, indexed_at)     key: (asset, spec)
//   frames(id, asset_id, spec, pts_ms, pts_tb, tb_num, tb_den, flags, generic, scale, emb)
//   speech(id, asset_id, spec, start_ms, end_ms, text)                  -- schema 2 (audio)
//   meta(key, value)
//
// Photos library (issue #72, macOS): a root whose path is "photos:" holds the
// system library's assets, each keyed "photos:<localIdentifier>"
// (photos_source.h). No schema change: the key scheme is the source. Its
// progress may be `unavailable` (4): only iCloud has the asset, so there was
// nothing local to embed; it is neither done nor failed, and it re-queues once
// the engine finds the original on this Mac (unavailable_assets, then
// requeue_unavailable for those).
//
// Schema 2 (2026-09-27, audio): roots.media says what a folder's videos are
// indexed for (MV_AI_MEDIA_PICTURES / _SOUND; 0 follows the default), sound
// embeddings are frames rows under the CLAP spec, and transcripts live in
// `speech`. A schema-1 index is upgraded in place.
//
// plan/17 sketches one assets row per (path, mtime, size, spec); progress is
// that row's per-model half, so two models' vectors live side by side while
// a quality change migrates (PR 23: "old index kept until the new one
// completes"). `emb` is int8 with a per-vector scale (0.5 KB a frame at 512
// dims; the PR 20 spike measured <= 0.5 recall points lost). Paths stay in
// this file on this machine and never reach a log (rule 6). One connection,
// serialised; every call is a worker-thread call.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "core/result.h"

struct sqlite3;

namespace mv::ai {

enum class asset_kind : std::uint8_t { photo = 1, video = 2 };
enum class work_state : std::uint8_t { pending = 0, partial = 1, done = 2, failed = 3, unavailable = 4 };

struct root_row {
  std::int64_t id = 0;
  std::string path;
  bool recursive = false;
  bool enabled = true;
  std::int64_t last_scan_at = 0;
  std::uint32_t media = 0;  // MV_AI_MEDIA_* for its videos; 0 = the default
};

// Which assets a work track covers: photos (the picture track only), and
// videos whose root's media (or the default, for 0) includes `media_bit`.
struct track_filter {
  bool photos = true;
  std::uint32_t media_bit = 1;
  std::uint32_t default_media = 1;
  std::int64_t skip_root = 0;  // a root to leave out (the Photos library while access is off)
};

struct speech_in {
  std::int64_t start_ms = 0;
  std::int64_t end_ms = 0;
  std::string text;
};

struct asset_row {
  std::int64_t id = 0;
  std::string path;
  std::int64_t root_id = 0;
  std::int64_t mtime = 0;
  std::uint64_t size = 0;
  asset_kind kind = asset_kind::photo;
  std::int64_t duration_ms = 0;
};

// One embedding to store. `emb` is float and L2-normalised; the store
// quantises it.
struct frame_in {
  std::int64_t pts_ms = -1;  // -1: a photo
  std::int64_t pts_tb = 0;
  std::int32_t tb_num = 0;
  std::int32_t tb_den = 1;
  std::uint32_t flags = 0;
  float generic = 0;         // best cosine against the model's generic prompts
  std::span<const float> emb;
};

// One stored embedding as the search matrix reads it.
struct frame_out {
  std::int64_t id = 0;
  std::int64_t asset_id = 0;
  std::int64_t pts_ms = -1;
  float generic = 0;
  float scale = 0;
  std::span<const std::int8_t> emb;
};

struct work_item {
  asset_row asset;
  work_state state = work_state::pending;
  std::int64_t resume_ms = 0;
  std::int32_t tries = 0;
};

struct counts {
  std::uint64_t assets = 0;   // in enabled roots
  std::uint64_t done = 0;     // done for `spec`
  std::uint64_t failed = 0;
  std::uint64_t frames = 0;   // rows for `spec`
  std::uint64_t pending_video_ms = 0;  // remaining clip time, for the ETA
  std::uint64_t pending_photos = 0;
  std::uint64_t unavailable = 0;  // only in iCloud (the Photos library)
};

// int8 quantisation of an L2-normalised vector: v ~= q * scale.
void quantise(std::span<const float> v, std::vector<std::int8_t>& q, float& scale);

class index_db {
 public:
  ~index_db();
  index_db(const index_db&) = delete;
  index_db& operator=(const index_db&) = delete;

  [[nodiscard]] static result<std::unique_ptr<index_db>> open(const std::string& path_utf8);
  [[nodiscard]] const std::string& path() const noexcept { return path_; }

  // ---- roots ------------------------------------------------------------------
  [[nodiscard]] std::vector<root_row> roots();
  [[nodiscard]] result<std::int64_t> add_root(const std::string& path, bool recursive);
  [[nodiscard]] expected set_root_enabled(std::int64_t id, bool enabled);
  [[nodiscard]] expected set_root_recursive(std::int64_t id, bool recursive);
  [[nodiscard]] expected set_root_media(std::int64_t id, std::uint32_t media);
  // Deletes the root, its assets, their progress and frames.
  [[nodiscard]] expected remove_root(std::int64_t id);
  // Moves every asset of `from` to `to` and deletes `from` (a new tree root
  // that swallows an older one).
  [[nodiscard]] expected merge_root(std::int64_t from, std::int64_t to);
  [[nodiscard]] expected touch_root(std::int64_t id, std::int64_t when);

  // ---- a scan (the delta) -----------------------------------------------------
  struct upsert {
    std::int64_t id = 0;
    bool added = false;
    bool changed = false;  // (mtime, size) differed: frames and progress dropped
  };
  struct seen_file {
    std::string path;
    std::int64_t mtime = 0;
    std::uint64_t size = 0;
    asset_kind kind = asset_kind::photo;
  };
  // Records a batch of files found under `root` in scan `generation`, in one
  // transaction. The result is in the batch's order.
  [[nodiscard]] result<std::vector<upsert>> see_assets(std::int64_t root,
                                                       std::span<const seen_file> files,
                                                       std::int64_t generation);
  // Deletes `root`'s assets not seen in `generation` (removed files). Returns
  // their ids so the search matrix can drop them.
  [[nodiscard]] result<std::vector<std::int64_t>> end_scan(std::int64_t root, std::int64_t generation);
  [[nodiscard]] std::int64_t next_generation();

  // ---- work ---------------------------------------------------------------------
  // Assets of enabled roots with no done / failed progress for `spec` (and
  // failed ones with tries < max_tries): partial clips first, then photos,
  // then clips. The indexer puts the folder on screen first.
  [[nodiscard]] std::vector<work_item> pending(const std::string& spec, std::size_t limit,
                                               std::int32_t max_tries, const track_filter& filter = {});
  [[nodiscard]] expected set_duration(std::int64_t asset, std::int64_t duration_ms);
  // Stores frames and moves progress in one transaction (a kill between two
  // commits resumes at `resume_ms`; nothing committed is redone).
  [[nodiscard]] expected commit_frames(std::int64_t asset, const std::string& spec,
                                       std::span<const frame_in> frames, work_state state,
                                       std::int64_t resume_ms, std::uint32_t dim);
  [[nodiscard]] expected fail(std::int64_t asset, const std::string& spec);
  // Nothing local to read in full (an iCloud-only Photos asset): not a failure,
  // not retried until requeue_unavailable. `frames` may hold what was local (an
  // iCloud-only clip's poster), searchable meanwhile.
  [[nodiscard]] expected mark_unavailable(std::int64_t asset, const std::string& spec,
                                          std::span<const frame_in> frames = {}, std::uint32_t dim = 0);
  // The assets of `root` that some track found unavailable, for the engine to
  // ask whether they are on this Mac now.
  [[nodiscard]] std::vector<asset_row> unavailable_assets(std::int64_t root);
  // These go back to pending, their stand-in rows (an iCloud-only clip's
  // poster) dropped: their originals have been downloaded since. The caller
  // drops them from the search matrix too (engine::forget_vectors).
  [[nodiscard]] expected requeue_unavailable(std::span<const std::int64_t> ids);
  // A clip's transcript segments, with progress, in one transaction.
  [[nodiscard]] expected commit_speech(std::int64_t asset, const std::string& spec,
                                       std::span<const speech_in> segments, work_state state,
                                       std::int64_t resume_ms);
  [[nodiscard]] expected each_speech(
      const std::string& spec,
      const std::function<void(std::int64_t asset, std::int64_t start_ms, std::int64_t end_ms,
                               const std::string& text)>& visit);
  // Resets `spec`'s progress for every asset (a spec change, a clear).
  [[nodiscard]] expected drop_spec(const std::string& spec);

  // ---- reads --------------------------------------------------------------------
  [[nodiscard]] counts count(const std::string& spec, const track_filter& filter = {});
  [[nodiscard]] std::uint64_t frames_in_root(std::int64_t root, const std::string& spec);
  [[nodiscard]] std::uint64_t assets_in_root(std::int64_t root);
  [[nodiscard]] std::uint64_t done_in_root(std::int64_t root, const std::string& spec);
  [[nodiscard]] std::uint64_t unavailable_in_root(std::int64_t root, const std::string& spec);
  // Every stored frame of `spec`, streamed.
  [[nodiscard]] expected each_frame(const std::string& spec,
                                    const std::function<void(const frame_out&)>& visit);
  [[nodiscard]] expected each_frame_of(std::int64_t asset, const std::string& spec,
                                       const std::function<void(const frame_out&)>& visit);
  [[nodiscard]] std::vector<asset_row> all_assets();
  [[nodiscard]] result<asset_row> asset_by_path(const std::string& path);
  [[nodiscard]] result<asset_row> asset_by_id(std::int64_t id);
  // Specs that have rows (a half-migrated index has two).
  [[nodiscard]] std::vector<std::string> specs();

  // ---- housekeeping ---------------------------------------------------------------
  [[nodiscard]] std::string meta(const std::string& key);
  [[nodiscard]] expected set_meta(const std::string& key, const std::string& value);
  [[nodiscard]] expected clear();       // every row, then VACUUM: frees the disk
  [[nodiscard]] std::uint64_t bytes();  // the file plus its WAL

 private:
  index_db() = default;
  bool exec(const char* sql);
  result<upsert> see_one(std::int64_t root, const seen_file& f, std::int64_t generation);
  std::mutex m_;
  sqlite3* db_ = nullptr;
  std::string path_;
};

}  // namespace mv::ai
