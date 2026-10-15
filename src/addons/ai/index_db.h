// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// index.db (docs/design/17 "Index store"): remembered roots, the assets under them,
// per-model progress (resumable per asset), and the sampled frames with their
// embeddings. A separate SQLite file in the add-on's data folder, so clearing
// the index can never touch the thumbnail cache, and vice versa.
//
//   roots(id, path, recursive, enabled, last_scan_at)
//   assets(id, path, root_id, mtime, size, kind, duration_ms, seen, cloud)   key: path;
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
// Schema 4 (#291, docs/design/17 "Schema 4"): assets gain tags (0 until read),
// taken_at, lat, lon, camera, lens, made_with, ai, and place / country /
// region (NULL: place names come from the Places add-on at query time), and
// an FTS5 asset_text(rowid = asset id, text = the folded search fields). One
// transaction, as every step: a kill in it leaves schema 3.
//
// Schema 5 (#405): assets gain rating, label, iso, aperture_x100, focal_mm,
// shutter_den and shutter_us (NULL until read). `tags` becomes a level: 1 =
// schema 4's facts read, 2 = these too (kTagsLevel), so the backfill reads a
// schema-4 row once more for them and never a row that has them.
//
// Schema 6 (#406): cues(id, asset_id, start_ms, end_ms, kind, text), a clip's
// own chapter titles and text subtitle lines. No spec: the text is the
// file's, not a model's; a changed (mtime, size) drops the asset's rows. Its
// progress is a progress row under kCueSpec, so a kill resumes it.
//
// docs/design/17 sketches one assets row per (path, mtime, size, spec); progress is
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
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
  // Roots held until the user resumes them (issue #299, engine_options::
  // hold_unfinished): left out of the work and of the counts.
  std::vector<std::int64_t> held;
};

// #406: the progress spec of the cue pass (not a model: there is no vector).
inline constexpr const char* kCueSpec = "cues/1";
enum class cue_kind : std::uint8_t { chapter = 1, subtitle = 2 };
struct cue_in {
  std::int64_t start_ms = 0;
  std::int64_t end_ms = 0;
  cue_kind kind = cue_kind::subtitle;
  std::string text;
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
  bool cloud = false;  // only a cloud provider has it (schema 3): never read until fetched
};

// What the tag backfill has read of an asset (assets.tags): 0 nothing, 1 the
// schema 4 facts, 2 (#405, schema 5) rating, label and exposure too.
inline constexpr int kTagsLevel = 2;

// #291 (schema 4): what an asset's tags say, as the host's tag_facts gave
// them. `text` is the search fields folded (core/fold.h) and joined by spaces,
// as asset_text holds it.
struct asset_tags {
  std::int64_t id = 0;
  int level = kTagsLevel;  // what was read (an older host: 1, without the #405 facts)
  std::int64_t mtime = 0;  // the stamp the facts were read at
  std::uint64_t size = 0;
  std::optional<std::int64_t> taken_at;  // date taken read as UTC
  bool has_where = false;
  double lat = 0, lon = 0;
  std::string camera, lens, made_with;
  bool ai = false;
  std::string text;
  // #405 (level 2). Absent: nullopt / empty / 0.
  std::optional<int> rating;  // 1..5, -1 rejected, 0 unrated
  std::string label;          // as written
  std::int32_t iso = 0;
  std::int32_t aperture_x100 = 0;
  std::int32_t focal_mm = 0;
  std::int32_t shutter_den = 0;
  std::int64_t shutter_us = 0;
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
  std::uint64_t cloud_only = 0;   // cloud files not yet indexed (OneDrive, iCloud Drive)
};

// int8 quantisation of an L2-normalised vector: v ~= q * scale.
void quantise(std::span<const float> v, std::vector<std::int8_t>& q, float& scale);

class index_db {
 public:
  ~index_db();
  index_db(const index_db&) = delete;
  index_db& operator=(const index_db&) = delete;

  [[nodiscard]] static result<std::unique_ptr<index_db>> open(const std::string& path_utf8);
  // Another process's view of the app's index (the search agent, docs/design/23):
  // SQLITE_OPEN_READONLY, no schema created or migrated, so every write this
  // connection could try fails (SQLITE_READONLY). WAL gives it a consistent
  // snapshot while the app goes on indexing. A missing file (nothing indexed
  // yet) is status::not_found; an index of another schema is
  // status::unsupported_format.
  [[nodiscard]] static result<std::unique_ptr<index_db>> open_read_only(const std::string& path_utf8);
  // PRAGMA data_version: changes when another connection commits. A reader
  // polls it to learn the app has indexed more.
  [[nodiscard]] std::int64_t data_version();
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
    bool cloud = false;  // its bytes are only in the cloud (host walk_files2)
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
  // pending(), limited to `ids`: the clips the iCloud fetch has downloaded,
  // which go to the front of the queue (2026-10-05).
  [[nodiscard]] std::vector<work_item> pending_among(const std::string& spec, std::span<const std::int64_t> ids,
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
  // The clips of `root` only iCloud has, newest first, and how many: what the
  // opt-in iCloud fetch downloads (2026-10-05).
  [[nodiscard]] std::vector<asset_row> unavailable_videos(std::int64_t root, std::size_t limit);
  [[nodiscard]] std::uint64_t unavailable_video_count(std::int64_t root);
  // These go back to pending, their stand-in rows (an iCloud-only clip's
  // poster) dropped: their originals have been downloaded since. The caller
  // drops them from the search matrix too (engine::forget_vectors).
  [[nodiscard]] expected requeue_unavailable(std::span<const std::int64_t> ids);
  // Cloud files (schema 3, 2026-10-05): assets of enabled roots only a cloud
  // provider has and not yet indexed, newest first, leaving out `skip`; their
  // count, overall and in a root. pending() never returns one: the opt-in
  // fetch brings it down and clears the mark (set_cloud) first.
  [[nodiscard]] std::vector<asset_row> cloud_assets(std::size_t limit, std::span<const std::int64_t> skip = {});
  [[nodiscard]] std::uint64_t cloud_count();
  [[nodiscard]] std::uint64_t cloud_in_root(std::int64_t root);
  [[nodiscard]] expected set_cloud(std::int64_t asset, bool cloud);
  // A clip's transcript segments, with progress, in one transaction.
  [[nodiscard]] expected commit_speech(std::int64_t asset, const std::string& spec,
                                       std::span<const speech_in> segments, work_state state,
                                       std::int64_t resume_ms);
  [[nodiscard]] expected each_speech(
      const std::string& spec,
      const std::function<void(std::int64_t asset, std::int64_t start_ms, std::int64_t end_ms,
                               const std::string& text)>& visit);
  // #406: a clip's cues, with progress under kCueSpec, in one transaction.
  // `replace_from` >= 0 first drops the asset's cues starting at or after it
  // (a resumed pass sends them again).
  [[nodiscard]] expected commit_cues(std::int64_t asset, std::span<const cue_in> cues, work_state state,
                                     std::int64_t resume_ms, std::int64_t replace_from = -1);
  [[nodiscard]] expected each_cue(
      const std::function<void(std::int64_t asset, const cue_in& cue)>& visit);
  // Resets `spec`'s progress for every asset (a spec change, a clear).
  [[nodiscard]] expected drop_spec(const std::string& spec);

  // ---- reads --------------------------------------------------------------------
  [[nodiscard]] counts count(const std::string& spec, const track_filter& filter = {});
  [[nodiscard]] std::uint64_t frames_in_root(std::int64_t root, const std::string& spec);
  [[nodiscard]] std::uint64_t assets_in_root(std::int64_t root);
  [[nodiscard]] std::uint64_t done_in_root(std::int64_t root, const std::string& spec);
  // How many assets of `root` pending() would still return for `spec` (the
  // filter's held roots aside): 0 when the root's indexing is finished.
  [[nodiscard]] std::uint64_t pending_in_root(std::int64_t root, const std::string& spec, std::int32_t max_tries,
                                              const track_filter& filter = {});
  [[nodiscard]] std::uint64_t unavailable_in_root(std::int64_t root, const std::string& spec);
  // Every stored frame of `spec` with an id above `after_id`, streamed in id
  // order (a reader catching up takes only what was committed since).
  [[nodiscard]] expected each_frame(const std::string& spec,
                                    const std::function<void(const frame_out&)>& visit,
                                    std::int64_t after_id = 0);
  [[nodiscard]] expected each_frame_of(std::int64_t asset, const std::string& spec,
                                       const std::function<void(const frame_out&)>& visit);
  [[nodiscard]] std::vector<asset_row> all_assets();
  [[nodiscard]] result<asset_row> asset_by_path(const std::string& path);
  [[nodiscard]] result<asset_row> asset_by_id(std::int64_t id);
  // ---- tags (#291, schema 4) -----------------------------------------------------
  // Assets of enabled roots whose tags are read below `level` (tags < level;
  // cloud-only ones wait for their bytes), oldest first: the backfill's next
  // batch.
  [[nodiscard]] std::vector<asset_row> assets_needing_tags(std::size_t limit, int level = kTagsLevel);
  [[nodiscard]] std::uint64_t tags_pending(int level = kTagsLevel);
  // Stores a batch in one transaction; a row whose asset's stamp moved since
  // the read is dropped (its tags are read again).
  [[nodiscard]] expected commit_tags(std::span<const asset_tags> rows);
  // Every asset with tags read (any level), for the engine's memory at start.
  [[nodiscard]] expected each_tags(const std::function<void(const asset_tags&)>& visit);
  // An FTS5 MATCH over asset_text (empty when the index has no FTS5). For a
  // reader that does not keep the text in memory. `words` is what was typed:
  // each word is matched as a quoted string (fts_phrases), never parsed as
  // FTS5 query syntax.
  [[nodiscard]] std::vector<std::int64_t> match_text(const std::string& words, std::size_t limit);
  // `typed` as an FTS5 query of quoted strings, one per word, ANDed.
  [[nodiscard]] static std::string fts_phrases(std::string_view typed);

  // Specs that have rows (a half-migrated index has two).
  [[nodiscard]] std::vector<std::string> specs();

  // ---- housekeeping ---------------------------------------------------------------
  [[nodiscard]] std::string meta(const std::string& key);
  [[nodiscard]] expected set_meta(const std::string& key, const std::string& value);
  [[nodiscard]] expected clear();       // every row, then VACUUM: frees the disk
  [[nodiscard]] std::uint64_t bytes();  // the file plus its WAL
  // Sharing an index (transfer.h): `fn` runs on this file's connection with
  // its lock held, so no other call interleaves with an import.
  [[nodiscard]] expected with_connection(const std::function<expected(sqlite3*)>& fn);

 private:
  index_db() = default;
  bool exec(const char* sql);
  std::vector<work_item> pending_where(const std::string& spec, std::size_t limit, std::int32_t max_tries,
                                       const track_filter& filter, const std::string& extra);
  std::mutex m_;
  sqlite3* db_ = nullptr;
  std::string path_;
};

}  // namespace mv::ai
