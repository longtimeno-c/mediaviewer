// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The AI pack's engine (plan/17, PRs 21-24): remembered roots and their delta
// scans, the background indexer and its yield policy, the search matrix, find
// similar, model-upgrade migration, and people. The models arrive through
// `engine_deps`, so the engine runs the same with the pack's ONNX Runtime
// towers (pack.cpp) and with the tests' deterministic fakes.
//
// Threads (plan/17 "Not hurting the viewer"):
//   control   loads models, scans roots, refreshes the status, merges people
//   workers   min(2, cores / 4), at least 1, OS background priority: embed
//   search    one, normal priority: queries answer while indexing runs
// None of them is the viewer's pool, and each waits between assets and
// between frames while the viewer is busy (should_yield), on battery below
// the threshold, or while the user has paused. Every public call returns
// without waiting on any of them ([no-block]) unless marked [worker-thread].
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <mediaviewer/mediaviewer_ai.h>

#include "addons/ai/faces.h"
#include "addons/ai/host.h"
#include "addons/ai/index_db.h"
#include "addons/ai/photos_source.h"
#include "addons/ai/platform.h"
#include "addons/ai/vectors.h"
#include "core/result.h"
#include "infer/audio_models.h"
#include "infer/models.h"

namespace mv::ai {

// What a CLIP tower brings besides its embeddings.
struct clip_meta {
  std::string name;          // "CLIP ViT-L/14"
  std::string spec_key;      // index key: model / precision / preprocessing
  std::uint32_t quality = 1; // mv_ai_quality
  std::uint32_t dim = 512;
  std::uint32_t input_edge = 224;
  float dedupe = 0.97f;
  float query_margin = 0.04f;
  float query_z = 0;  // 0: the margin alone decides "nothing found"
  float result_z = 2.0f;
  float result_margin = 0.015f;
  float similar_min = 0.62f;
  std::vector<std::string> generic_prompts;
};

struct loaded_clip {
  std::shared_ptr<infer::embedder> model;
  clip_meta meta;
  std::vector<std::vector<float>> generic;  // the generic prompts, embedded once
  infer::backend on = infer::backend::cpu;
  infer::provider_fault fault = infer::provider_fault::none;
};

// Faces: detect + embed one image. Boxes come back as fractions (face_in).
class face_analyzer {
 public:
  virtual ~face_analyzer() = default;
  [[nodiscard]] virtual result<std::vector<face_in>> analyze(const rgb_frame& img) = 0;
  [[nodiscard]] virtual const std::string& spec_key() const noexcept = 0;
  [[nodiscard]] virtual float same_person() const noexcept = 0;
  [[nodiscard]] virtual std::uint32_t dim() const noexcept = 0;
};

// Audio (plan/17 "Audio", 2026-09-27): what a clip sounds like, in the same
// vector space as a description of a sound (CLAP)...
class sound_model {
 public:
  virtual ~sound_model() = default;
  // One L2-normalised vector per window of 48 kHz mono PCM.
  [[nodiscard]] virtual expected embed_audio(std::span<const std::span<const float>> windows,
                                             std::vector<float>& out) = 0;
  [[nodiscard]] virtual result<std::vector<float>> embed_text(std::string_view utf8) = 0;
};

struct loaded_sound {
  std::shared_ptr<sound_model> model;
  std::string name;       // "LAION CLAP (general)"
  std::string spec_key;
  std::uint32_t dim = 512;
  std::uint32_t window_ms = 10000;
  std::uint32_t hop_ms = 5000;
  float dedupe = 0.97f;
  float query_margin = 0.04f;
  float result_margin = 0.015f;
  std::vector<std::string> generic_prompts;
  std::vector<std::vector<float>> generic;
};

// ...and what is said in it (Whisper): up to 30 s of 16 kHz PCM in, segments out.
class speech_model {
 public:
  virtual ~speech_model() = default;
  [[nodiscard]] virtual result<infer::speech_window> transcribe(std::span<const float> pcm,
                                                                std::int64_t start_ms) = 0;
};

struct loaded_speech {
  std::shared_ptr<speech_model> model;
  std::string name;
  std::string spec_key;
};

struct engine_deps {
  // Loads the runtime and reads the pack (seconds: ONNX Runtime and, with the
  // vendor piece, the CUDA libraries). The control thread calls it, after the
  // viewer is quiet. The informational calls below never load anything: they
  // answer empty until it has run, since Settings asks from the UI thread.
  std::function<void()> prepare;
  // The towers the pack carries (mv_ai_quality 1 and / or 2).
  std::function<std::vector<std::uint32_t>()> qualities;
  // Which accelerated backends exist in the loaded runtime.
  std::function<bool(infer::backend)> backend_available;
  // Opens a tower on a compute choice (mv_ai_compute). Auto self-tests the
  // provider against CPU and falls back (plan/17 "Runtime").
  std::function<result<loaded_clip>(std::uint32_t quality, std::uint32_t compute)> open_clip;
  // Null when the ai-faces piece is not installed.
  std::function<result<std::unique_ptr<face_analyzer>>()> open_faces;
  // Audio: an error when the ai-audio piece is not installed.
  std::function<result<loaded_sound>(std::uint32_t compute)> open_sound;
  std::function<result<loaded_speech>(std::uint32_t quality, std::uint32_t compute)> open_speech;
  // "CLIP ViT-B/32" for a quality, without opening it (Settings).
  std::function<std::string(std::uint32_t quality)> model_name;
  // The index key of a quality's tower, without opening it (sharing an
  // index: which Quality a file's vectors belong to). "" when not carried.
  std::function<std::string(std::uint32_t quality)> clip_spec;
  std::function<std::string()> runtime_version;
  // A vendor piece installed or removed since the runtime loaded: the
  // change needs the app to start again (the runtime cannot be swapped live).
  std::function<bool()> restart_needed;
  // The power source (plan/17 "Yield policy"); null reads the OS
  // (platform::power_state). The tests fake it.
  std::function<platform::power()> power;
  // Opening this tower on this compute choice compiles it for this machine
  // with nothing cached yet (Core ML's first compile: minutes). Cheap: a look
  // at the cache folder, after `prepare`. Null or false: an ordinary load.
  std::function<bool(std::uint32_t quality, std::uint32_t compute)> first_compile;
  // The system Photos library (issue #72): null uses make_photos_source()
  // (PhotoKit on macOS, none elsewhere). The tests pass a fake.
  std::function<std::unique_ptr<photos_source>()> photos;
  // At most one change-driven Photos library rescan per this many seconds
  // (PhotoKit reports an iCloud sync as a burst of changes).
  double photos_rescan_gap_s = 10;
};

struct settings {
  std::uint32_t compute = MV_AI_COMPUTE_AUTO;
  std::uint32_t quality = MV_AI_QUALITY_AUTO;
  int battery_percent = 30;                  // pause on battery below this
  std::uint64_t index_cap = 8'000'000'000;   // bytes; 0 = no cap
  bool faces = false;                        // the separate People opt-in
  std::uint32_t video_index = MV_AI_MEDIA_DEFAULT;  // 0: Pictures, plus Sound once ai-audio is in
  std::uint32_t precision = kPrecisionDefault;      // 0 broader .. 2 calibrated .. 4 stricter
};

// A spoken-phrase search's share of the query's words a transcript line must
// hold, at a Precision level: 2 as calibrated (a phrase of one or two words
// needs all of them, a longer one 60 %), 3-4 every word, 1 half of a phrase of
// three words or more, 0 half of any phrase of two or more.
[[nodiscard]] constexpr double speech_coverage_needed(std::size_t words, std::uint32_t precision) noexcept {
  if (precision >= 3) return 1.0;
  if (precision == 2) return words <= 2 ? 1.0 : 0.6;
  if (precision == 1) return words <= 2 ? 1.0 : 0.5;
  return words <= 1 ? 1.0 : 0.5;
}

class engine {
 public:
  engine(const mv_host_api* api, engine_deps deps);
  ~engine();
  engine(const engine&) = delete;
  engine& operator=(const engine&) = delete;

  [[nodiscard]] expected start();
  void stop() noexcept;

  // ---- state and settings ------------------------------------------------------
  void status(mv_ai_status& out) const;
  [[nodiscard]] std::string settings_json() const;
  [[nodiscard]] expected set_setting(const std::string& key, const std::string& value_json);
  void pause(bool paused);

  // ---- roots -------------------------------------------------------------------
  [[nodiscard]] std::string roots_json();                                  // [worker-thread]
  [[nodiscard]] result<std::int64_t> index_folder(const std::string& dir, bool recursive);
  [[nodiscard]] expected root_set_enabled(std::int64_t id, bool enabled);
  [[nodiscard]] expected root_rescan(std::int64_t id);
  [[nodiscard]] expected root_remove(std::int64_t id);
  [[nodiscard]] std::uint32_t folder_coverage(const std::string& dir) const;
  void note_folder_opened(const std::string& dir);
  [[nodiscard]] expected clear_index();
  [[nodiscard]] expected root_set_media(std::int64_t id, std::uint32_t media);

  // ---- the Photos library (issue #72; macOS) --------------------------------
  // Remembers the library as a root ("photos:") and scans it. unsupported_format
  // where there is no Photos library source; permission_denied until the
  // chrome's own prompt has granted access (this never asks). [no-block]
  [[nodiscard]] result<std::int64_t> index_photos_library();
  [[nodiscard]] photos_access photos_library_access() const;

  // ---- search ------------------------------------------------------------------
  [[nodiscard]] std::uint64_t search_text(const std::string& query, const std::string& scope_dir,
                                          std::uint32_t scope, std::uint32_t kinds);
  [[nodiscard]] std::uint64_t search_similar(const std::string& path, std::int64_t pts_ms,
                                             const std::string& scope_dir, std::uint32_t scope,
                                             std::uint32_t kinds);
  [[nodiscard]] std::uint64_t search_person(std::int64_t person, const std::string& scope_dir,
                                            std::uint32_t scope);
  [[nodiscard]] std::uint64_t search_this_person(const std::string& path, std::int64_t pts_ms);
  [[nodiscard]] result<std::uint32_t> result_count(std::uint64_t id) const;
  [[nodiscard]] result<mv_ai_result> result_at(std::uint64_t id, std::uint32_t index) const;
  [[nodiscard]] result<std::string> result_path(std::uint64_t id, std::uint32_t index) const;
  [[nodiscard]] result<std::string> result_thumb(std::uint64_t id, std::uint32_t index) const;  // [worker-thread]
  [[nodiscard]] result<std::vector<std::pair<std::int64_t, float>>> clip_matches(
      std::uint64_t id, const std::string& path) const;
  void search_release(std::uint64_t id);
  [[nodiscard]] result<std::string> result_snippet(std::uint64_t id, std::uint32_t index) const;
  // Named people for the word being typed (query.h suggest):
  // [{"id":1,"name":"Tristan","completion":"Tristan "}]  [worker-thread]
  [[nodiscard]] std::string suggest_json(const std::string& query);

  // ---- people ------------------------------------------------------------------
  [[nodiscard]] expected faces_enable(bool enable);
  // People, or with `scope` (mv_ai_scope over scope_dir, as a search) the
  // people with a face there: plan/17 "People in the open folder". [worker-thread]
  [[nodiscard]] std::string people_json(const std::string& scope_dir = std::string(),
                                        std::uint32_t scope = MV_AI_SCOPE_ALL);
  [[nodiscard]] std::string person_faces_json(std::int64_t person); // [worker-thread]
  [[nodiscard]] expected person_rename(std::int64_t person, const std::string& name);
  [[nodiscard]] expected person_merge(std::int64_t into, std::int64_t from);
  [[nodiscard]] expected face_reject(std::int64_t face);
  [[nodiscard]] result<std::int64_t> face_split(const std::vector<std::int64_t>& faces);
  // "Refine": files this person's misplaced faces out (plan/17 "People
  // refinement"); how many left them. Only ever on request. [worker-thread]
  [[nodiscard]] result<std::uint32_t> person_refine(std::int64_t person);
  [[nodiscard]] result<std::string> face_thumb(std::int64_t face) const;  // [worker-thread]

  // ---- sharing an index (plan/17 "Sharing an index") ---------------------------
  // An export / import runs on the control thread, one at a time (status::busy
  // while one is queued or running); transfer_json reports it. [no-block]
  [[nodiscard]] result<std::uint64_t> export_index(const std::string& dest, std::vector<std::int64_t> roots,
                                                   std::uint32_t flags);
  // `map_json`: [{"id": file root, "path": folder here}]; roots left out stay out.
  [[nodiscard]] result<std::uint64_t> import_index(const std::string& file, const std::string& map_json,
                                                   std::uint32_t flags);
  // What a file holds and whether its vectors can answer here. [worker-thread]
  [[nodiscard]] result<std::string> inspect_export(const std::string& file);
  [[nodiscard]] std::string transfer_json() const;
  void transfer_cancel() noexcept;

  // ---- for tests -----------------------------------------------------------------
  // Blocks until the queue is empty and every worker idle (or `ms` elapses).
  bool wait_idle(int ms);
  bool wait_search(std::uint64_t id, int ms);
  [[nodiscard]] std::string active_spec() const;
  // Model reloads the control thread has finished (a no-op one included).
  [[nodiscard]] std::uint64_t reloads_done() const noexcept { return reloads_done_.load(); }

 private:
  struct asset_meta {
    std::string path;
    std::string key;      // path_key(path)
    std::string dir_key;  // its folder's key
    asset_kind kind = asset_kind::photo;
    std::int64_t root = 0;
    std::int64_t mtime = 0;  // unix seconds: in: / before: / after: (query.h)
  };
  struct result_row {
    std::int64_t asset = 0;
    std::int64_t pts_ms = -1;
    float score = 0;
    std::uint32_t kind = MV_AI_KIND_PHOTOS;
    std::uint32_t more = 0;
    std::uint32_t match = 0;  // MV_AI_MATCH_*
    float rank = 0;           // across models: margin over the generic prompts, or speech coverage
    std::string path;
    std::string snippet;      // speech: the words that matched
  };
  struct search_state {
    bool done = false;
    std::vector<result_row> rows;
    std::unordered_map<std::int64_t, std::vector<std::pair<std::int64_t, float>>> moments;
    std::unordered_map<std::int64_t, std::size_t> row_of;  // asset -> rows index while merging
  };
  struct search_job {
    std::uint64_t id = 0;
    std::function<void(search_state&)> run;
  };

  // threads
  void control_loop();
  void worker_loop(unsigned index);
  void search_loop();
  // work
  enum class track : std::uint8_t { picture, faces, sound, speech };
  struct job {
    work_item w;
    track t = track::picture;
  };
  bool claim(std::vector<work_item>& out, track& t);
  void process_sound(const work_item& item, const loaded_sound& sound);
  void process_speech(const work_item& item, const loaded_speech& speech);
  [[nodiscard]] std::uint32_t default_media() const;
  [[nodiscard]] platform::power power_state() const;
  // audio search
  struct speech_row {
    std::int64_t asset = 0;
    std::int64_t start_ms = 0;
    std::string text;
    std::vector<std::string> words;
  };
  void load_speech(const std::string& spec);
  void merge_audio(search_state& st, const std::string& query, const std::function<bool(std::int64_t)>& allow,
                   std::uint32_t find, std::uint32_t precision);
  void process_photos(std::vector<work_item>& items, const loaded_clip& clip, bool faces_only);
  void process_video(const work_item& item, const loaded_clip& clip, bool faces_only);
  // An iCloud-only Photos clip: its local poster as one row at 0 ms, marked
  // unavailable so the whole clip is indexed once its original is on this Mac.
  void index_poster(const work_item& item, const loaded_clip& clip, std::uint32_t edge);
  void faces_of(std::int64_t asset, const std::string& path, std::int64_t pts_ms, const rgb_frame& img);
  bool wait_turn();  // false: stopping
  mv_ai_yield yield_reason() const;
  // scans
  void scan_root(const root_row& root);
  void scan_photos(const root_row& root);
  // A scan's batch into the index and the asset map; false on a write error.
  bool see_batch(const root_row& root, std::vector<index_db::seen_file>& batch, std::int64_t gen,
                 std::vector<std::int64_t>& changed);
  // A scan's end: removed assets leave every index, changed ones their vectors.
  void end_root_scan(const root_row& root, std::int64_t gen, const std::vector<std::int64_t>& changed);
  void forget_vectors(const std::vector<std::int64_t>& ids);
  // Whether an unavailable Photos asset is on this Mac now (the launch check).
  [[nodiscard]] bool photos_local(const asset_row& a) const;
  void scan_all();
  // Pixels and files for an asset path: a file goes to the host, a Photos
  // library key ("photos:...") to the Photos source.
  [[nodiscard]] result<rgb_frame> still_of(const std::string& path, std::uint32_t edge) const;
  [[nodiscard]] result<std::string> file_of(const std::string& path) const;  // a clip's readable file
  [[nodiscard]] result<rgb_frame> frame_of(const std::string& path, std::int64_t pts_ms, std::uint32_t edge) const;
  // A result or face tile: a JPEG in the viewer's cache, or for a Photos
  // library still the "photos:" key itself (the chrome asks PhotoKit).
  [[nodiscard]] result<std::string> tile_of(const std::string& path, std::int64_t pts_ms) const;
  // The edge the launch check asks a still at: what the picture pass reads
  // (process_photos, 448 px at the base quality), so "local" means the same.
  static constexpr std::uint32_t kPhotosProbeEdge = 448;
  // The root the indexer leaves alone: the Photos library while access is off.
  [[nodiscard]] std::int64_t skip_root() const noexcept {
    return photos_readable_ ? 0 : photos_root_.load();
  }
  void refresh_counts();
  // models
  void load_models();
  // People and Sound only ("reload", People turned on); the towers stay.
  void load_pieces();
  struct faces_parts {
    std::shared_ptr<face_analyzer> model;
    std::unique_ptr<faces_db> db;
    std::set<std::int64_t> scanned;
  };
  // With the opt-in on: `model` (opened when null) and its database.
  faces_parts open_faces_parts(const settings& s, std::shared_ptr<face_analyzer> model = nullptr);
  // `replacing`: a piece reload, so an absent piece clears what it answered.
  void load_audio(const settings& s, std::uint32_t speech_quality, bool replacing);
  // Blocks until the viewer is quiet (or stopping): opening sessions contends
  // with the present loop for the GPU. False when stopping.
  bool wait_viewer_quiet();
  void load_vectors(const std::string& spec, std::uint32_t dim);
  void load_labels();  // after load_vectors: the label vocabulary into store_ (issue #85)
  std::uint32_t effective_quality(infer::backend on) const;
  void maybe_finish_migration();
  // The answering tower and the search matrix change together (a reload, the
  // end of a migration): answer_gen_ is odd while they do, and a search that
  // saw it change starts again on the new pair, so a query vector never
  // scans vectors of another model (plan/17 "never mixed").
  void begin_answer_swap() noexcept { answer_gen_.fetch_add(1); }
  void end_answer_swap() noexcept { answer_gen_.fetch_add(1); }
  // An even generation (waits out a swap in progress: the vector reload).
  [[nodiscard]] std::uint64_t settled_answer_gen() const;
  // A tower taken out of service. Its last reference goes on the control
  // thread with no lock held, once nothing else holds it and it has stopped
  // settling (Core ML still compiling it: its destructor waits for that,
  // minutes on a first compile). Never under models_m_, never on a thread a
  // chrome calls from.
  void retire(std::shared_ptr<infer::embedder> model);
  void reap_retired();
  // With models_m_ held: what Settings reads of the pieces (pieces_).
  void publish_pieces_locked();
  // search helpers
  std::uint64_t submit(std::function<void(search_state&)> run);
  std::function<bool(std::int64_t)> scope_filter(const std::string& scope_dir, std::uint32_t scope,
                                                 std::uint32_t kinds) const;
  // The assets in scope, of `kinds`, with a file date in [from_unix, to_unix):
  // a snapshot, so a scan never takes assets_m_.
  std::shared_ptr<std::set<std::int64_t>> scope_assets(
      const std::string& scope_dir, std::uint32_t scope, std::uint32_t kinds,
      std::int64_t from_unix = std::numeric_limits<std::int64_t>::min(),
      std::int64_t to_unix = std::numeric_limits<std::int64_t>::max()) const;
  // The picture tower's rows for a description (find_text), the query and the
  // matrix read as one pair (answer_gen_). `gate` false: no "nothing found".
  std::vector<vector_store::hit> picture_hits(const std::string& text, const std::function<bool(std::int64_t)>& allow,
                                              bool gate, std::uint32_t precision);
  // Merges one model's hits into the result rows (one row per asset, its best
  // moment first). `text`: rank by margin over the generic prompts and apply
  // the "nothing found" rule; else rank by score.
  void group(search_state& st, const std::vector<vector_store::hit>& hits, bool text,
             float query_margin, std::uint32_t match = MV_AI_MATCH_PICTURE, bool stands_out = false) const;
  void add_row(search_state& st, std::int64_t asset, std::int64_t pts_ms, float score, float rank,
               std::uint32_t match, std::string snippet) const;
  static void finish(search_state& st);
  std::vector<float> query_vector(const loaded_clip& answer, const std::string& text);
  // sharing an index
  struct transfer_job {
    std::uint64_t id = 0;
    bool import = false;
    std::string file;
    std::vector<std::int64_t> roots;                         // export
    std::vector<std::pair<std::int64_t, std::string>> map;   // import: file root -> folder here
    std::uint32_t flags = 0;
  };
  void run_transfer(transfer_job job);  // control thread
  [[nodiscard]] std::string run_export(const transfer_job& job, mv::status& st);
  [[nodiscard]] std::string run_import(const transfer_job& job, mv::status& st);
  // The picture specs a file's rows may land under here, and the Quality to
  // adopt (0 none) when this index is empty and the file's tower is carried.
  struct spec_plan {
    std::set<std::string> specs;
    std::string picture;       // the file's picture spec when usable here, else ""
    std::uint32_t adopt = 0;
    std::string why_not;       // "model" | "no_models" | ""
  };
  [[nodiscard]] spec_plan plan_specs(const std::string& file_picture_spec);
  // Every in-memory view of the index from its rows again (after an import).
  void reload_from_db();
  void set_transfer_progress(double f);
  // An import holds index.db (and People) for its one transaction: a [no-block]
  // call that would write them answers status::busy rather than wait on it.
  [[nodiscard]] bool importing_elsewhere() const noexcept {
    return transferring_.load() && std::this_thread::get_id() != control_.get_id();
  }
  // settings
  void save_settings() const;
  void load_settings();
  void post(mv_addon_event_kind kind, std::uint64_t id = 0, std::int64_t payload = 0) const;

  host host_;
  engine_deps deps_;
  // The Photos library (issue #72): the source, its root's id (0 none) and
  // whether it is readable now (read by the control thread each scan).
  std::unique_ptr<photos_source> photos_;
  std::atomic<std::int64_t> photos_root_{0};
  std::atomic<bool> photos_readable_{false};
  std::atomic<bool> photos_changed_{false};  // PhotoKit said so; the control thread rescans
  std::string data_dir_;
  std::unique_ptr<index_db> db_;

  mutable std::mutex settings_m_;
  settings settings_;

  // models (swapped by the control thread; workers copy the shared_ptr)
  mutable std::mutex models_m_;
  loaded_clip build_;   // the tower indexing now (the target)
  loaded_clip answer_;  // the tower whose vectors answer queries (the active spec)
  std::uint32_t build_compute_ = MV_AI_COMPUTE_AUTO;  // the compute choice build_ opened on
  std::shared_ptr<face_analyzer> faces_model_;
  std::unique_ptr<faces_db> faces_;
  std::set<std::int64_t> faces_scanned_;  // under models_m_
  loaded_sound sound_;    // empty model: no ai-audio piece
  loaded_speech speech_;
  vector_store sounds_;   // the CLAP matrix (window starts as pts)
  mutable std::mutex speech_m_;
  std::vector<speech_row> speech_rows_;
  std::atomic<bool> models_ready_{false};
  std::atomic<bool> models_failed_{false};
  std::atomic<bool> reload_models_{false};
  std::atomic<bool> reload_pieces_{false};
  std::atomic<bool> first_compile_{false};  // this load compiles a tower for the first time
  // load_models is opening towers. The towers already in keep answering
  // searches meanwhile (a reload); the indexer waits for the new ones.
  std::atomic<bool> loading_{false};
  std::atomic<std::uint64_t> answer_gen_{0};
  std::atomic<std::uint64_t> reloads_done_{0};
  std::vector<std::shared_ptr<infer::embedder>> retired_;  // control thread only

  // What settings_json reports of the pieces, so that call (the UI thread's)
  // never takes models_m_, which a worker holds across a People database
  // write. Written under models_m_ -> pieces_m_ (a leaf).
  struct pieces_view {
    bool faces_ready = false;
    bool sound_ready = false;
    bool speech_ready = false;
    std::string sound_name;
    std::string speech_name;
  };
  mutable std::mutex pieces_m_;
  pieces_view pieces_;

  vector_store store_;
  mutable std::mutex assets_m_;
  std::unordered_map<std::int64_t, asset_meta> assets_;

  // work queue. Lock order: work_m_ -> models_m_ -> assets_m_ -> status_m_;
  // no path takes an earlier one while holding a later one.
  mutable std::mutex work_m_;
  std::condition_variable work_cv_;
  std::deque<job> queue_;
  std::set<std::int64_t> in_flight_;
  std::string prefer_dir_key_;
  bool queue_exhausted_ = false;
  unsigned busy_workers_ = 0;

  // control
  std::atomic<bool> stopping_{false};
  std::atomic<bool> paused_{false};
  std::atomic<bool> index_full_{false};
  std::atomic<int> yield_now_{MV_AI_YIELD_NONE};
  // "Index anyway": the user's session override of the battery pause. Never
  // saved; ended by the machine going back to AC (so the next unplug pauses
  // again) or by a restart.
  mutable std::atomic<bool> battery_override_{false};
  std::atomic<bool> clearing_{false};
  std::mutex control_m_;
  std::condition_variable control_cv_;
  std::set<std::int64_t> rescan_roots_;
  bool rescan_all_ = true;
  std::atomic<bool> scanning_{true};  // the control thread is scanning (wait_idle)
  std::thread control_;
  std::vector<std::thread> workers_;

  // status
  mutable std::mutex status_m_;
  mv_ai_status status_{};
  struct tick {
    double t = 0;
    double units = 0;
    std::uint64_t frames = 0;
  };
  std::deque<tick> ticks_;
  counts counts_{};
  std::string active_root_;
  std::map<std::int64_t, std::pair<std::uint64_t, std::uint64_t>> root_counts_;  // assets, done
  std::vector<root_row> roots_cache_;
  double units_done_ = 0;          // under status_m_
  std::uint64_t frames_done_ = 0;  // under status_m_

  // search
  std::thread search_thread_;
  mutable std::mutex search_m_;
  std::condition_variable search_cv_;
  std::deque<search_job> search_queue_;
  std::map<std::uint64_t, std::shared_ptr<search_state>> searches_;
  std::uint64_t next_search_ = 1;
  std::mutex text_cache_m_;

  // sharing an index
  mutable std::mutex transfer_m_;
  std::deque<transfer_job> transfer_queue_;  // at most one
  std::uint64_t next_transfer_ = 1;
  struct transfer_view {
    std::uint64_t id = 0;
    bool import = false;
    bool running = false;
    bool done = false;
    std::int32_t status = 0;  // mv_status once done
    double fraction = 0;
    std::string outcome;      // the finished job's counts, a JSON object
  } transfer_;
  std::atomic<bool> transfer_cancel_{false};
  std::atomic<bool> transferring_{false};  // workers wait (an import)
  std::deque<std::pair<std::string, std::vector<float>>> text_cache_;
};

// Media by name, for the walk only (the host still probes bytes when it
// decodes; plan/04 "probe by magic bytes"): what is worth offering it.
[[nodiscard]] int media_kind_of_name(const std::string& name) noexcept;  // 0 none, 1 photo, 2 video
// A comparable form of a path: '/' separators, no trailing '/', and on
// Windows ASCII case folded.
[[nodiscard]] std::string path_key(const std::string& path);

}  // namespace mv::ai
