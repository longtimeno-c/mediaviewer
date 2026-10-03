// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// People (plan/17 PR 24): face vectors, their clusters, and the corrections a
// user makes, in their OWN file (faces.db, beside index.db) so one click
// deletes every face vector while the frame index stays intact.
//
//   faces(id, asset_id, path, pts_ms, x, y, w, h, score, person_id, emb,
//         pinned, quality, tta, spec) -- pinned: the user put it there;
//                                        spec: the embedder of emb
//   meta(key, value)                 -- 'rerun': a re-run is under way
//   people(id, name, created_at)
//   rejected(face_id, person_id)     -- "not this person": never rejoins it
//   no_merge(a, b)                   -- a split: never merged back automatically
//   scanned(asset_id, spec)          -- assets whose faces are done
//
// Boxes are fractions of the image (0..1), so a result can outline a face on
// any thumbnail without a crop ever being written to disk. Clustering is
// online: a face joins the person whose members it resembles on average by
// `same_person` (pairwise cosine, the units SFace's threshold is quoted in),
// unless a second person is as close (then nobody, for now); an idle pass
// merges near-duplicate unnamed clusters by the same average; and the
// refinement (face_refine.h) re-checks every face against its person's core,
// evicting outliers to unassigned and moving clear mistakes, in passes, with
// the user's own faces (split, and the cover they named or merged) pinned as
// anchors it never moves. Nothing here leaves the machine or reaches a log (rule 6), and a
// crash report never carries this file (plan/13; the minidump filter excludes
// the add-on's heaps).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "addons/ai/face_refine.h"
#include "core/result.h"

struct sqlite3;

namespace mv::ai {

// An embedder's pairwise-cosine thresholds (infer::face_spec; plan/17
// "People model"). The defaults are SFace's.
struct face_tuning {
  float same_person = 0.40f;  // join a person; also the refinement's join and core
  float keep = 0.30f;         // the refinement's stay
  float keep_weak = 0.34f;
  float margin = 0.08f;       // a move beats own and runner-up by this
  float ambiguous = 0.03f;    // online: a runner-up this close leaves the face unassigned
  float merge_at = 0.42f;     // the idle merge (average linkage)
  [[nodiscard]] refine_params refine() const noexcept {
    refine_params p;
    p.join = same_person;
    p.core = same_person;
    p.keep = keep;
    p.keep_weak = keep_weak;
    p.margin = margin;
    return p;
  }
};

struct face_in {
  float x = 0, y = 0, w = 0, h = 0;  // 0..1 of the image
  float score = 0;
  float quality = -1;                // 0..1 (face_quality); < 0 unknown
  bool tta = false;                  // the embedding averages the face and its mirror
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
  bool pinned = false;
};

// One refinement call's input, taken under the lock and computed without it.
struct refine_snapshot {
  std::uint64_t serial = 0;  // the faces_db it came from
  std::uint64_t gen = 0;     // its touch generation when taken
  bool full = false;
  std::vector<float> emb;
  std::vector<refine_face> faces;
  std::vector<person_proto> fixed;
  std::vector<std::int64_t> named;
  std::set<std::int64_t> rebuilt;  // persons judged in full this call
};

struct refine_stats {
  std::uint32_t evicted = 0, moved = 0, admitted = 0, regrouped = 0, groups = 0;
  std::uint32_t skipped = 0;  // the user (or a scan) changed the face meanwhile
  std::vector<std::int64_t> recheck_assets;  // stills worth re-analysing (flip-averaged)
  [[nodiscard]] bool changed() const noexcept { return evicted + moved + admitted + regrouped > 0; }
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

  // `spec` names the embedder whose vectors this session adds and compares;
  // rows of another spec stay (and keep showing their person) until a re-run
  // re-analyses their asset. The short form is SFace's spec and thresholds.
  [[nodiscard]] static result<std::unique_ptr<faces_db>> open(const std::string& path_utf8,
                                                              const face_tuning& tuning, std::uint32_t dim,
                                                              const std::string& spec);
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

  // Every person with min_faces faces or a name. With `assets` (the open
  // folder's, plan/17 "People in the open folder"): only those with a face in
  // one of them, `faces` counting those faces and `cover` the clearest of them;
  // the min_faces / name rule still judges the whole person.
  [[nodiscard]] std::vector<person_row> people(std::uint32_t min_faces,
                                               const std::set<std::int64_t>* assets = nullptr);
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
  // The idle pass: merges clusters (not two named ones) whose faces resemble
  // each other by `merge_at` on average (pairwise cosine; unless a split said
  // not to). Returns how many merged.
  [[nodiscard]] std::size_t consolidate(float merge_at);

  // Refinement (plan/17 "People refinement"), in three steps so the compute
  // holds no lock: begin (a snapshot), refine_people (pure), commit (applies
  // what still holds: a face the user or a scan changed meanwhile is skipped).
  // Incremental unless `full` or nothing is cached yet: only persons whose
  // faces changed are rebuilt, the rest answer from cached prototypes.
  [[nodiscard]] bool refine_due();
  [[nodiscard]] bool refine_full_due();
  [[nodiscard]] refine_snapshot refine_begin(bool full);
  refine_stats refine_commit(const refine_snapshot& snap, const refine_output& out);
  // Forgets that a still was scanned so the People pass re-analyses it; the
  // new faces then replace the old ones box by box (add), keeping person and pin.
  [[nodiscard]] expected rescan(std::int64_t asset, const std::string& spec);
  [[nodiscard]] std::uint64_t serial() const noexcept { return serial_; }

  // Re-run (plan/17 "People model"): every asset is analysed again, new boxes
  // replacing old ones in place (person and pin kept), new faces waiting
  // unassigned; then the engine settles (a full refinement and merge) and
  // calls rerun_done. Pending from open too, when rows of another embedder
  // are present. Survives a restart (meta).
  [[nodiscard]] expected rerun_all();
  [[nodiscard]] bool rerun_pending();
  [[nodiscard]] std::uint64_t stale_count();       // faces of another embedder
  [[nodiscard]] std::uint64_t unassigned_count();  // this embedder's faces with no person
  void rerun_done();
  [[nodiscard]] const std::string& spec() const noexcept { return spec_; }
  // Sharing an index (transfer.h): `fn` writes on this file's connection with
  // its lock held; the clusters are then rebuilt from the rows and the next
  // refinement is a full one.
  [[nodiscard]] expected import_with(const std::function<expected(sqlite3*)>& fn);

  [[nodiscard]] std::uint64_t face_count();
  [[nodiscard]] std::uint32_t person_count(std::uint32_t min_faces);

 private:
  faces_db() = default;
  bool exec(const char* sql);
  bool migrate();
  void load_locked();
  std::int64_t assign_locked(std::span<const float> emb, const std::set<std::int64_t>& rejected);
  void recompute_locked(std::int64_t person);
  void pin_cover_locked(std::int64_t person);
  void touch_locked(std::int64_t person);
  void set_rerun_locked(bool on);

  struct cluster {
    std::vector<float> sum;  // unnormalised
    std::uint32_t n = 0;
  };
  std::mutex m_;
  sqlite3* db_ = nullptr;
  std::string path_;
  float same_ = 0.4f;
  float ambiguous_ = 0.03f;
  std::uint32_t dim_ = 128;
  std::string spec_;
  bool rerun_ = false;
  std::map<std::int64_t, cluster> clusters_;
  // Refinement state (in memory: rebuilt by the first, full, pass after open).
  std::set<std::int64_t> dirty_;               // persons whose faces changed
  std::set<std::int64_t> loose_;               // unassigned faces not yet judged
  std::map<std::int64_t, person_proto> protos_;  // cached, for clean persons
  std::map<std::int64_t, std::uint64_t> touched_at_;  // person -> touch generation
  std::uint64_t touch_gen_ = 0;
  std::set<std::int64_t> rechecked_;           // assets re-analysed this session
  bool full_due_ = true;
  std::uint32_t incremental_since_full_ = 0;
  std::uint64_t serial_ = 0;
};

}  // namespace mv::ai
