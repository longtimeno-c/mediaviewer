// SPDX-License-Identifier: GPL-2.0-or-later
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
#include "addons/ai/vectors.h"
#include "core/result.h"
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

struct engine_deps {
  // The towers the pack carries (mv_ai_quality 1 and / or 2).
  std::function<std::vector<std::uint32_t>()> qualities;
  // Which accelerated backends exist in the loaded runtime.
  std::function<bool(infer::backend)> backend_available;
  // Opens a tower on a compute choice (mv_ai_compute). Auto self-tests the
  // provider against CPU and falls back (plan/17 "Runtime").
  std::function<result<loaded_clip>(std::uint32_t quality, std::uint32_t compute)> open_clip;
  // Null when the ai-faces piece is not installed.
  std::function<result<std::unique_ptr<face_analyzer>>()> open_faces;
  // "CLIP ViT-B/32" for a quality, without opening it (Settings).
  std::function<std::string(std::uint32_t quality)> model_name;
  std::function<std::string()> runtime_version;
  // A vendor piece installed or removed since the runtime loaded: the
  // change needs the app to start again (the runtime cannot be swapped live).
  std::function<bool()> restart_needed;
};

struct settings {
  std::uint32_t compute = MV_AI_COMPUTE_AUTO;
  std::uint32_t quality = MV_AI_QUALITY_AUTO;
  int battery_percent = 30;                  // pause on battery below this
  std::uint64_t index_cap = 8'000'000'000;   // bytes; 0 = no cap
  bool faces = false;                        // the separate People opt-in
};

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

  // ---- people ------------------------------------------------------------------
  [[nodiscard]] expected faces_enable(bool enable);
  [[nodiscard]] std::string people_json();                          // [worker-thread]
  [[nodiscard]] std::string person_faces_json(std::int64_t person); // [worker-thread]
  [[nodiscard]] expected person_rename(std::int64_t person, const std::string& name);
  [[nodiscard]] expected person_merge(std::int64_t into, std::int64_t from);
  [[nodiscard]] expected face_reject(std::int64_t face);
  [[nodiscard]] result<std::int64_t> face_split(const std::vector<std::int64_t>& faces);
  [[nodiscard]] result<std::string> face_thumb(std::int64_t face) const;  // [worker-thread]

  // ---- for tests -----------------------------------------------------------------
  // Blocks until the queue is empty and every worker idle (or `ms` elapses).
  bool wait_idle(int ms);
  bool wait_search(std::uint64_t id, int ms);
  [[nodiscard]] std::string active_spec() const;

 private:
  struct asset_meta {
    std::string path;
    std::string key;      // path_key(path)
    std::string dir_key;  // its folder's key
    asset_kind kind = asset_kind::photo;
    std::int64_t root = 0;
  };
  struct result_row {
    std::int64_t asset = 0;
    std::int64_t pts_ms = -1;
    float score = 0;
    std::uint32_t kind = MV_AI_KIND_PHOTOS;
    std::uint32_t more = 0;
    std::string path;
  };
  struct search_state {
    bool done = false;
    std::vector<result_row> rows;
    std::unordered_map<std::int64_t, std::vector<std::pair<std::int64_t, float>>> moments;
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
  bool claim(std::vector<work_item>& out, bool& faces_only);
  void process_photos(std::vector<work_item>& items, const loaded_clip& clip, bool faces_only);
  void process_video(const work_item& item, const loaded_clip& clip, bool faces_only);
  void faces_of(std::int64_t asset, const std::string& path, std::int64_t pts_ms, const rgb_frame& img);
  bool wait_turn();  // false: stopping
  mv_ai_yield yield_reason() const;
  // scans
  void scan_root(const root_row& root);
  void scan_all();
  void refresh_counts();
  // models
  void load_models();
  void load_vectors(const std::string& spec, std::uint32_t dim);
  std::uint32_t effective_quality(infer::backend on) const;
  void maybe_finish_migration();
  // search helpers
  std::uint64_t submit(std::function<void(search_state&)> run);
  std::function<bool(std::int64_t)> scope_filter(const std::string& scope_dir, std::uint32_t scope,
                                                 std::uint32_t kinds) const;
  void group(search_state& st, const std::vector<vector_store::hit>& hits, bool text,
             float query_margin) const;
  std::vector<float> query_vector(const std::string& text);
  // settings
  void save_settings() const;
  void load_settings();
  void post(mv_addon_event_kind kind, std::uint64_t id = 0, std::int64_t payload = 0) const;

  host host_;
  engine_deps deps_;
  std::string data_dir_;
  std::unique_ptr<index_db> db_;

  mutable std::mutex settings_m_;
  settings settings_;

  // models (swapped by the control thread; workers copy the shared_ptr)
  mutable std::mutex models_m_;
  loaded_clip build_;   // the tower indexing now (the target)
  loaded_clip answer_;  // the tower whose vectors answer queries (the active spec)
  std::shared_ptr<face_analyzer> faces_model_;
  std::unique_ptr<faces_db> faces_;
  std::set<std::int64_t> faces_scanned_;  // under models_m_
  std::atomic<bool> models_ready_{false};
  std::atomic<bool> models_failed_{false};
  std::atomic<bool> reload_models_{false};

  vector_store store_;
  mutable std::mutex assets_m_;
  std::unordered_map<std::int64_t, asset_meta> assets_;

  // work queue. Lock order: work_m_ -> models_m_ -> assets_m_ -> status_m_;
  // no path takes an earlier one while holding a later one.
  mutable std::mutex work_m_;
  std::condition_variable work_cv_;
  std::deque<work_item> queue_;
  std::set<std::int64_t> in_flight_;
  std::string prefer_dir_key_;
  bool queue_exhausted_ = false;
  unsigned busy_workers_ = 0;

  // control
  std::atomic<bool> stopping_{false};
  std::atomic<bool> paused_{false};
  std::atomic<bool> index_full_{false};
  std::atomic<int> yield_now_{MV_AI_YIELD_NONE};
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
  double active_seconds_ = 0;
  counts counts_{};
  counts build_counts_{};  // the migration target, when it differs
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
  std::deque<std::pair<std::string, std::vector<float>>> text_cache_;
};

// Media by name, for the walk only (the host still probes bytes when it
// decodes; plan/04 "probe by magic bytes"): what is worth offering it.
[[nodiscard]] int media_kind_of_name(const std::string& name) noexcept;  // 0 none, 1 photo, 2 video
// A comparable form of a path: '/' separators, no trailing '/', and on
// Windows ASCII case folded.
[[nodiscard]] std::string path_key(const std::string& path);

}  // namespace mv::ai
