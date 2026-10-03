// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/ai/engine.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>

#include "addons/ai/platform.h"
#include "addons/ai/query.h"
#include "addons/ai/transfer.h"
#include "addons/ai/vocabulary.h"
#include "core/json.h"

namespace mv::ai {
namespace {

using clock = std::chrono::steady_clock;

double now_s() {
  return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

std::int64_t unix_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string lower_ascii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

std::string extension_of(const std::string& name) {
  const std::size_t dot = name.find_last_of('.');
  const std::size_t slash = name.find_last_of("/\\");
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
  return lower_ascii(name.substr(dot + 1));
}

// Folders nothing should index (plan/10 PR 26's housekeeping list).
bool housekeeping(const std::string& key) {
  for (const char* part : {"/@eadir/", "/#recycle/", "/$recycle.bin/", "/system volume information/",
                           "/.trashes/", "/.spotlight-v100/"}) {
    if (lower_ascii(key).find(part) != std::string::npos) return true;
  }
  return false;
}

std::filesystem::path fs_path(const std::string& utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string join(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  const char last = dir.back();
  return (last == '/' || last == '\\') ? dir + name : dir + "/" + name;
}

float max_dot(std::span<const float> v, const std::vector<std::vector<float>>& set) {
  float best = -1;
  for (const auto& g : set) best = std::max(best, infer::dot(v, g));
  return best < -0.5f ? 0.0f : best;
}

std::string key_parent(const std::string& key) {
  const std::size_t slash = key.find_last_of('/');
  return slash == std::string::npos ? std::string() : key.substr(0, slash);
}

// The folder an asset counts as in: its parent, or the library for a Photos
// asset (its identifier's slashes are not folders).
std::string dir_of(const std::string& key) {
  return is_photos_key(key) ? std::string(kPhotosRoot) : key_parent(key);
}

bool under(const std::string& key, const std::string& dir_key) {
  return key.size() > dir_key.size() && key.compare(0, dir_key.size(), dir_key) == 0 &&
         key[dir_key.size()] == '/';
}

void copy_str(char* dst, std::size_t cap, const std::string& src) {
  const std::size_t n = std::min(src.size(), cap - 1);
  std::memcpy(dst, src.data(), n);
  dst[n] = '\0';
}

constexpr std::size_t kPhotoBatch = 4;
constexpr std::size_t kFrameBatch = 8;
constexpr std::int32_t kMaxTries = 3;
constexpr std::uint32_t kPeopleMinFaces = 2;

}  // namespace

// ---- names and keys ---------------------------------------------------------------

int media_kind_of_name(const std::string& name) noexcept {
  static const char* const photos[] = {"jpg", "jpeg", "jpe", "png", "bmp", "gif", "tif", "tiff", "webp",
                                       "heic", "heif", "hif", "avif", "ico", "dng", "cr2", "cr3", "crw",
                                       "nef", "nrw", "arw", "srf", "sr2", "raf", "orf", "rw2", "pef",
                                       "srw", "x3f", "3fr", "erf", "kdc", "mrw", "iiq", "rwl", "raw"};
  static const char* const videos[] = {"mp4", "m4v", "mov", "mkv", "webm", "avi", "ts", "mts",
                                       "m2ts", "mpg", "mpeg", "3gp", "wmv"};
  const std::string ext = extension_of(name);
  if (ext.empty()) return 0;
  for (const char* e : photos) {
    if (ext == e) return 1;
  }
  for (const char* e : videos) {
    if (ext == e) return 2;
  }
  return 0;
}

std::string path_key(const std::string& path) {
  std::string k = path;
  for (char& c : k) {
    if (c == '\\') c = '/';
  }
  while (k.size() > 1 && k.back() == '/') k.pop_back();
#if defined(_WIN32)
  k = lower_ascii(std::move(k));
#endif
  return k;
}

// ---- lifetime ------------------------------------------------------------------------

engine::engine(const mv_host_api* api, engine_deps deps) : host_(api), deps_(std::move(deps)) {
  status_.struct_size = sizeof(mv_ai_status);
  status_.state = MV_AI_STATE_LOADING;
  status_.eta_low_seconds = -1;
  status_.eta_high_seconds = -1;
}

engine::~engine() { stop(); }

expected engine::start() {
  MV_TRY(std::string dir, host_.data_dir());
  data_dir_ = dir;
  MV_TRY_VOID(host_.make_directories(data_dir_));
  load_settings();
  MV_TRY(auto db, index_db::open(join(data_dir_, "index.db")));
  db_ = std::move(db);
  {
    std::lock_guard lock(assets_m_);
    for (asset_row& a : db_->all_assets()) {
      asset_meta m;
      m.key = path_key(a.path);
      m.dir_key = dir_of(m.key);
      m.path = std::move(a.path);
      m.kind = a.kind;
      m.root = a.root_id;
      m.mtime = a.mtime;
      assets_.emplace(a.id, std::move(m));
    }
  }
  roots_cache_ = db_->roots();
  photos_ = deps_.photos ? deps_.photos() : make_photos_source();
  for (const root_row& r : roots_cache_) {
    if (is_photos_key(r.path)) photos_root_ = r.id;
  }
  control_ = std::thread([this] { control_loop(); });
  search_thread_ = std::thread([this] { search_loop(); });
  return {};
}

void engine::stop() noexcept {
  if (stopping_.exchange(true)) return;
  if (photos_) photos_->observe(nullptr);  // before the threads it wakes go
  control_cv_.notify_all();
  work_cv_.notify_all();
  search_cv_.notify_all();
  if (control_.joinable()) control_.join();
  for (std::thread& t : workers_) {
    if (t.joinable()) t.join();
  }
  workers_.clear();
  if (search_thread_.joinable()) search_thread_.join();
}

void engine::post(mv_addon_event_kind kind, std::uint64_t id, std::int64_t payload) const {
  host_.post(kind, MV_OK, id, payload);
}

// ---- settings --------------------------------------------------------------------------

void engine::load_settings() {
  std::ifstream in(fs_path(join(data_dir_, "settings.json")), std::ios::binary);
  if (!in) return;
  std::ostringstream ss;
  ss << in.rdbuf();
  const auto doc = json::parse(ss.str(), 4);
  if (!doc || doc->k != json::kind::object) return;
  std::lock_guard lock(settings_m_);
  settings_.compute = static_cast<std::uint32_t>(std::clamp<std::int64_t>(doc->integer("compute").value_or(0), 0, 4));
  settings_.quality = static_cast<std::uint32_t>(std::clamp<std::int64_t>(doc->integer("quality").value_or(0), 0, 2));
  settings_.battery_percent = static_cast<int>(std::clamp<std::int64_t>(doc->integer("pause_on_battery_percent").value_or(30), 0, 100));
  settings_.index_cap = static_cast<std::uint64_t>(std::max<std::int64_t>(0, doc->integer("index_cap_bytes").value_or(8'000'000'000)));
  settings_.faces = doc->boolean("faces").value_or(false);
  settings_.video_index = static_cast<std::uint32_t>(std::clamp<std::int64_t>(doc->integer("video_index").value_or(0), 0, 3));
  settings_.precision = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
      doc->integer("precision").value_or(kPrecisionDefault), 0, kPrecisionLevels - 1));
}

void engine::save_settings() const {
  json::writer w;
  {
    std::lock_guard lock(settings_m_);
    w.begin_object();
    w.key("compute").integer(settings_.compute);
    w.key("quality").integer(settings_.quality);
    w.key("pause_on_battery_percent").integer(settings_.battery_percent);
    w.key("index_cap_bytes").integer(static_cast<std::int64_t>(settings_.index_cap));
    w.key("faces").boolean(settings_.faces);
    w.key("video_index").integer(settings_.video_index);
    w.key("precision").integer(settings_.precision);
    w.end_object();
  }
  const std::string tmp = join(data_dir_, "settings.json.tmp");
  {
    std::ofstream out(fs_path(tmp), std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << w.str();
  }
  std::error_code ec;
  std::filesystem::rename(fs_path(tmp), fs_path(join(data_dir_, "settings.json")), ec);
}

std::string engine::settings_json() const {
  json::writer w;
  settings s;
  {
    std::lock_guard lock(settings_m_);
    s = settings_;
  }
  w.begin_object();
  w.key("compute").integer(s.compute);
  w.key("quality").integer(s.quality);
  w.key("pause_on_battery_percent").integer(s.battery_percent);
  w.key("battery_override").boolean(battery_override_.load());
  w.key("index_cap_bytes").integer(static_cast<std::int64_t>(s.index_cap));
  w.key("faces").boolean(s.faces);
  // What videos are indexed for, in effect: an unset choice reads as Pictures,
  // or Both once the ai-audio piece is installed (default_media).
  w.key("video_index").integer(default_media());
  w.key("video_index_setting").integer(s.video_index);
  w.key("precision").integer(s.precision);
  {
    // Not models_m_: this is the UI thread's call (pieces_).
    std::lock_guard lock(pieces_m_);
    w.key("faces_ready").boolean(pieces_.faces_ready);
    w.key("audio_ready").boolean(pieces_.sound_ready || pieces_.speech_ready);
    w.key("sound_model").string(pieces_.sound_name);
    w.key("speech_model").string(pieces_.speech_name);
  }
  w.key("available").begin_object();
  w.key("cuda").boolean(deps_.backend_available && deps_.backend_available(infer::backend::cuda));
  w.key("openvino").boolean(deps_.backend_available && deps_.backend_available(infer::backend::openvino));
  w.key("coreml").boolean(deps_.backend_available && deps_.backend_available(infer::backend::coreml));
  w.end_object();
  w.key("models").begin_array();
  if (deps_.qualities) {
    for (std::uint32_t q : deps_.qualities()) {
      w.begin_object();
      w.key("quality").integer(q);
      w.key("name").string(deps_.model_name ? deps_.model_name(q) : std::string());
      w.end_object();
    }
  }
  w.end_array();
  w.key("runtime").string(deps_.runtime_version ? deps_.runtime_version() : std::string());
  w.key("restart_needed").boolean(deps_.restart_needed && deps_.restart_needed());
  w.end_object();
  return w.take();
}

expected engine::set_setting(const std::string& key, const std::string& value_json) {
  const auto v = json::parse(value_json, 2);
  if (!v || v->k != json::kind::number) return err(status::invalid_arg);
  const double x = v->is_integer ? static_cast<double>(v->i) : v->d;
  if (key == "battery_override") {
    // "Index anyway": for this spell on battery only, never written to
    // settings.json. The workers pick it up on their next turn (wait_turn).
    battery_override_ = x != 0;
    return {};
  }
  bool reload = false;
  bool pieces = false;
  {
    std::lock_guard lock(settings_m_);
    if (key == "compute") {
      if (x < 0 || x > 4) return err(status::invalid_arg);
      reload = settings_.compute != static_cast<std::uint32_t>(x);
      settings_.compute = static_cast<std::uint32_t>(x);
    } else if (key == "quality") {
      if (x < 0 || x > 2) return err(status::invalid_arg);
      reload = settings_.quality != static_cast<std::uint32_t>(x);
      settings_.quality = static_cast<std::uint32_t>(x);
    } else if (key == "pause_on_battery_percent") {
      settings_.battery_percent = static_cast<int>(std::clamp(x, 0.0, 100.0));
    } else if (key == "index_cap_bytes") {
      settings_.index_cap = static_cast<std::uint64_t>(std::max(0.0, x));
      index_full_ = false;  // re-checked by the control thread
    } else if (key == "min_score") {
      // Calibrated per model (model.json); accepted and ignored.
    } else if (key == "reload") {
      pieces = true;  // a piece came or went: the towers stay as they are
    } else if (key == "video_index") {
      if (x < 0 || x > 3) return err(status::invalid_arg);
      settings_.video_index = static_cast<std::uint32_t>(x);
    } else if (key == "precision") {
      // Read by each search as it starts: no reload, no re-index (plan/17
      // "Precision scale"). Out of range clamps to the nearest end.
      settings_.precision = static_cast<std::uint32_t>(std::clamp(std::round(x), 0.0, static_cast<double>(kPrecisionLevels - 1)));
    } else {
      return err(status::invalid_arg);
    }
  }
  if (!pieces) save_settings();
  if (reload || pieces) {
    (reload ? reload_models_ : reload_pieces_) = true;
    control_cv_.notify_all();
  }
  if (key == "video_index") {
    std::lock_guard lock(work_m_);
    queue_.clear();
    queue_exhausted_ = false;
    work_cv_.notify_all();
  }
  post(MV_ADDON_EVENT_AI_STATUS);
  return {};
}

void engine::pause(bool paused) {
  paused_ = paused;
  work_cv_.notify_all();
  control_cv_.notify_all();
  post(MV_ADDON_EVENT_AI_STATUS);
}

// ---- status ------------------------------------------------------------------------------

void engine::status(mv_ai_status& out) const {
  std::lock_guard lock(status_m_);
  out = status_;
  out.struct_size = sizeof(mv_ai_status);
}

void engine::refresh_counts() {
  std::string build_spec, active_spec;
  loaded_clip build;
  {
    std::lock_guard lock(models_m_);
    build = build_;
    build_spec = build_.meta.spec_key;
    active_spec = answer_.meta.spec_key;
  }
  const std::uint32_t media = default_media();
  const std::int64_t skip = skip_root();
  const counts c = build_spec.empty() ? counts{} : db_->count(build_spec, track_filter{true, MV_AI_MEDIA_PICTURES, media, skip});
  std::string sound_spec, speech_spec;
  {
    std::lock_guard lock(models_m_);
    sound_spec = sound_.spec_key;
    speech_spec = speech_.spec_key;
  }
  const track_filter audio{false, MV_AI_MEDIA_SOUND, media, skip};
  const counts cs = sound_spec.empty() ? counts{} : db_->count(sound_spec, audio);
  const counts cp = speech_spec.empty() ? counts{} : db_->count(speech_spec, audio);
  std::map<std::int64_t, std::pair<std::uint64_t, std::uint64_t>> per_root;
  const std::vector<root_row> roots = db_->roots();
  for (const root_row& r : roots) {
    per_root[r.id] = {db_->assets_in_root(r.id), build_spec.empty() ? 0 : db_->done_in_root(r.id, build_spec)};
  }
  std::uint64_t bytes = db_->bytes();
  std::uint64_t face_total = 0;
  std::uint32_t people = 0;
  bool faces_ready = false;
  bool rerun = false;
  std::uint64_t people_total = 0, people_done = 0;
  std::string people_model;
  {
    std::lock_guard lock(models_m_);
    faces_ready = faces_model_ != nullptr;
    if (faces_model_) people_model = faces_model_->name();
    if (faces_) {
      face_total = faces_->face_count();
      people = faces_->person_count(kPeopleMinFaces);
      rerun = faces_->rerun_pending();
      std::lock_guard al(assets_m_);
      people_total = assets_.size();
      if (rerun) {
        // Exact while a re-run shows progress; one pass over the assets.
        for (const auto& [id, m] : assets_) people_done += faces_scanned_.count(id);
      } else {
        people_done = std::min<std::uint64_t>(faces_scanned_.size(), people_total);
      }
    }
  }
  {
    std::error_code ec;
    const auto fsz = std::filesystem::file_size(fs_path(join(data_dir_, "faces.db")), ec);
    if (!ec) bytes += fsz;
  }
  settings s;
  {
    std::lock_guard lock(settings_m_);
    s = settings_;
  }
  if (s.index_cap != 0 && bytes > s.index_cap && !index_full_) index_full_ = true;

  std::lock_guard lock(status_m_);
  counts_ = c;
  root_counts_ = std::move(per_root);
  roots_cache_ = roots;
  status_.assets_total = c.assets;
  status_.assets_done = c.done;
  status_.assets_failed = c.failed;
  status_.assets_unavailable = c.unavailable;
  status_.sound_total = cs.assets;
  status_.sound_done = cs.done + cs.failed + cs.unavailable;
  status_.speech_total = cp.assets;
  status_.speech_done = cp.done + cp.failed + cp.unavailable;
  status_.frames_indexed = store_.live_rows();
  status_.index_bytes = bytes;
  status_.faces_total = face_total;
  status_.people = people;
  status_.people_scan_total = people_total;
  status_.people_scan_done = people_done;
  copy_str(status_.people_model_utf8, sizeof(status_.people_model_utf8), people_model);
  // Read live: on the Mac, Core ML takes over from CPU once it has compiled.
  status_.backend = static_cast<std::uint32_t>(build.model ? build.model->on() : build.on);
  const infer::provider_fault fault =
      build.fault != infer::provider_fault::none || !build.model ? build.fault : build.model->fault();
  status_.provider_fault = static_cast<std::uint32_t>(fault);
  status_.quality = build.meta.quality;
  copy_str(status_.model_utf8, sizeof(status_.model_utf8), build.meta.name);
  copy_str(status_.active_root_utf8, sizeof(status_.active_root_utf8), active_root_);
  status_.migrate_total = build_spec != active_spec ? c.assets : 0;
  status_.migrate_done = build_spec != active_spec ? c.done + c.failed + c.unavailable : 0;
  std::uint32_t flags = 0;
  if (index_full_) flags |= MV_AI_STATUS_INDEX_FULL;
  if (s.faces) flags |= MV_AI_STATUS_FACES_ON;
  if (face_total > 0 || faces_ready) flags |= MV_AI_STATUS_FACES_READY;
  if (models_failed_) flags |= MV_AI_STATUS_NO_MODELS;
  if (first_compile_ && (loading_ || !models_ready_) && !models_failed_) flags |= MV_AI_STATUS_FIRST_COMPILE;
  if (!sound_spec.empty() || !speech_spec.empty()) flags |= MV_AI_STATUS_AUDIO_READY;
  if (rerun) flags |= MV_AI_STATUS_PEOPLE_RERUN;
  if (settling_) flags |= MV_AI_STATUS_PEOPLE_SETTLING;
  status_.flags = flags;

  // Rates over the last minute of active work; the ETA is a range from them
  // (plan/17: "based on completed work, not a hard-coded claim").
  const double t = now_s();
  while (!ticks_.empty() && t - ticks_.front().t > 60.0) ticks_.pop_front();
  ticks_.push_back(tick{t, units_done_, frames_done_});
  if (ticks_.size() >= 2 && ticks_.back().t - ticks_.front().t > 5.0) {
    const double dt = ticks_.back().t - ticks_.front().t;
    const double du = ticks_.back().units - ticks_.front().units;
    const double df = static_cast<double>(ticks_.back().frames - ticks_.front().frames);
    status_.frames_per_second = df / dt;
    const double assets_rate = du / dt;
    status_.assets_per_second = assets_rate;
    const double remaining = static_cast<double>(c.pending_photos) + static_cast<double>(c.pending_video_ms) / 1000.0;
    if (du > 0 && remaining > 0) {
      const double eta = remaining / assets_rate;
      status_.eta_low_seconds = eta * 0.8;
      status_.eta_high_seconds = eta * 1.35;
    } else if (remaining <= 0) {
      status_.eta_low_seconds = 0;
      status_.eta_high_seconds = 0;
    } else {
      status_.eta_low_seconds = -1;
      status_.eta_high_seconds = -1;
    }
  }
  const bool pending = c.assets > c.done + c.failed + c.unavailable ||
                       cs.assets > cs.done + cs.failed + cs.unavailable ||
                       cp.assets > cp.done + cp.failed + cp.unavailable;
  std::uint32_t state = MV_AI_STATE_INDEXING;
  if (models_failed_) {
    state = MV_AI_STATE_ERROR;
  } else if (!models_ready_ || loading_) {
    state = MV_AI_STATE_LOADING;
  } else if (paused_ || index_full_) {
    state = MV_AI_STATE_PAUSED;
  } else if (!pending) {
    state = MV_AI_STATE_IDLE;
  } else if (yield_now_ != MV_AI_YIELD_NONE) {
    state = MV_AI_STATE_YIELDING;
  }
  status_.state = state;
  status_.yield_reason = state == MV_AI_STATE_YIELDING || state == MV_AI_STATE_LOADING
                             ? static_cast<std::uint32_t>(yield_now_.load())
                             : 0;
}

// ---- models ---------------------------------------------------------------------------------

std::uint32_t engine::effective_quality(infer::backend on) const {
  std::uint32_t q;
  {
    std::lock_guard lock(settings_m_);
    q = settings_.quality;
  }
  const std::vector<std::uint32_t> have = deps_.qualities ? deps_.qualities() : std::vector<std::uint32_t>{};
  const auto has = [&](std::uint32_t x) { return std::find(have.begin(), have.end(), x) != have.end(); };
  if (q == MV_AI_QUALITY_AUTO) {
    // plan/17 PR 20 spike: the large tower where a GPU / Neural Engine runs
    // it, the small one on CPU only (it is ~12x faster there).
    q = on != infer::backend::cpu ? MV_AI_QUALITY_HIGH : MV_AI_QUALITY_FAST;
  }
  if (!has(q)) q = has(MV_AI_QUALITY_FAST) ? MV_AI_QUALITY_FAST : MV_AI_QUALITY_HIGH;
  return q;
}

bool engine::wait_viewer_quiet() {
  // A GPU context, CUDA or Core ML compilation and a gigabyte of weights
  // cost the present loop frames (the PR 1 soak dropped two while a pack
  // loaded): load between the viewer's busy spells, as indexing works.
  bool waited = false;
  while (!stopping_ && host_.should_yield()) {
    if (!waited) {
      // Say why it is still loading (LOADING with yield_reason VIEWER).
      yield_now_ = MV_AI_YIELD_VIEWER;
      refresh_counts();
      post(MV_ADDON_EVENT_AI_STATUS);
      waited = true;
    }
    std::unique_lock lock(control_m_);
    control_cv_.wait_for(lock, std::chrono::milliseconds(250), [this] { return stopping_.load(); });
  }
  yield_now_ = MV_AI_YIELD_NONE;
  if (waited && !stopping_) {
    refresh_counts();
    post(MV_ADDON_EVENT_AI_STATUS);
  }
  return !stopping_;
}

void engine::load_models() {
  // A reload keeps the towers in answering searches until the new ones are
  // open (the swap below); only a first load (or one after a failure) has
  // nothing to answer with. The indexer waits either way (loading_).
  loading_ = true;
  struct done_loading {
    std::atomic<bool>* flag;
    ~done_loading() { *flag = false; }
  } const done{&loading_};
  const bool reloading = models_ready_.load();
  if (!reloading) models_ready_ = false;
  if (!wait_viewer_quiet()) return;
  if (deps_.prepare) deps_.prepare();
  if (!deps_.open_clip) {
    models_ready_ = false;
    models_failed_ = true;
    return;
  }
  settings s;
  {
    std::lock_guard lock(settings_m_);
    s = settings_;
  }
  if (reloading) {
    // Already running what these settings ask for: nothing opens. Quality
    // High where Auto already chose High (Core ML) is the same tower, and
    // reopening it was a second multi-minute Core ML compile of L/14 beside
    // the first, with gigabytes written (2026-09-28).
    std::uint32_t quality = 0;
    std::uint32_t compute = 0;
    infer::backend on = infer::backend::cpu;
    bool have_model = false;
    {
      std::lock_guard lock(models_m_);
      have_model = build_.model != nullptr;
      quality = build_.meta.quality;
      compute = build_compute_;
      on = build_.on;
    }
    if (have_model && compute == s.compute && effective_quality(on) == quality) {
      post(MV_ADDON_EVENT_AI_STATUS);
      return;
    }
  }
  if (reloading) {
    // Say so while the new tower opens (the one in service still answers).
    refresh_counts();
    post(MV_ADDON_EVENT_AI_STATUS);
  }
  // Open the preferred tower on the chosen compute; with Auto quality, the
  // backend that tower lands on decides between the two.
  const std::vector<std::uint32_t> have = deps_.qualities ? deps_.qualities() : std::vector<std::uint32_t>{};
  std::uint32_t first = s.quality;
  if (first == MV_AI_QUALITY_AUTO) {
    first = s.compute == MV_AI_COMPUTE_CPU_ONLY ? MV_AI_QUALITY_FAST : MV_AI_QUALITY_HIGH;
  }
  if (std::find(have.begin(), have.end(), first) == have.end() && !have.empty()) first = have.front();
  // "The first time on this Mac takes a few minutes" only when it is: Core
  // ML's cache holds no compiled copy of this tower (the status flag).
  first_compile_ = deps_.first_compile && deps_.first_compile(first, s.compute);
  if (first_compile_) {
    refresh_counts();
    post(MV_ADDON_EVENT_AI_STATUS);
  }
  auto opened = deps_.open_clip(first, s.compute);
  if (opened) {
    const std::uint32_t want = effective_quality(opened->on);
    if (want != opened->meta.quality) {
      auto other = deps_.open_clip(want, s.compute);
      if (other) {
        retire(std::move(opened->model));
        opened = std::move(other);
      }
    }
  }
  if (!opened) {
    models_ready_ = false;
    models_failed_ = true;
    post(MV_ADDON_EVENT_AI_COMPUTE, 0, -1);
    return;
  }
  // The generic prompts, embedded once per tower (the result margin, PR 20).
  const auto embed_generic = [](loaded_clip& c) {
    c.generic.clear();
    for (const std::string& p : c.meta.generic_prompts) {
      if (auto v = c.model->embed_text(p)) c.generic.push_back(std::move(*v));
    }
  };
  embed_generic(*opened);
  loaded_clip build = std::move(*opened);

  // The tower that answers queries is the one whose vectors are in the index
  // (the active spec); a different build spec is a migration in progress.
  std::string active = db_->meta("active_spec");
  const bool fresh = active.empty() || db_->count(active).frames == 0;
  loaded_clip answer = build;
  if (!fresh && active != build.meta.spec_key) {
    bool found = false;
    {
      // Migrating already and the answering tower is still in: keep it
      // rather than open it a second time.
      std::lock_guard lock(models_m_);
      if (answer_.model && answer_.meta.spec_key == active) {
        answer = answer_;
        found = true;
      }
    }
    for (std::uint32_t q : have) {
      if (found) break;
      if (q == build.meta.quality) continue;
      auto other = deps_.open_clip(q, MV_AI_COMPUTE_CPU_ONLY);
      if (other && other->meta.spec_key == active) {
        embed_generic(*other);
        answer = std::move(*other);
        found = true;
      } else if (other) {
        retire(std::move(other->model));
      }
    }
    if (!found) {
      // The pack no longer carries the model those vectors came from: they
      // cannot answer a query, so they go (never mixed; PR 23).
      (void)db_->drop_spec(active);
      active = build.meta.spec_key;
    }
  } else {
    active = build.meta.spec_key;
  }
  (void)db_->set_meta("active_spec", active);
  faces_parts people = open_faces_parts(s);
  const std::uint32_t speech_quality = build.meta.quality;
  const std::uint32_t answer_dim = answer.meta.dim;
  // What goes out is released below, after the swap and with no lock held:
  // a tower's destructor can wait minutes for a Core ML compile (retire).
  loaded_clip old_build, old_answer;
  faces_parts old_people;
  loaded_sound old_sound;
  loaded_speech old_speech;
  begin_answer_swap();
  {
    std::lock_guard lock(models_m_);
    old_build = std::move(build_);
    old_answer = std::move(answer_);
    old_people.model = std::move(faces_model_);
    old_people.db = std::move(faces_);
    old_sound = std::move(sound_);
    old_speech = std::move(speech_);
    build_ = std::move(build);
    answer_ = std::move(answer);
    build_compute_ = s.compute;
    faces_model_ = std::move(people.model);
    faces_ = std::move(people.db);
    faces_scanned_ = std::move(people.scanned);
    sound_ = {};
    speech_ = {};
    publish_pieces_locked();
  }
  load_vectors(active, answer_dim);
  sounds_.reset(0);
  {
    std::lock_guard lock(speech_m_);
    speech_rows_.clear();
  }
  end_answer_swap();
  retire(std::move(old_build.model));
  retire(std::move(old_answer.model));
  {
    std::lock_guard lock(work_m_);
    queue_.clear();
    queue_exhausted_ = false;
  }
  // Pictures index and answer from here; the audio models (another ~15 s on
  // a GPU) load behind them rather than holding the whole pack in LOADING.
  models_failed_ = false;
  models_ready_ = true;
  loading_ = false;
  work_cv_.notify_all();
  infer::backend landed = infer::backend::cpu;
  {
    std::lock_guard lock(models_m_);
    landed = build_.on;
  }
  post(MV_ADDON_EVENT_AI_COMPUTE, 0, static_cast<std::int64_t>(landed));

  load_labels();
  load_audio(s, speech_quality, false);
}

void engine::retire(std::shared_ptr<infer::embedder> model) {
  if (!model) return;
  {
    std::lock_guard lock(models_m_);
    if (model == build_.model || model == answer_.model) return;  // still in service
  }
  for (const auto& r : retired_) {
    if (r == model) return;
  }
  retired_.push_back(std::move(model));
  reap_retired();
}

void engine::reap_retired() {
  // use_count 1: nothing else can reach it (it is in neither slot), so no
  // other thread can take a copy between the test and the release.
  retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                [](const std::shared_ptr<infer::embedder>& m) {
                                  return m.use_count() == 1 && !m->settling();
                                }),
                 retired_.end());
}

void engine::publish_pieces_locked() {
  pieces_view v;
  v.faces_ready = faces_model_ != nullptr;
  v.sound_ready = sound_.model != nullptr;
  v.speech_ready = speech_.model != nullptr;
  v.sound_name = sound_.name;
  v.speech_name = speech_.name;
  std::lock_guard lock(pieces_m_);
  pieces_ = std::move(v);
}

std::uint64_t engine::settled_answer_gen() const {
  // A swap holds the generation odd for the vector reload only (the towers
  // opened before it began): well under a second at 100 k frames.
  for (int i = 0; i < 1000 && !stopping_; ++i) {
    const std::uint64_t g = answer_gen_.load();
    if ((g & 1) == 0) return g;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return answer_gen_.load();
}

// People refinement (plan/17): re-checks one person's faces against the core
// of that person and of everyone else, in passes. Only when the user asks
// ("Refine" on the person in People); the caller's worker thread. The
// snapshot and the commit hold the People lock (a full snapshot reads every
// face vector: ~50 MB at 100 k faces); the compute between them holds
// nothing, so an indexing worker or a People call never waits on it, and a
// user correction made meanwhile wins (the commit skips any face that changed).
result<std::uint32_t> engine::person_refine(std::int64_t person) {
  refine_snapshot snap;
  refine_params params;
  std::string spec;
  std::uint32_t dim = 0;
  {
    std::lock_guard lock(models_m_);
    if (!faces_ || !faces_model_ || person <= 0) return err(status::invalid_arg);
    params = faces_model_->tuning().refine();
    spec = faces_model_->spec_key();
    dim = faces_model_->dim();
    snap = faces_->refine_begin(true);
  }
  refine_input in;
  in.dim = dim;
  in.emb = snap.emb;
  in.faces = snap.faces;
  in.fixed = snap.fixed;
  in.named = snap.named;
  in.regroup = true;
  in.focus = person;
  in.cancel = &stopping_;
  const refine_output out = refine_people(in, params);
  if (stopping_) return err(status::cancelled);
  refine_stats st;
  std::size_t rescanned = 0;
  {
    std::lock_guard lock(models_m_);
    if (!faces_ || faces_->serial() != snap.serial) return err(status::cancelled);  // People turned off or reopened
    st = faces_->refine_commit(snap, out);
    // Borderline stills from before flip averaging: the People pass looks
    // at them again, and the new vectors replace the old box by box.
    for (std::int64_t asset : st.recheck_assets) {
      if (faces_->rescan(asset, spec)) {
        faces_scanned_.erase(asset);
        ++rescanned;
      }
    }
  }
  if (rescanned > 0) {
    std::lock_guard lock(work_m_);
    queue_exhausted_ = false;
    work_cv_.notify_all();
  }
  if (st.changed()) post(MV_ADDON_EVENT_AI_PEOPLE);
  // Every move leaves `person` (focus): to someone else, to nobody, or to a
  // new person made of faces that left together.
  return st.evicted + st.moved + st.regrouped;
}

// "Re-analyse faces" (plan/17 "People model"): forget what the People pass
// has scanned, so the workers analyse every asset again. faces_db::add then
// replaces each face in place (box by box, keeping person, pin, rejections)
// and holds new faces back until settle_people files them.
expected engine::people_reanalyse() {
  if (importing_elsewhere()) return err(status::busy);
  {
    std::lock_guard lock(models_m_);
    if (!faces_ || !faces_model_) return err(status::invalid_arg);
    MV_TRY_VOID(faces_->rerun_all());
    faces_scanned_.clear();
  }
  {
    std::lock_guard lock(work_m_);
    queue_exhausted_ = false;
  }
  work_cv_.notify_all();
  refresh_counts();
  post(MV_ADDON_EVENT_AI_STATUS);
  return {};
}

// The settle at the end of a re-run. Every face is this model's now, the
// people are the user's (carried box by box); a full refinement with no
// focus judges every unpinned face: it may leave a person the new vectors
// say it never belonged to, move to the one it does, or (waiting since its
// re-analysis) join the person it clearly matches; waiting faces that match
// each other but nobody become new people. A few calls, until nothing moves,
// then the merge. Same lock discipline as person_refine: the snapshot and
// commit hold the People lock, the compute holds nothing.
void engine::settle_people() {
  settling_ = true;
  refresh_counts();
  post(MV_ADDON_EVENT_AI_STATUS);
  bool any = false;
  for (int round = 0; round < 3 && !stopping_; ++round) {
    refine_snapshot snap;
    refine_params params;
    std::uint32_t dim = 0;
    {
      std::lock_guard lock(models_m_);
      if (!faces_ || !faces_model_) break;
      params = faces_model_->tuning().refine();
      dim = faces_model_->dim();
      snap = faces_->refine_begin(true);
    }
    refine_input in;
    in.dim = dim;
    in.emb = snap.emb;
    in.faces = snap.faces;
    in.fixed = snap.fixed;
    in.named = snap.named;
    in.regroup = true;
    in.cancel = &stopping_;
    const refine_output out = refine_people(in, params);
    if (stopping_) break;
    refine_stats st;
    {
      std::lock_guard lock(models_m_);
      if (!faces_ || faces_->serial() != snap.serial) break;
      st = faces_->refine_commit(snap, out);
    }
    if (!st.changed()) break;
    any = true;
  }
  {
    std::lock_guard lock(models_m_);
    if (faces_ && faces_model_ && !stopping_) {
      if (faces_->consolidate(faces_model_->tuning().merge_at) > 0) any = true;
      faces_->rerun_done();
    }
  }
  settling_ = false;
  refresh_counts();
  if (any) post(MV_ADDON_EVENT_AI_PEOPLE);
  post(MV_ADDON_EVENT_AI_STATUS);
}

engine::faces_parts engine::open_faces_parts(const settings& s, std::shared_ptr<face_analyzer> model) {
  faces_parts out;
  if (!s.faces) return out;
  if (!model && deps_.open_faces) {
    if (auto f = deps_.open_faces(s.compute); f && *f) model = std::shared_ptr<face_analyzer>(std::move(*f));
  }
  if (model) {
    out.model = std::move(model);
    auto db = faces_db::open(join(data_dir_, "faces.db"), out.model->tuning(), out.model->dim(),
                             out.model->spec_key());
    if (db) {
      out.db = std::move(*db);
      for (std::int64_t a : out.db->scanned_assets(out.model->spec_key())) out.scanned.insert(a);
    } else {
      out.model.reset();
    }
  }
  return out;
}

// A piece (People, Sound) came or went, or People was turned on: re-read the
// pieces only. The picture towers keep running and nothing waits in LOADING:
// reopening them was a second Core ML compile of the tower (minutes, gigabytes
// written) on every piece install, while indexing and searches were stopped.
void engine::load_pieces() {
  if (!models_ready_) {
    // The towers are not in (they failed, or a full load is pending): the
    // full load reads the pieces as well.
    if (!models_failed_) return;
    reload_models_ = true;
    return;
  }
  settings s;
  {
    std::lock_guard lock(settings_m_);
    s = settings_;
  }
  std::uint32_t speech_quality;
  std::string faces_spec;  // "" when People is not running
  {
    std::lock_guard lock(models_m_);
    speech_quality = build_.meta.quality;
    if (faces_ && faces_model_) faces_spec = faces_model_->spec_key();
  }
  // People: the piece may have come or gone. Running already with the same
  // model: only the analyzer is swapped, so the database a worker is adding
  // to (and what it has scanned) stays. Anything else reopens it all.
  if (s.faces || !faces_spec.empty()) {
    std::shared_ptr<face_analyzer> model;
    if (s.faces && deps_.open_faces) {
      if (auto f = deps_.open_faces(s.compute); f && *f) model = std::shared_ptr<face_analyzer>(std::move(*f));
    }
    if (model && !faces_spec.empty() && model->spec_key() == faces_spec) {
      std::lock_guard lock(models_m_);
      faces_model_ = std::move(model);
      publish_pieces_locked();
    } else {
      faces_parts people = model ? open_faces_parts(s, std::move(model)) : faces_parts{};
      std::lock_guard lock(models_m_);
      faces_model_ = std::move(people.model);
      faces_ = std::move(people.db);
      faces_scanned_ = std::move(people.scanned);
      publish_pieces_locked();
    }
    post(MV_ADDON_EVENT_AI_PEOPLE);
  }
  load_audio(s, speech_quality, true);
  {
    std::lock_guard lock(work_m_);
    queue_.clear();
    queue_exhausted_ = false;
  }
  refresh_counts();
  work_cv_.notify_all();
  post(MV_ADDON_EVENT_AI_STATUS);
}

void engine::load_audio(const settings& s, std::uint32_t speech_quality, bool replacing) {
  // Audio (2026-09-27): only with the ai-audio piece. Speech follows the
  // picture tower's quality: Whisper small where CLIP High runs, else base.
  loaded_sound sound;
  loaded_speech speech;
  if ((deps_.open_sound || deps_.open_speech) && !wait_viewer_quiet()) return;
  if (deps_.open_sound && !stopping_) {
    if (auto so = deps_.open_sound(s.compute)) {
      sound = std::move(*so);
      sound.generic.clear();
      for (const std::string& g : sound.generic_prompts) {
        if (auto v = sound.model->embed_text(g)) sound.generic.push_back(std::move(*v));
      }
    }
  }
  if (deps_.open_speech && !stopping_) {
    if (auto sp = deps_.open_speech(speech_quality, s.compute)) speech = std::move(*sp);
  }
  if (!sound.model && !speech.model) {
    if (!replacing) return;
    // The piece went: its models first (a worker holding one finishes its
    // clip with its own copy), then what they answered from.
    {
      std::lock_guard lock(models_m_);
      sound_ = {};
      speech_ = {};
      publish_pieces_locked();
    }
    sounds_.reset(0);
    std::lock_guard lock(speech_m_);
    speech_rows_.clear();
    return;
  }
  // The stored vectors and transcripts first, then the models: a job that
  // lands between the two must not be wiped by the reload.
  sounds_.reset(sound.model ? sound.dim : 0);
  if (sound.model) {
    (void)db_->each_frame(sound.spec_key, [&](const frame_out& f) {
      sounds_.add(f.asset_id, f.pts_ms, f.generic, f.scale, f.emb);
    });
  }
  if (speech.model) {
    load_speech(speech.spec_key);
  } else {
    std::lock_guard lock(speech_m_);
    speech_rows_.clear();
  }
  {
    std::lock_guard lock(models_m_);
    sound_ = std::move(sound);
    speech_ = std::move(speech);
    publish_pieces_locked();
  }
  {
    std::lock_guard lock(work_m_);
    queue_.clear();
    queue_exhausted_ = false;
  }
  refresh_counts();
  work_cv_.notify_all();
  post(MV_ADDON_EVENT_AI_STATUS);
}

void engine::load_vectors(const std::string& spec, std::uint32_t dim) {
  store_.reset(dim);
  (void)db_->each_frame(spec, [&](const frame_out& f) { store_.add(f.asset_id, f.pts_ms, f.generic, f.scale, f.emb); });
}

void engine::load_labels() {
  // The label vocabulary in the answering tower's space (vocabulary.h, issue
  // #85): embedded once per tower and label list, kept in labels.f32 beside
  // the index (a few seconds of text tower on first use), then each row's bar
  // is filled behind the searches. Until then every row passes, as before.
  loaded_clip answer;
  {
    std::lock_guard lock(models_m_);
    answer.model = answer_.model;
    answer.meta = answer_.meta;
  }
  if (!answer.model || answer.meta.dim == 0 || store_.dim() != answer.meta.dim) return;
  const auto words = labels();
  std::uint64_t key = 1469598103934665603ull;  // FNV-1a of the tower and the list
  const auto mix = [&](std::string_view s) {
    for (unsigned char c : s) key = (key ^ c) * 1099511628211ull;
    key = (key ^ 0xFFu) * 1099511628211ull;
  };
  mix(answer.meta.spec_key);
  for (std::string_view w : words) mix(w);
  const std::uint32_t dim = answer.meta.dim;
  const std::uint32_t count = static_cast<std::uint32_t>(words.size());
  const std::string file = join(data_dir_, "labels.f32");
  std::vector<float> vocab;
  {
    std::ifstream in(fs_path(file), std::ios::binary);
    std::uint64_t k = 0;
    std::uint32_t n = 0, d = 0;
    if (in.read(reinterpret_cast<char*>(&k), 8) && in.read(reinterpret_cast<char*>(&n), 4) &&
        in.read(reinterpret_cast<char*>(&d), 4) && k == key && n == count && d == dim) {
      vocab.resize(std::size_t{count} * dim);
      if (!in.read(reinterpret_cast<char*>(vocab.data()), static_cast<std::streamsize>(vocab.size() * 4))) vocab.clear();
    }
  }
  if (vocab.empty()) {
    vocab.reserve(std::size_t{count} * dim);
    for (std::string_view w : words) {
      if (stopping_ || reload_models_) return;
      auto v = answer.model->embed_text(w);
      if (!v || v->size() != dim) return;
      vocab.insert(vocab.end(), v->begin(), v->end());
    }
    const std::string tmp = file + ".tmp";
    {
      std::ofstream out(fs_path(tmp), std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(&key), 8);
      out.write(reinterpret_cast<const char*>(&count), 4);
      out.write(reinterpret_cast<const char*>(&dim), 4);
      out.write(reinterpret_cast<const char*>(vocab.data()), static_cast<std::streamsize>(vocab.size() * 4));
    }
    std::error_code ec;
    std::filesystem::rename(fs_path(tmp), fs_path(file), ec);
  }
  // The tower may have changed while the labels embedded: the store is only
  // given labels of the tower whose vectors it holds.
  {
    std::lock_guard lock(models_m_);
    if (answer_.model != answer.model) return;
  }
  store_.set_labels(vocab, count);
}

void engine::maybe_finish_migration() {
  std::string build_spec, active_spec;
  {
    std::lock_guard lock(models_m_);
    build_spec = build_.meta.spec_key;
    active_spec = answer_.meta.spec_key;
  }
  if (build_spec.empty() || build_spec == active_spec) return;
  const counts c = db_->count(build_spec);
  if (c.assets > c.done + c.failed) return;
  // The new index is complete: it answers from now on, and the old vectors go.
  // The tower and its vectors change together (answer_gen_), and the old
  // tower is released off the lock (retire).
  loaded_clip old_answer;
  std::uint32_t dim = 0;
  begin_answer_swap();
  {
    std::lock_guard lock(models_m_);
    old_answer = std::move(answer_);
    answer_ = build_;
    dim = build_.meta.dim;
  }
  (void)db_->set_meta("active_spec", build_spec);
  load_vectors(build_spec, dim);
  end_answer_swap();
  retire(std::move(old_answer.model));
  (void)db_->drop_spec(active_spec);
  post(MV_ADDON_EVENT_AI_STATUS);
  load_labels();
}

// ---- control ------------------------------------------------------------------------------

void engine::control_loop() {
  loading_ = true;
  load_models();
  const unsigned cores = std::max(1u, platform::hardware_threads());
  const unsigned n = std::max(1u, std::min(2u, cores / 4));
  for (unsigned i = 0; i < n; ++i) workers_.emplace_back([this, i] { worker_loop(i); });
  double last_full_scan = 0;
  double last_photos_scan = 0;
  double last_consolidate = now_s();
  std::uint32_t last_state = 0xFFFF;
  while (!stopping_) {
    if (reload_models_.load()) {
      loading_ = true;  // before the flag clears (wait_idle reads them in the other order)
      reload_models_ = false;
      reload_pieces_ = false;  // a full load reads the pieces too
      load_models();
      refresh_counts();
      ++reloads_done_;
    } else if (reload_pieces_.exchange(false)) {
      load_pieces();
    }
    {
      // An export / import (plan/17 "Sharing an index"): here, so it never
      // runs beside a scan or a model load.
      std::optional<transfer_job> t;
      {
        std::lock_guard lock(transfer_m_);
        if (!transfer_queue_.empty()) {
          t = std::move(transfer_queue_.front());
          transfer_queue_.pop_front();
        }
      }
      if (t) run_transfer(std::move(*t));
    }
    bool all = false;
    std::set<std::int64_t> some;
    const double t = now_s();
    // PhotoKit reports bursts (an iCloud sync is many); one enumeration (~0.4 s
    // warm at 23 k assets) per ten seconds at most.
    if (photos_root_ != 0 && t - last_photos_scan >= deps_.photos_rescan_gap_s && photos_changed_.exchange(false)) {
      std::lock_guard lock(control_m_);
      rescan_roots_.insert(photos_root_);
    }
    {
      std::lock_guard lock(control_m_);
      all = rescan_all_;
      rescan_all_ = false;
      some.swap(rescan_roots_);
      scanning_ = all || !some.empty();
    }
    if (all || some.count(photos_root_)) last_photos_scan = t;
    if (all || (t - last_full_scan) > 15 * 60) {
      scan_all();
      last_full_scan = t;
    } else {
      for (const root_row& r : db_->roots()) {
        if (some.count(r.id) && r.enabled) scan_root(r);
      }
    }
    refresh_counts();
    scanning_ = false;
    maybe_finish_migration();
    reap_retired();
    // Plugging in ends an override even while no worker is asking (idle).
    if (battery_override_ && !power_state().on_battery) battery_override_ = false;
    {
      bool idle = false;
      {
        std::lock_guard wl(work_m_);
        idle = queue_exhausted_ && busy_workers_ == 0;
      }
      bool settle = false;
      {
        // Mean pairwise cosine now (faces_db::consolidate): the model's
        // merge_at (SFace 0.42) is just past same_person, where 0.55 to a
        // normalised centroid let two clusters whose faces averaged ~0.28
        // merge. Not during a re-run: its settle merges once, at the end,
        // as soon as the last asset is analysed.
        std::lock_guard lock(models_m_);
        if (faces_ && faces_model_ && idle) {
          if (faces_->rerun_pending()) {
            settle = true;
          } else if (t - last_consolidate > 60) {
            if (faces_->consolidate(faces_model_->tuning().merge_at) > 0) post(MV_ADDON_EVENT_AI_PEOPLE);
            last_consolidate = t;
          }
        }
      }
      if (settle) settle_people();
    }
    std::uint32_t state;
    {
      std::lock_guard lock(status_m_);
      state = status_.state;
    }
    if (state != last_state || state == MV_AI_STATE_INDEXING || state == MV_AI_STATE_YIELDING) {
      post(MV_ADDON_EVENT_AI_STATUS);
      last_state = state;
    }
    std::unique_lock lock(control_m_);
    control_cv_.wait_for(lock, std::chrono::milliseconds(1000), [this] {
      if (stopping_.load() || reload_models_.load() || reload_pieces_.load() || rescan_all_ ||
          !rescan_roots_.empty()) {
        return true;
      }
      std::lock_guard tl(transfer_m_);  // control_m_ -> transfer_m_ (a leaf)
      return !transfer_queue_.empty();
    });
  }
}

void engine::scan_all() {
  for (const root_row& r : db_->roots()) {
    if (stopping_) return;
    if (r.enabled) scan_root(r);
  }
}

bool engine::see_batch(const root_row& root, std::vector<index_db::seen_file>& batch, std::int64_t gen,
                       std::vector<std::int64_t>& changed) {
  if (batch.empty()) return true;
  auto r = db_->see_assets(root.id, batch, gen);
  if (!r) {
    batch.clear();
    return false;
  }
  std::lock_guard lock(assets_m_);
  for (std::size_t i = 0; i < batch.size(); ++i) {
    const index_db::upsert& u = (*r)[i];
    if (u.changed) changed.push_back(u.id);
    asset_meta& m = assets_[u.id];
    m.path = batch[i].path;
    m.key = path_key(m.path);
    m.dir_key = dir_of(m.key);
    m.kind = batch[i].kind;
    m.root = root.id;
    m.mtime = batch[i].mtime;
  }
  batch.clear();
  return true;
}

void engine::scan_root(const root_row& root) {
  if (is_photos_key(root.path)) {
    scan_photos(root);
    return;
  }
  {
    std::lock_guard lock(status_m_);
    active_root_ = root.path;
  }
  const std::int64_t gen = db_->next_generation();
  std::vector<index_db::seen_file> batch;
  std::vector<std::int64_t> changed;
  bool failed = false;
  const auto flush = [&] {
    if (!see_batch(root, batch, gen, changed)) failed = true;
  };
  // Live Photo / RAW+JPEG: one row per pair, on the still (plan/17 step 6).
  // The host's own pairing decides, per directory.
  std::map<std::string, std::vector<host::entry>> by_dir;
  const expected walked = host_.walk(root.path, root.recursive ? 64 : 0, [&](const host::entry& e) {
    if (stopping_) return false;
    const std::string key = path_key(e.path);
    if (housekeeping(key) || media_kind_of_name(e.path) == 0) return true;
    by_dir[key_parent(key)].push_back(e);
    return true;
  });
  if (!walked && walked.error() != status::cancelled) {
    // An unreachable root (a share offline, a card pulled) keeps its index:
    // deleting rows because a drive is absent would throw work away.
    return;
  }
  if (stopping_) return;
  for (auto& [dir, files] : by_dir) {
    std::vector<std::string> names;
    names.reserve(files.size());
    for (const host::entry& e : files) {
      const std::size_t slash = e.path.find_last_of("/\\");
      names.push_back(slash == std::string::npos ? e.path : e.path.substr(slash + 1));
    }
    std::vector<std::uint32_t> partner(names.size(), UINT32_MAX), kind(names.size(), 0);
    if (host_.api()->pair_names) {
      std::vector<const char*> raw;
      for (const std::string& n : names) raw.push_back(n.c_str());
      (void)host_.api()->pair_names(host_.api()->host, raw.data(), static_cast<uint32_t>(raw.size()),
                                    partner.data(), kind.data());
    }
    for (std::size_t i = 0; i < files.size(); ++i) {
      const int k = media_kind_of_name(names[i]);
      if (partner[i] != UINT32_MAX) {
        const int other = media_kind_of_name(names[partner[i]]);
        // Keep the still: drop a Live Photo's clip, and a RAW's JPEG twin's RAW.
        if (kind[i] == 2 && k == 2 && other == 1) continue;
        if (kind[i] == 1) {
          const std::string ext = extension_of(names[i]);
          const bool is_jpeg_like = ext == "jpg" || ext == "jpeg" || ext == "heic" || ext == "heif" || ext == "hif";
          if (!is_jpeg_like) continue;
        }
      }
      batch.push_back(index_db::seen_file{files[i].path, files[i].mtime, files[i].size,
                                          k == 2 ? asset_kind::video : asset_kind::photo});
      if (batch.size() >= 512) flush();
    }
  }
  flush();
  if (failed) return;
  end_root_scan(root, gen, changed);
}

// The Photos library (issue #72): PhotoKit's enumeration in place of the
// folder walk. Live Photos and bursts are already one asset each there.
void engine::scan_photos(const root_row& root) {
  photos_root_ = root.id;
  const photos_access access = photos_ ? photos_->access() : photos_access::unsupported;
  const bool was_readable = photos_readable_.exchange(readable(access));
  if (!readable(access)) {
    // Access off (or never granted): keep the rows, like an unreachable
    // drive, and leave the root out of the work until it comes back.
    if (was_readable) {
      std::lock_guard lock(work_m_);
      queue_.clear();
      queue_exhausted_ = false;
    }
    post(MV_ADDON_EVENT_AI_ROOTS, static_cast<std::uint64_t>(root.id));
    return;
  }
  // (Re)attached each scan: a no-op once observing, and the first scan after
  // access is granted starts it.
  photos_->observe([this] {
    photos_changed_ = true;
    control_cv_.notify_all();
  });
  if (!was_readable) {
    // Once per launch (and when access comes back): what was only in iCloud
    // may be on this Mac now. Each unavailable asset is asked whether it is
    // local now (a PhotoKit lookup, milliseconds; a still that is still in
    // iCloud answers without a decode), and only those that are go back to
    // pending. The rest keep their state and their stand-in rows (a clip's
    // poster): nothing is re-read or re-embedded for nothing. Not on every
    // change either, which would ask about every iCloud-only asset each time
    // a photo is favourited.
    std::vector<std::int64_t> again;
    for (const asset_row& a : db_->unavailable_assets(root.id)) {
      if (stopping_) return;
      if (photos_local(a)) again.push_back(a.id);
    }
    if (!again.empty() && db_->requeue_unavailable(again)) forget_vectors(again);
  }
  {
    std::lock_guard lock(status_m_);
    active_root_ = root.path;
  }
  const std::int64_t gen = db_->next_generation();
  std::vector<index_db::seen_file> batch;
  std::vector<std::int64_t> changed;
  bool failed = false;
  const expected walked = photos_->enumerate([&](const photos_item& it) {
    if (stopping_ || failed) return false;
    batch.push_back(index_db::seen_file{photos_key(it.id), it.mtime, it.size, it.kind});
    if (batch.size() >= 512 && !see_batch(root, batch, gen, changed)) failed = true;
    return !failed;
  });
  if (!walked || stopping_ || failed) return;  // keep what is indexed
  if (!see_batch(root, batch, gen, changed)) return;
  end_root_scan(root, gen, changed);
}

bool engine::photos_local(const asset_row& a) const {
  if (!photos_) return false;
  const std::string_view id = photos_id(a.path);
  if (a.kind == asset_kind::video) return photos_->video_file(id).has_value();
  return photos_->still(id, kPhotosProbeEdge).has_value();
}

void engine::forget_vectors(const std::vector<std::int64_t>& ids) {
  if (ids.empty()) return;
  for (std::int64_t id : ids) {
    store_.remove_asset(id);
    sounds_.remove_asset(id);
  }
  {
    // Their People pass too: an unavailable asset was marked scanned so the
    // faces track would not ask for it again, and now there is something to scan.
    std::lock_guard lock(models_m_);
    if (faces_) {
      for (std::int64_t id : ids) {
        (void)faces_->forget_asset(id);
        faces_scanned_.erase(id);
      }
    }
  }
  const std::set<std::int64_t> drop(ids.begin(), ids.end());
  {
    std::lock_guard lock(speech_m_);
    speech_rows_.erase(std::remove_if(speech_rows_.begin(), speech_rows_.end(),
                                      [&](const speech_row& r) { return drop.count(r.asset) != 0; }),
                       speech_rows_.end());
  }
  {
    std::lock_guard lock(work_m_);
    queue_.clear();
    queue_exhausted_ = false;
  }
  work_cv_.notify_all();
}

void engine::end_root_scan(const root_row& root, std::int64_t gen, const std::vector<std::int64_t>& changed) {
  auto gone = db_->end_scan(root.id, gen);
  if (gone) {
    std::lock_guard lock(assets_m_);
    for (std::int64_t id : *gone) {
      assets_.erase(id);
      store_.remove_asset(id);
      sounds_.remove_asset(id);
    }
  }
  if (gone) {
    std::lock_guard lock(models_m_);
    if (faces_) {
      for (std::int64_t id : *gone) {
        (void)faces_->forget_asset(id);
        faces_scanned_.erase(id);
      }
    }
  }
  if (gone || !changed.empty()) {
    std::set<std::int64_t> drop(changed.begin(), changed.end());
    if (gone) drop.insert(gone->begin(), gone->end());
    std::lock_guard lock(speech_m_);
    speech_rows_.erase(std::remove_if(speech_rows_.begin(), speech_rows_.end(),
                                      [&](const speech_row& r) { return drop.count(r.asset) != 0; }),
                       speech_rows_.end());
  }
  for (std::int64_t id : changed) {
    store_.remove_asset(id);
    sounds_.remove_asset(id);
    std::lock_guard lock(models_m_);
    if (faces_) {
      (void)faces_->forget_asset(id);
      faces_scanned_.erase(id);
    }
  }
  (void)db_->touch_root(root.id, unix_now());
  {
    std::lock_guard lock(work_m_);
    queue_.clear();
    queue_exhausted_ = false;
  }
  {
    std::lock_guard lock(status_m_);
    active_root_.clear();
  }
  work_cv_.notify_all();
  post(MV_ADDON_EVENT_AI_ROOTS);
}

// ---- pixels and files: a file or a Photos library asset -----------------------------------------

result<rgb_frame> engine::still_of(const std::string& path, std::uint32_t edge) const {
  if (!is_photos_key(path)) return host_.decode_still(path, edge);
  if (!photos_) return err(status::unsupported_format);
  return photos_->still(photos_id(path), edge);
}

result<std::string> engine::file_of(const std::string& path) const {
  if (!is_photos_key(path)) return path;
  if (!photos_) return err(status::unsupported_format);
  return photos_->video_file(photos_id(path));
}

result<rgb_frame> engine::frame_of(const std::string& path, std::int64_t pts_ms, std::uint32_t edge) const {
  if (pts_ms < 0) return still_of(path, edge);
  auto file = file_of(path);
  if (!file) {
    // An iCloud-only clip: its poster is what was indexed.
    if (is_photos_key(path) && file.error() == status::io) return still_of(path, edge);
    return err(file.error());
  }
  return host_.video_frame(*file, pts_ms, edge);
}

// ---- workers --------------------------------------------------------------------------------

mv_ai_yield engine::yield_reason() const {
  if (host_.should_yield()) return MV_AI_YIELD_VIEWER;
  int threshold;
  {
    std::lock_guard lock(settings_m_);
    threshold = settings_.battery_percent;
  }
  const platform::power p = power_state();
  if (!p.on_battery) {
    // Back on AC: "Index anyway" was for that spell on battery only.
    battery_override_ = false;
    return MV_AI_YIELD_NONE;
  }
  if (p.percent < threshold && !battery_override_) return MV_AI_YIELD_BATTERY;
  return MV_AI_YIELD_NONE;
}

platform::power engine::power_state() const {
  return deps_.power ? deps_.power() : platform::power_state();
}

bool engine::wait_turn() {
  while (!stopping_) {
    if (clearing_ || transferring_ || !models_ready_ || loading_ || paused_ || index_full_) {
      std::unique_lock lock(work_m_);
      work_cv_.wait_for(lock, std::chrono::milliseconds(200));
      continue;
    }
    const mv_ai_yield y = yield_reason();
    yield_now_ = y;
    if (y == MV_AI_YIELD_NONE) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

std::uint32_t engine::default_media() const {
  std::uint32_t v;
  {
    std::lock_guard lock(settings_m_);
    v = settings_.video_index;
  }
  if (v != MV_AI_MEDIA_DEFAULT) return v;
  // Unset: pictures, and sound too once the ai-audio piece is installed
  // (installing it is the choice to index what clips sound like).
  std::lock_guard lock(pieces_m_);
  return pieces_.sound_ready ? MV_AI_MEDIA_BOTH : MV_AI_MEDIA_PICTURES;
}

bool engine::claim(std::vector<work_item>& out, track& t) {
  out.clear();
  t = track::picture;
  std::string spec, sound_spec, speech_spec;
  bool faces_on = false;
  const std::uint32_t media = default_media();
  {
    std::lock_guard lock(models_m_);
    spec = build_.meta.spec_key;
    faces_on = faces_ != nullptr;
    if (sound_.model) sound_spec = sound_.spec_key;
    if (speech_.model) speech_spec = speech_.spec_key;
  }
  if (spec.empty()) return false;
  std::lock_guard lock(work_m_);
  if (queue_.empty() && !queue_exhausted_) {
    // Pictures first (fast, and what most searches need), then what clips
    // sound like, then what is said in them, then the People pass.
    const auto take = [&](const std::string& track_spec, track which, const track_filter& f) {
      if (!queue_.empty() || track_spec.empty()) return;
      for (work_item& w : db_->pending(track_spec, 256, kMaxTries, f)) {
        if (!in_flight_.count(w.asset.id)) queue_.push_back(job{std::move(w), which});
      }
    };
    const std::int64_t skip = skip_root();
    take(spec, track::picture, track_filter{true, MV_AI_MEDIA_PICTURES, media, skip});
    take(sound_spec, track::sound, track_filter{false, MV_AI_MEDIA_SOUND, media, skip});
    take(speech_spec, track::speech, track_filter{false, MV_AI_MEDIA_SOUND, media, skip});
    if (queue_.empty() && faces_on) {
      std::lock_guard ml(models_m_);
      std::lock_guard al(assets_m_);
      const std::int64_t skip_faces = skip_root();
      for (const auto& [id, m] : assets_) {
        if (faces_scanned_.count(id) || in_flight_.count(id)) continue;
        if (skip_faces != 0 && m.root == skip_faces) continue;
        work_item w;
        w.asset.id = id;
        w.asset.path = m.path;
        w.asset.kind = m.kind;
        w.asset.root_id = m.root;
        queue_.push_back(job{std::move(w), track::faces});
        if (queue_.size() >= 64) break;
      }
    }
    if (queue_.empty()) {
      queue_exhausted_ = true;
      return false;
    }
    // The folder on screen first (plan/17: searchable as results commit).
    if (!prefer_dir_key_.empty()) {
      std::lock_guard al(assets_m_);
      std::stable_partition(queue_.begin(), queue_.end(), [&](const job& j) {
        auto it = assets_.find(j.w.asset.id);
        return it != assets_.end() &&
               (it->second.dir_key == prefer_dir_key_ || under(it->second.dir_key, prefer_dir_key_));
      });
    }
  }
  if (queue_.empty()) return false;
  t = queue_.front().t;
  const bool photo = queue_.front().w.asset.kind == asset_kind::photo;
  const std::size_t batch = photo && (t == track::picture || t == track::faces) ? kPhotoBatch : 1;
  while (!queue_.empty() && out.size() < batch) {
    const job& j = queue_.front();
    if ((j.w.asset.kind == asset_kind::photo) != photo || j.t != t) break;
    if (in_flight_.count(j.w.asset.id)) {  // another track of this asset is running
      queue_.pop_front();
      continue;
    }
    in_flight_.insert(j.w.asset.id);
    out.push_back(j.w);
    queue_.pop_front();
  }
  if (out.empty()) return false;
  ++busy_workers_;
  return true;
}

void engine::worker_loop(unsigned) {
  platform::enter_background();
  while (!stopping_) {
    if (!wait_turn()) break;
    std::vector<work_item> items;
    track t = track::picture;
    if (!claim(items, t)) {
      std::unique_lock lock(work_m_);
      work_cv_.wait_for(lock, std::chrono::milliseconds(500));
      continue;
    }
    loaded_clip clip;
    loaded_sound sound;
    loaded_speech speech;
    {
      std::lock_guard lock(models_m_);
      clip = build_;
      sound = sound_;
      speech = speech_;
    }
    const bool faces_only = t == track::faces;
    if (t == track::sound) {
      if (sound.model) process_sound(items.front(), sound);
    } else if (t == track::speech) {
      if (speech.model) process_speech(items.front(), speech);
    } else if (clip.model) {
      if (items.front().asset.kind == asset_kind::video) {
        process_video(items.front(), clip, faces_only);
      } else {
        process_photos(items, clip, faces_only);
      }
    }
    {
      std::lock_guard lock(work_m_);
      for (const work_item& w : items) in_flight_.erase(w.asset.id);
      if (busy_workers_ > 0) --busy_workers_;
      // Another track of this asset (its speech after its sound) was passed
      // over while this ran: look again.
      queue_exhausted_ = false;
    }
    work_cv_.notify_all();
  }
  yield_now_ = MV_AI_YIELD_NONE;
}

// ---- audio (2026-09-27) ----------------------------------------------------------

void engine::process_sound(const work_item& item, const loaded_sound& sound) {
  const mv_host_api* api = host_.api();
  if (!api->audio_open) return;
  void* handle = nullptr;
  std::int64_t duration = 0;
  auto file = file_of(item.asset.path);
  if (!file) {
    if (file.error() == status::permission_denied) {
      photos_readable_ = false;
      return;
    }
    if (file.error() == status::io && is_photos_key(item.asset.path)) {
      (void)db_->mark_unavailable(item.asset.id, sound.spec_key);
    } else {
      (void)db_->fail(item.asset.id, sound.spec_key);
    }
    return;
  }
  if (api->audio_open(api->host, file->c_str(), 48000, item.resume_ms, &duration, &handle) != MV_OK) {
    // No soundtrack (or unreadable): nothing to index for sound. Done, not failed.
    (void)db_->commit_frames(item.asset.id, sound.spec_key, {}, work_state::done, 0, sound.dim);
    return;
  }
  struct closer {
    const mv_host_api* api;
    void* h;
    ~closer() { api->audio_close(api->host, h); }
  } const close_it{api, handle};
  if (duration > 0) (void)db_->set_duration(item.asset.id, duration);
  const std::size_t window = static_cast<std::size_t>(sound.window_ms) * 48;  // samples
  const std::size_t hop = static_cast<std::size_t>(sound.hop_ms) * 48;
  std::vector<float> buf;             // samples not yet a full window behind them
  std::int64_t buf_start = -1;        // time of buf[0]
  std::vector<float> chunk(48000 * 2);
  std::vector<float> last_kept;
  if (item.resume_ms > 0) {
    // Resumed: dedupe against the last committed window (as pictures do).
    std::vector<float> prev;
    if (sounds_.vector_of(item.asset.id, item.resume_ms, prev) && prev.size() == sound.dim) {
      last_kept = std::move(prev);
    }
  }
  bool eof = false;
  std::int64_t done_ms = item.resume_ms;
  std::string active_sound = sound.spec_key;
  while (true) {
    // Fill four windows' worth (one batch), then embed.
    std::vector<std::pair<std::int64_t, std::vector<float>>> windows;
    while (windows.size() < 4) {
      while (buf.size() < window && !eof) {
        if (!wait_turn()) return;  // resumes at done_ms
        uint32_t got = 0;
        int64_t t0 = 0;
        if (api->audio_read(api->host, handle, chunk.data(), static_cast<uint32_t>(chunk.size()), &got, &t0) != MV_OK) {
          eof = true;
          break;
        }
        if (got == 0) {
          eof = true;
          break;
        }
        if (buf_start < 0) buf_start = t0;
        buf.insert(buf.end(), chunk.begin(), chunk.begin() + got);
      }
      if (buf.empty() || (eof && buf.size() < window / 10 && !windows.empty())) break;
      const std::size_t n = std::min(window, buf.size());
      windows.push_back({buf_start, std::vector<float>(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n))});
      if (eof && buf.size() <= window) {
        buf.clear();
        break;
      }
      const std::size_t drop = std::min(hop, buf.size());
      buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(drop));
      buf_start += static_cast<std::int64_t>(drop / 48);
    }
    if (windows.empty()) break;
    std::vector<std::span<const float>> spans;
    for (const auto& w : windows) spans.emplace_back(w.second);
    std::vector<float> embs;
    if (!sound.model->embed_audio(spans, embs)) {
      (void)db_->fail(item.asset.id, sound.spec_key);
      return;
    }
    std::vector<frame_in> keep;
    for (std::size_t i = 0; i < windows.size(); ++i) {
      const std::span<const float> e(embs.data() + i * sound.dim, sound.dim);
      if (!last_kept.empty() && infer::dot(e, last_kept) >= sound.dedupe) continue;
      last_kept.assign(e.begin(), e.end());
      frame_in f;
      f.pts_ms = windows[i].first;
      f.flags = 0;
      f.emb = e;
      f.generic = max_dot(e, sound.generic);
      keep.push_back(f);
    }
    const bool finished = eof && buf.empty();
    done_ms = windows.back().first + static_cast<std::int64_t>(sound.hop_ms);
    if (!db_->commit_frames(item.asset.id, sound.spec_key, keep, finished ? work_state::done : work_state::partial,
                            done_ms, sound.dim)) {
      return;
    }
    for (const frame_in& f : keep) {
      std::vector<std::int8_t> q;
      float scale = 1;
      quantise(f.emb, q, scale);
      sounds_.add(item.asset.id, f.pts_ms, f.generic, scale, q);
    }
    {
      std::lock_guard lock(status_m_);
      units_done_ += static_cast<double>(windows.size()) * sound.hop_ms / 1000.0;
    }
    if (finished) return;
  }
  (void)db_->commit_frames(item.asset.id, sound.spec_key, {}, work_state::done, done_ms, sound.dim);
}

void engine::process_speech(const work_item& item, const loaded_speech& speech) {
  const mv_host_api* api = host_.api();
  if (!api->audio_open) return;
  void* handle = nullptr;
  std::int64_t duration = 0;
  auto file = file_of(item.asset.path);
  if (!file) {
    if (file.error() == status::permission_denied) {
      photos_readable_ = false;
      return;
    }
    if (file.error() == status::io && is_photos_key(item.asset.path)) {
      (void)db_->mark_unavailable(item.asset.id, speech.spec_key);
    } else {
      (void)db_->fail(item.asset.id, speech.spec_key);
    }
    return;
  }
  if (api->audio_open(api->host, file->c_str(), 16000, item.resume_ms, &duration, &handle) != MV_OK) {
    (void)db_->commit_speech(item.asset.id, speech.spec_key, {}, work_state::done, 0);
    return;
  }
  struct closer {
    const mv_host_api* api;
    void* h;
    ~closer() { api->audio_close(api->host, h); }
  } const close_it{api, handle};
  constexpr std::size_t kWindow = 16000 * 30;
  std::vector<float> buf;
  std::int64_t buf_start = -1;
  std::vector<float> chunk(16000 * 5);
  bool eof = false;
  while (true) {
    while (buf.size() < kWindow && !eof) {
      if (!wait_turn()) return;
      uint32_t got = 0;
      int64_t t0 = 0;
      if (api->audio_read(api->host, handle, chunk.data(), static_cast<uint32_t>(chunk.size()), &got, &t0) != MV_OK ||
          got == 0) {
        eof = true;
        break;
      }
      if (buf_start < 0) buf_start = t0;
      buf.insert(buf.end(), chunk.begin(), chunk.begin() + got);
    }
    if (buf.empty()) break;
    if (!wait_turn()) return;
    const std::size_t n = std::min(kWindow, buf.size());
    auto w = speech.model->transcribe(std::span<const float>(buf.data(), n), buf_start);
    if (!w) {
      (void)db_->fail(item.asset.id, speech.spec_key);
      return;
    }
    const std::size_t consumed = std::min(n, static_cast<std::size_t>(std::max<std::int64_t>(w->consumed_ms, 1000)) * 16);
    const bool finished = eof && consumed >= buf.size();
    std::vector<speech_in> rows;
    for (const infer::speech_segment& seg : w->segments) {
      rows.push_back(speech_in{seg.start_ms, seg.end_ms, seg.text});
    }
    const std::int64_t next_ms = buf_start + static_cast<std::int64_t>(consumed / 16);
    if (!db_->commit_speech(item.asset.id, speech.spec_key, rows, finished ? work_state::done : work_state::partial,
                            next_ms)) {
      return;
    }
    {
      std::lock_guard lock(speech_m_);
      for (const speech_in& r : rows) {
        speech_rows_.push_back(speech_row{item.asset.id, r.start_ms, r.text, infer::speech_words(r.text)});
      }
    }
    {
      std::lock_guard lock(status_m_);
      units_done_ += static_cast<double>(consumed) / 16000.0;
    }
    buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(consumed));
    buf_start = next_ms;
    if (finished) return;
  }
  (void)db_->commit_speech(item.asset.id, speech.spec_key, {}, work_state::done, buf_start < 0 ? 0 : buf_start);
}

void engine::faces_of(std::int64_t asset, const std::string& path, std::int64_t pts_ms, const rgb_frame& img) {
  std::shared_ptr<face_analyzer> model;
  {
    std::lock_guard lock(models_m_);
    model = faces_model_;
  }
  if (!model || img.width == 0) return;
  auto found = model->analyze(img);
  if (!found || found->empty()) return;
  std::lock_guard lock(models_m_);
  if (faces_) {
    if (faces_->add(asset, path, pts_ms, *found)) post(MV_ADDON_EVENT_AI_PEOPLE);
  }
}

void engine::process_photos(std::vector<work_item>& items, const loaded_clip& clip, bool faces_only) {
  bool faces_on = false;
  std::string face_spec;
  {
    std::lock_guard lock(models_m_);
    faces_on = faces_model_ != nullptr;
    if (faces_model_) face_spec = faces_model_->spec_key();
  }
  const std::uint32_t edge = faces_on ? 1024u : std::max<std::uint32_t>(448u, clip.meta.input_edge * 2);
  std::vector<rgb_frame> imgs;
  std::vector<const work_item*> ok;
  for (const work_item& w : items) {
    auto img = still_of(w.asset.path, edge);
    if (!img) {
      // Photos access went away mid-run: leave it pending; the next scan
      // takes the library out of the work until access is back.
      if (img.error() == status::permission_denied) {
        photos_readable_ = false;
        continue;
      }
      if (!faces_only) {
        // Only in iCloud (Optimize Mac Storage): nothing local to read.
        if (img.error() == status::io && is_photos_key(w.asset.path)) {
          (void)db_->mark_unavailable(w.asset.id, clip.meta.spec_key);
        } else {
          (void)db_->fail(w.asset.id, clip.meta.spec_key);
        }
      }
      std::lock_guard lock(models_m_);
      if (faces_ && faces_on) {
        (void)faces_->mark_scanned(w.asset.id, face_spec);
        faces_scanned_.insert(w.asset.id);
      }
      continue;
    }
    imgs.push_back(std::move(*img));
    ok.push_back(&w);
  }
  if (imgs.empty()) return;
  if (!faces_only) {
    std::vector<infer::rgb_view> views;
    for (const rgb_frame& f : imgs) views.push_back(infer::rgb_view{f.rgb.data(), f.width, f.height});
    std::vector<float> embs;
    if (!clip.model->embed_images(views, embs)) {
      for (const work_item* w : ok) (void)db_->fail(w->asset.id, clip.meta.spec_key);
      return;
    }
    std::string active;
    {
      std::lock_guard lock(models_m_);
      active = answer_.meta.spec_key;
    }
    for (std::size_t i = 0; i < ok.size(); ++i) {
      const std::span<const float> e(embs.data() + i * clip.meta.dim, clip.meta.dim);
      frame_in f;
      f.emb = e;
      f.generic = max_dot(e, clip.generic);
      if (!db_->commit_frames(ok[i]->asset.id, clip.meta.spec_key, std::span<const frame_in>(&f, 1),
                              work_state::done, 0, clip.meta.dim)) {
        continue;
      }
      if (clip.meta.spec_key == active) {
        std::vector<std::int8_t> q;
        float scale = 1;
        quantise(e, q, scale);
        store_.add(ok[i]->asset.id, -1, f.generic, scale, q);
      }
      std::lock_guard lock(status_m_);
      units_done_ += 1;
      frames_done_ += 1;
    }
  }
  if (faces_on) {
    for (std::size_t i = 0; i < ok.size(); ++i) {
      faces_of(ok[i]->asset.id, ok[i]->asset.path, -1, imgs[i]);
      std::lock_guard lock(models_m_);
      if (faces_) {
        (void)faces_->mark_scanned(ok[i]->asset.id, face_spec);
        faces_scanned_.insert(ok[i]->asset.id);
      }
    }
  }
}

void engine::index_poster(const work_item& item, const loaded_clip& clip, std::uint32_t edge) {
  auto img = still_of(item.asset.path, edge);
  if (!img) {
    (void)db_->mark_unavailable(item.asset.id, clip.meta.spec_key);
    return;
  }
  const infer::rgb_view view{img->rgb.data(), img->width, img->height};
  std::vector<float> e;
  if (!clip.model->embed_images(std::span<const infer::rgb_view>(&view, 1), e) || e.size() != clip.meta.dim) {
    (void)db_->fail(item.asset.id, clip.meta.spec_key);
    return;
  }
  frame_in f;
  f.pts_ms = 0;
  f.emb = e;
  f.generic = max_dot(e, clip.generic);
  if (!db_->mark_unavailable(item.asset.id, clip.meta.spec_key, std::span<const frame_in>(&f, 1), clip.meta.dim)) return;
  std::string active;
  {
    std::lock_guard lock(models_m_);
    active = answer_.meta.spec_key;
  }
  if (clip.meta.spec_key == active) {
    std::vector<std::int8_t> q;
    float scale = 1;
    quantise(e, q, scale);
    store_.add(item.asset.id, 0, f.generic, scale, q);
  }
  std::lock_guard lock(status_m_);
  units_done_ += 1;
  frames_done_ += 1;
}

void engine::process_video(const work_item& item, const loaded_clip& clip, bool faces_only) {
  bool faces_on = false;
  std::string face_spec;
  {
    std::lock_guard lock(models_m_);
    faces_on = faces_model_ != nullptr;
    if (faces_model_) face_spec = faces_model_->spec_key();
  }
  mv_addon_sampler_options o{};
  o.struct_size = sizeof(o);
  o.min_gap_ms = faces_only ? 2000 : 500;
  o.max_gap_ms = faces_only ? 10000 : 2000;
  o.max_long_edge = faces_on ? 768u : std::max<std::uint32_t>(448u, clip.meta.input_edge * 2);
  o.start_ms = faces_only ? 0 : item.resume_ms;
  // Nothing to scan for faces: said so, or the faces track would claim the
  // clip again on its next pass, forever (an unavailable clip is asked again
  // through forget_vectors once its original is here).
  const auto no_faces = [&] {
    std::lock_guard lock(models_m_);
    if (faces_ && faces_on) {
      (void)faces_->mark_scanned(item.asset.id, face_spec);
      faces_scanned_.insert(item.asset.id);
    }
  };
  auto file = file_of(item.asset.path);
  if (!file) {
    if (file.error() == status::permission_denied) {
      photos_readable_ = false;  // left pending: the next scan skips the library
      return;
    }
    if (file.error() == status::io && is_photos_key(item.asset.path)) {
      // An iCloud-only clip: its poster, which Photos keeps on this Mac, is
      // searchable until the original arrives (requeue_unavailable).
      if (!faces_only) index_poster(item, clip, o.max_long_edge);
      no_faces();
      return;
    }
    if (!faces_only) (void)db_->fail(item.asset.id, clip.meta.spec_key);
    no_faces();
    return;
  }
  auto sampler = host_.open_sampler(*file, o);
  if (!sampler) {
    if (!faces_only) (void)db_->fail(item.asset.id, clip.meta.spec_key);
    no_faces();
    return;
  }
  const mv_addon_video_info info = (*sampler)->info();
  if (info.duration_ms > 0) (void)db_->set_duration(item.asset.id, info.duration_ms);
  std::vector<float> last_kept;
  if (item.resume_ms > 0 && !faces_only) {
    // A resumed clip dedupes against what it already committed, not against
    // nothing: otherwise its first new frame repeats the last stored moment.
    // Only when the loaded vectors are this model's (not mid-migration).
    bool same_space = false;
    {
      std::lock_guard lock(models_m_);
      same_space = answer_.meta.spec_key == clip.meta.spec_key;
    }
    std::vector<float> prev;
    if (same_space && store_.vector_of(item.asset.id, item.resume_ms, prev) && prev.size() == clip.meta.dim) {
      last_kept = std::move(prev);
    }
  }
  std::vector<rgb_frame> batch;
  std::int64_t done_ms = item.resume_ms;
  bool ok = true;
  const auto flush = [&](work_state state) -> bool {
    std::vector<frame_in> keep;
    std::vector<float> embs;
    if (!batch.empty() && !faces_only) {
      std::vector<infer::rgb_view> views;
      for (const rgb_frame& f : batch) views.push_back(infer::rgb_view{f.rgb.data(), f.width, f.height});
      if (!clip.model->embed_images(views, embs)) return false;
    }
    std::vector<std::size_t> kept_idx;
    for (std::size_t i = 0; i < batch.size(); ++i) {
      if (faces_only) {
        kept_idx.push_back(i);
        continue;
      }
      const std::span<const float> e(embs.data() + i * clip.meta.dim, clip.meta.dim);
      // plan/17 step 3: a static shot collapses to a few rows.
      if (!last_kept.empty() && infer::dot(e, last_kept) >= clip.meta.dedupe) continue;
      last_kept.assign(e.begin(), e.end());
      frame_in f;
      f.pts_ms = batch[i].pts_ms;
      f.pts_tb = batch[i].pts_tb;
      f.tb_num = batch[i].tb_num;
      f.tb_den = batch[i].tb_den;
      f.flags = batch[i].flags;
      f.emb = e;
      f.generic = max_dot(e, clip.generic);
      keep.push_back(f);
      kept_idx.push_back(i);
    }
    // The tile a result shows: the moment in the viewer's JPEG-512 cache.
    if (!faces_only) {
      for (std::size_t i : kept_idx) (void)host_.moment_thumbnail(*file, batch[i].pts_ms, &batch[i]);
    }
    if (faces_on) {
      for (std::size_t i : kept_idx) faces_of(item.asset.id, item.asset.path, batch[i].pts_ms, batch[i]);
    }
    const std::int64_t prev = done_ms;
    if (!batch.empty()) done_ms = batch.back().pts_ms + 1;
    if (!faces_only) {
      if (!db_->commit_frames(item.asset.id, clip.meta.spec_key, keep, state, done_ms, clip.meta.dim)) return false;
      std::string active;
      {
        std::lock_guard lock(models_m_);
        active = answer_.meta.spec_key;
      }
      if (clip.meta.spec_key == active) {
        for (const frame_in& f : keep) {
          std::vector<std::int8_t> q;
          float scale = 1;
          quantise(f.emb, q, scale);
          store_.add(item.asset.id, f.pts_ms, f.generic, scale, q);
        }
      }
    }
    {
      std::lock_guard lock(status_m_);
      units_done_ += static_cast<double>(std::max<std::int64_t>(0, done_ms - prev)) / 1000.0;
      frames_done_ += keep.size();
    }
    batch.clear();
    return true;
  };
  while (true) {
    if (!wait_turn()) {
      // Shutting down mid-clip: what is embedded commits; the rest resumes.
      if (!faces_only) ok = flush(work_state::partial);
      return;
    }
    auto f = (*sampler)->next();
    if (!f) {
      ok = false;
      break;
    }
    if (f->flags & MV_ADDON_FRAME_END) break;
    batch.push_back(std::move(*f));
    if (batch.size() >= kFrameBatch && !flush(work_state::partial)) {
      ok = false;
      break;
    }
  }
  if (ok) ok = flush(work_state::done);
  if (!ok && !faces_only) (void)db_->fail(item.asset.id, clip.meta.spec_key);
  if (faces_on) {
    std::lock_guard lock(models_m_);
    if (faces_) {
      (void)faces_->mark_scanned(item.asset.id, face_spec);
      faces_scanned_.insert(item.asset.id);
    }
  }
}

bool engine::wait_idle(int ms) {
  const auto until = clock::now() + std::chrono::milliseconds(ms);
  while (clock::now() < until) {
    bool idle = false;
    {
      std::lock_guard lock(work_m_);
      // The reload flag before loading_: the control thread sets loading_
      // before it clears the flag, so one of the two reads true.
      const bool reloading = reload_models_.load() || loading_.load();
      if (models_ready_ && !reloading && queue_exhausted_ && busy_workers_ == 0 && in_flight_.empty()) {
        std::lock_guard cl(control_m_);
        idle = !rescan_all_ && rescan_roots_.empty() && !scanning_;
      }
    }
    if (idle) {
      // The status line refreshes once a second; a caller that waited for
      // the work wants the counts that describe it.
      refresh_counts();
      return true;
    }
    if (models_failed_) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

std::string engine::active_spec() const {
  std::lock_guard lock(models_m_);
  return answer_.meta.spec_key;
}

// ---- roots ------------------------------------------------------------------------------------

std::string engine::roots_json() {
  const std::vector<root_row> roots = db_->roots();
  std::string spec;
  {
    std::lock_guard lock(models_m_);
    spec = build_.meta.spec_key;
  }
  json::writer w;
  w.begin_array();
  for (const root_row& r : roots) {
    w.begin_object();
    w.key("id").integer(r.id);
    w.key("path").string(r.path);
    const bool photos = is_photos_key(r.path);
    w.key("kind").string(photos ? "photos" : "folder");
    if (photos) {
      w.key("access").string(access_name(photos_library_access()));
      w.key("unavailable").integer(static_cast<std::int64_t>(spec.empty() ? 0 : db_->unavailable_in_root(r.id, spec)));
    }
    w.key("recursive").boolean(r.recursive);
    w.key("enabled").boolean(r.enabled);
    w.key("media").integer(r.media);
    w.key("assets").integer(static_cast<std::int64_t>(db_->assets_in_root(r.id)));
    w.key("done").integer(static_cast<std::int64_t>(spec.empty() ? 0 : db_->done_in_root(r.id, spec)));
    w.key("frames").integer(static_cast<std::int64_t>(spec.empty() ? 0 : db_->frames_in_root(r.id, spec)));
    w.key("last_scan").integer(r.last_scan_at);
    w.end_object();
  }
  w.end_array();
  return w.take();
}

result<std::int64_t> engine::index_folder(const std::string& dir, bool recursive) {
  if (dir.empty() || is_photos_key(dir)) return err(status::invalid_arg);
  if (importing_elsewhere()) return err(status::busy);
  const std::string key = path_key(dir);
  const std::vector<root_row> roots = db_->roots();
  for (const root_row& r : roots) {
    const std::string rk = path_key(r.path);
    if ((rk == key && (r.recursive || !recursive)) || (r.recursive && under(key, rk))) {
      if (!r.enabled) (void)db_->set_root_enabled(r.id, true);
      return r.id;
    }
  }
  MV_TRY(std::int64_t id, db_->add_root(dir, recursive));
  if (recursive) {
    // A tree root swallows the roots beneath it.
    for (const root_row& r : roots) {
      if (r.id != id && under(path_key(r.path), key)) (void)db_->merge_root(r.id, id);
    }
    std::lock_guard lock(assets_m_);
    for (auto& [aid, m] : assets_) {
      if (under(m.key, key)) m.root = id;
    }
  }
  {
    std::lock_guard lock(control_m_);
    rescan_roots_.insert(id);
  }
  control_cv_.notify_all();
  post(MV_ADDON_EVENT_AI_ROOTS, static_cast<std::uint64_t>(id));
  return id;
}

photos_access engine::photos_library_access() const {
  return photos_ ? photos_->access() : photos_access::unsupported;
}

result<std::int64_t> engine::index_photos_library() {
  const photos_access access = photos_library_access();
  if (access == photos_access::unsupported) return err(status::unsupported_format);
  if (!readable(access)) return err(status::permission_denied);
  if (importing_elsewhere()) return err(status::busy);  // as index_folder: not while an import runs
  MV_TRY(std::int64_t id, db_->add_root(std::string(kPhotosRoot), true));
  photos_root_ = id;
  {
    std::lock_guard lock(control_m_);
    rescan_roots_.insert(id);
  }
  control_cv_.notify_all();
  post(MV_ADDON_EVENT_AI_ROOTS, static_cast<std::uint64_t>(id));
  return id;
}

expected engine::root_set_enabled(std::int64_t id, bool enabled) {
  if (importing_elsewhere()) return err(status::busy);
  MV_TRY_VOID(db_->set_root_enabled(id, enabled));
  {
    std::lock_guard lock(work_m_);
    queue_.clear();
    queue_exhausted_ = false;
  }
  if (enabled) {
    std::lock_guard lock(control_m_);
    rescan_roots_.insert(id);
  }
  control_cv_.notify_all();
  work_cv_.notify_all();
  post(MV_ADDON_EVENT_AI_ROOTS, static_cast<std::uint64_t>(id));
  return {};
}

expected engine::root_set_media(std::int64_t id, std::uint32_t media) {
  if (importing_elsewhere()) return err(status::busy);
  if (media > MV_AI_MEDIA_BOTH) return err(status::invalid_arg);
  MV_TRY_VOID(db_->set_root_media(id, media));
  {
    std::lock_guard lock(work_m_);
    queue_.clear();
    queue_exhausted_ = false;
  }
  work_cv_.notify_all();
  refresh_counts();
  post(MV_ADDON_EVENT_AI_ROOTS, static_cast<std::uint64_t>(id));
  return {};
}

expected engine::root_rescan(std::int64_t id) {
  {
    std::lock_guard lock(control_m_);
    rescan_roots_.insert(id);
  }
  control_cv_.notify_all();
  return {};
}

expected engine::root_remove(std::int64_t id) {
  if (importing_elsewhere()) return err(status::busy);
  std::vector<std::int64_t> gone;
  {
    std::lock_guard lock(assets_m_);
    for (auto it = assets_.begin(); it != assets_.end();) {
      if (it->second.root == id) {
        gone.push_back(it->first);
        it = assets_.erase(it);
      } else {
        ++it;
      }
    }
  }
  {
    std::lock_guard lock(work_m_);
    queue_.clear();
    queue_exhausted_ = false;
  }
  MV_TRY_VOID(db_->remove_root(id));
  if (photos_root_ == id) {
    photos_root_ = 0;
    photos_readable_ = false;
    if (photos_) photos_->observe(nullptr);
  }
  for (std::int64_t a : gone) {
    store_.remove_asset(a);
    sounds_.remove_asset(a);
  }
  {
    const std::set<std::int64_t> drop(gone.begin(), gone.end());
    std::lock_guard lock(speech_m_);
    speech_rows_.erase(std::remove_if(speech_rows_.begin(), speech_rows_.end(),
                                      [&](const speech_row& r) { return drop.count(r.asset) != 0; }),
                       speech_rows_.end());
  }
  {
    std::lock_guard lock(models_m_);
    if (faces_) {
      for (std::int64_t a : gone) {
        (void)faces_->forget_asset(a);
        faces_scanned_.erase(a);
      }
    }
  }
  refresh_counts();
  post(MV_ADDON_EVENT_AI_ROOTS, static_cast<std::uint64_t>(id));
  post(MV_ADDON_EVENT_AI_PEOPLE);
  return {};
}

std::uint32_t engine::folder_coverage(const std::string& dir) const {
  const std::string key = path_key(dir);
  std::lock_guard lock(status_m_);
  for (const root_row& r : roots_cache_) {
    const std::string rk = path_key(r.path);
    if (rk == key || (r.recursive && under(key, rk))) {
      auto it = root_counts_.find(r.id);
      const bool complete = it != root_counts_.end() && it->second.first > 0 &&
                            it->second.second >= it->second.first && r.last_scan_at != 0;
      return complete ? 2u : 1u;
    }
  }
  return 0;
}

void engine::note_folder_opened(const std::string& dir) {
  const std::string key = path_key(dir);
  std::int64_t root = 0;
  {
    std::lock_guard lock(status_m_);
    for (const root_row& r : roots_cache_) {
      const std::string rk = path_key(r.path);
      if (rk == key || (r.recursive && under(key, rk))) root = r.id;
    }
  }
  {
    std::lock_guard lock(work_m_);
    prefer_dir_key_ = key;
    queue_.clear();  // re-ordered on the next claim
    queue_exhausted_ = false;
  }
  if (root != 0) {
    std::lock_guard lock(control_m_);
    rescan_roots_.insert(root);
  }
  control_cv_.notify_all();
}

expected engine::clear_index() {
  if (importing_elsewhere()) return err(status::busy);
  clearing_ = true;
  // Let in-flight work land (or fail) before the rows go.
  for (int i = 0; i < 300; ++i) {
    {
      std::lock_guard lock(work_m_);
      if (busy_workers_ == 0) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  expected r = db_->clear();
  photos_root_ = 0;  // the roots went too
  photos_readable_ = false;
  if (photos_) photos_->observe(nullptr);
  {
    std::lock_guard lock(assets_m_);
    assets_.clear();
  }
  store_.clear();
  sounds_.reset(sounds_.dim());
  {
    std::lock_guard lock(speech_m_);
    speech_rows_.clear();
  }
  {
    std::lock_guard lock(work_m_);
    queue_.clear();
    in_flight_.clear();
    queue_exhausted_ = false;
  }
  {
    // The people index hangs off the assets that just went.
    std::lock_guard lock(models_m_);
    if (faces_) {
      faces_.reset();
      faces_db::destroy(join(data_dir_, "faces.db"));
      if (faces_model_) {
        if (auto db = faces_db::open(join(data_dir_, "faces.db"), faces_model_->tuning(), faces_model_->dim(),
                                     faces_model_->spec_key())) {
          faces_ = std::move(*db);
        }
      }
      faces_scanned_.clear();
    }
  }
  index_full_ = false;
  clearing_ = false;
  refresh_counts();
  post(MV_ADDON_EVENT_AI_ROOTS);
  post(MV_ADDON_EVENT_AI_PEOPLE);
  return r;
}

// ---- sharing an index (plan/17 "Sharing an index") ---------------------------------------

namespace {

std::string machine_label() {
#if defined(_WIN32)
  std::string os = "Windows";
#elif defined(__APPLE__)
  std::string os = "macOS";
#else
  std::string os = "Linux";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
  return os + " arm64";
#else
  return os + " x64";
#endif
}

}  // namespace

result<std::uint64_t> engine::export_index(const std::string& dest, std::vector<std::int64_t> roots,
                                           std::uint32_t flags) {
  if (dest.empty()) return err(status::invalid_arg);
  std::lock_guard lock(transfer_m_);
  if (!transfer_queue_.empty() || (transfer_.running && !transfer_.done)) return err(status::busy);
  transfer_job job;
  job.id = next_transfer_++;
  job.file = dest;
  job.roots = std::move(roots);
  job.flags = flags & (MV_AI_TRANSFER_PEOPLE | MV_AI_TRANSFER_THUMBS);
  transfer_ = transfer_view{};
  transfer_.id = job.id;
  transfer_.running = true;
  transfer_queue_.push_back(std::move(job));
  control_cv_.notify_all();
  return transfer_.id;
}

result<std::uint64_t> engine::import_index(const std::string& file, const std::string& map_json,
                                           std::uint32_t flags) {
  if (file.empty()) return err(status::invalid_arg);
  const auto doc = json::parse(map_json, 4);
  if (!doc || doc->k != json::kind::array) return err(status::invalid_arg);
  transfer_job job;
  for (const json::value& v : doc->a) {
    const auto id = v.integer("id");
    const std::string* path = v.str("path");
    if (!id || !path || path->empty()) return err(status::invalid_arg);
    job.map.emplace_back(*id, *path);
  }
  if (job.map.empty()) return err(status::invalid_arg);
  std::lock_guard lock(transfer_m_);
  if (!transfer_queue_.empty() || (transfer_.running && !transfer_.done)) return err(status::busy);
  job.id = next_transfer_++;
  job.import = true;
  job.file = file;
  job.flags = flags & (MV_AI_TRANSFER_PEOPLE | MV_AI_TRANSFER_THUMBS);
  transfer_ = transfer_view{};
  transfer_.id = job.id;
  transfer_.import = true;
  transfer_.running = true;
  transfer_queue_.push_back(std::move(job));
  control_cv_.notify_all();
  return transfer_.id;
}

std::string engine::transfer_json() const {
  std::lock_guard lock(transfer_m_);
  json::writer w;
  w.begin_object();
  w.key("id").integer(static_cast<std::int64_t>(transfer_.id));
  w.key("kind").string(transfer_.import ? "import" : "export");
  w.key("running").boolean(transfer_.running && !transfer_.done);
  w.key("done").boolean(transfer_.done);
  w.key("status").integer(transfer_.status);
  w.key("fraction").number(transfer_.fraction);
  w.key("outcome");
  if (transfer_.outcome.empty()) {
    w.null();
  } else {
    w.raw(transfer_.outcome);
  }
  w.end_object();
  return w.take();
}

void engine::transfer_cancel() noexcept { transfer_cancel_ = true; }

void engine::set_transfer_progress(double f) {
  {
    std::lock_guard lock(transfer_m_);
    // At most every 2 %: the chrome polls status on the event.
    if (f < 1.0 && f - transfer_.fraction < 0.02) return;
    transfer_.fraction = std::clamp(f, 0.0, 1.0);
  }
  post(MV_ADDON_EVENT_AI_STATUS);
}

engine::spec_plan engine::plan_specs(const std::string& file_picture) {
  spec_plan p;
  const std::string active = db_->meta("active_spec");
  const bool fresh = active.empty() || db_->count(active).frames == 0;
  std::string build, sound, speech;
  {
    std::lock_guard lock(models_m_);
    build = build_.meta.spec_key;
    sound = sound_.spec_key;
    speech = speech_.spec_key;
  }
  if (!fresh) p.specs.insert(active);
  if (!build.empty()) p.specs.insert(build);
  if (!sound.empty()) p.specs.insert(sound);
  if (!speech.empty()) p.specs.insert(speech);
  if (file_picture.empty()) return p;
  if (p.specs.count(file_picture)) {
    p.picture = file_picture;
    return p;
  }
  // An empty index here takes the file's tower when the pack carries it: the
  // point of importing is not to embed the library again.
  if (fresh && deps_.qualities && deps_.clip_spec) {
    for (std::uint32_t q : deps_.qualities()) {
      if (deps_.clip_spec(q) == file_picture) {
        p.adopt = q;
        p.picture = file_picture;
        p.specs.insert(file_picture);
        return p;
      }
    }
  }
  // Still loading (a Core ML first compile takes minutes): nothing is known
  // yet, and the import itself runs after the load, where it is decided again.
  p.why_not = !build.empty() ? "model" : models_failed_ ? "no_models" : "loading";
  return p;
}

result<std::string> engine::inspect_export(const std::string& file) {
  MV_TRY(transfer::file_info f, transfer::inspect(file));
  const spec_plan plan = plan_specs(f.picture_spec);
  std::string model;
  if (deps_.qualities && deps_.clip_spec && deps_.model_name) {
    for (std::uint32_t q : deps_.qualities()) {
      if (deps_.clip_spec(q) == f.picture_spec) model = deps_.model_name(q);
    }
  }
  bool people_ready = false, people_match = false;
  {
    std::lock_guard lock(models_m_);
    people_ready = faces_ && faces_model_;
    people_match = people_ready && faces_model_->spec_key() == f.face_spec;
  }
  json::writer w;
  w.begin_object();
  w.key("version").integer(f.version);
  w.key("created").integer(f.created);
  w.key("from").string(f.from);
  w.key("model").string(model);
  w.key("picture_usable").boolean(!plan.picture.empty());
  w.key("adopt_quality").integer(plan.adopt);
  w.key("why_not").string(plan.why_not);
  w.key("people");
  if (f.faces) {
    w.begin_object();
    w.key("faces").integer(static_cast<std::int64_t>(f.face_count));
    w.key("people").integer(static_cast<std::int64_t>(f.people));
    w.key("ready").boolean(people_ready);
    w.key("match").boolean(people_match);
    w.end_object();
  } else {
    w.null();
  }
  w.key("thumbs").integer(static_cast<std::int64_t>(f.thumbs ? f.thumb_count : 0));
  w.key("roots").begin_array();
  for (const transfer::file_root& r : f.roots) {
    w.begin_object();
    w.key("id").integer(r.id);
    w.key("name").string(r.name);
    w.key("path").string(r.path);
    w.key("recursive").boolean(r.recursive);
    w.key("assets").integer(static_cast<std::int64_t>(r.assets));
    bool exists = false;
    if (!r.path.empty()) {
      const auto st = host_.stat(r.path);
      exists = st && st->is_directory;
    }
    w.key("exists").boolean(exists);
    w.end_object();
  }
  w.end_array();
  w.end_object();
  return w.take();
}

void engine::run_transfer(transfer_job job) {
  transfer_cancel_ = false;
  mv::status st = mv::status::ok;
  std::string outcome = job.import ? run_import(job, st) : run_export(job, st);
  {
    std::lock_guard lock(transfer_m_);
    if (transfer_.id == job.id) {
      transfer_.done = true;
      transfer_.running = false;
      transfer_.status = static_cast<std::int32_t>(st);
      transfer_.fraction = st == status::ok ? 1.0 : transfer_.fraction;
      transfer_.outcome = std::move(outcome);
    }
  }
  if (job.import) {
    post(MV_ADDON_EVENT_AI_ROOTS);
    post(MV_ADDON_EVENT_AI_PEOPLE);
  }
  post(MV_ADDON_EVENT_AI_STATUS);
}

std::string engine::run_export(const transfer_job& job, mv::status& st) {
  transfer::export_options o;
  o.index_db = db_->path();
  o.faces_db = join(data_dir_, "faces.db");
  o.roots = job.roots;
  o.faces = (job.flags & MV_AI_TRANSFER_PEOPLE) != 0;
  o.thumbs = (job.flags & MV_AI_TRANSFER_THUMBS) != 0;
  o.from = machine_label();
  {
    std::lock_guard lock(models_m_);
    o.picture_spec = answer_.meta.spec_key;
    // People's vectors only travel with the model that made them.
    if (faces_ && faces_model_) o.face_spec = faces_model_->spec_key();
  }
  if (o.picture_spec.empty()) o.picture_spec = db_->meta("active_spec");
  if (o.face_spec.empty()) o.faces = false;
  if (o.thumbs && host_.has_thumbnail_bytes()) {
    o.thumb = [this](const std::string& path, std::int64_t pts_ms) { return host_.thumbnail_jpeg(path, pts_ms); };
  }
  transfer::control c{&transfer_cancel_, [this](double f) { set_transfer_progress(f); }};
  auto r = transfer::write(job.file, o, c);
  if (!r) {
    st = r.error();
    return {};
  }
  json::writer w;
  w.begin_object();
  w.key("roots").integer(static_cast<std::int64_t>(r->roots));
  w.key("assets").integer(static_cast<std::int64_t>(r->assets));
  w.key("frames").integer(static_cast<std::int64_t>(r->frames));
  w.key("speech").integer(static_cast<std::int64_t>(r->speech));
  w.key("faces").integer(static_cast<std::int64_t>(r->faces));
  w.key("people").integer(static_cast<std::int64_t>(r->people));
  w.key("people_included").boolean(o.faces);
  w.key("thumbs").integer(static_cast<std::int64_t>(r->thumbs));
  w.key("thumbs_missing").integer(static_cast<std::int64_t>(r->thumbs_missing));
  w.key("bytes").integer(static_cast<std::int64_t>(r->bytes));
  w.end_object();
  return w.take();
}

std::string engine::run_import(const transfer_job& job, mv::status& st) {
  auto info = transfer::inspect(job.file);
  if (!info) {
    st = info.error();
    return {};
  }
  // Indexing waits, as for Clear: in-flight work lands first, and nothing
  // claims an asset while its rows change underneath.
  transferring_ = true;
  for (int i = 0; i < 300; ++i) {
    {
      std::lock_guard lock(work_m_);
      if (busy_workers_ == 0) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  const spec_plan plan = plan_specs(info->picture_spec);
  std::vector<transfer::root_target> targets;
  for (const auto& [file_root, dir] : job.map) {
    const auto r = std::find_if(info->roots.begin(), info->roots.end(),
                                [&](const transfer::file_root& x) { return x.id == file_root; });
    if (r == info->roots.end()) continue;
    // The folder here joins (or is) a remembered root, like "Index this folder".
    auto local = index_folder(dir, r->recursive);
    if (!local) continue;
    if (r->media != 0) (void)db_->set_root_media(*local, r->media);
    targets.push_back(transfer::root_target{file_root, dir, *local});
  }
  if (targets.empty()) {
    transferring_ = false;
    work_cv_.notify_all();
    st = status::invalid_arg;
    return {};
  }
  transfer::control c{&transfer_cancel_, [this](double f) { set_transfer_progress(f); }};
  std::vector<transfer::imported_asset> assets;
  transfer::import_counts n;
  expected r = db_->with_connection([&](sqlite3* db) {
    return transfer::import_index(db, job.file, targets, plan.specs, assets, n, c);
  });
  std::string people_why;
  bool adopted = false;
  if (r) {
    if (plan.adopt != 0 && n.frames > 0) {
      // This index was empty: it takes the file's Quality, and its vectors
      // answer from now on (no migration, nothing embedded again).
      {
        std::lock_guard lock(settings_m_);
        settings_.quality = plan.adopt;
      }
      save_settings();
      (void)db_->set_meta("active_spec", plan.picture);
      // Workers wait for the new tower (the next loop's load): the old one
      // would embed the imported assets again under its own spec.
      loading_ = true;
      reload_models_ = true;
      adopted = true;
    } else if (!plan.picture.empty() && n.frames > 0) {
      const std::string active = db_->meta("active_spec");
      std::string answering;
      {
        std::lock_guard lock(models_m_);
        answering = answer_.meta.spec_key;
      }
      // An empty index whose tower is the file's: the file's vectors are it.
      if (active.empty() || (active != plan.picture && db_->count(active).frames == 0)) {
        (void)db_->set_meta("active_spec", plan.picture);
        if (answering != plan.picture) {
          loading_ = true;
          reload_models_ = true;
        }
      }
    }
    if ((job.flags & MV_AI_TRANSFER_PEOPLE) && info->faces) {
      bool on;
      {
        std::lock_guard lock(settings_m_);
        on = settings_.faces;
      }
      bool open;
      {
        std::lock_guard lock(models_m_);
        open = faces_ && faces_model_;
      }
      if (!open) {
        // Importing People turns People on (the checkbox said so).
        settings s;
        {
          std::lock_guard lock(settings_m_);
          settings_.faces = true;
          s = settings_;
        }
        if (!on) save_settings();
        faces_parts people = open_faces_parts(s);
        std::lock_guard lock(models_m_);
        if (people.model) {
          faces_model_ = std::move(people.model);
          faces_ = std::move(people.db);
          faces_scanned_ = std::move(people.scanned);
          publish_pieces_locked();
        }
      }
      std::lock_guard lock(models_m_);
      if (!faces_ || !faces_model_) {
        people_why = "no_piece";
      } else if (faces_model_->spec_key() != info->face_spec) {
        people_why = "model";
      } else {
        const std::string spec = faces_model_->spec_key();
        (void)faces_->import_with([&](sqlite3* fdb) {
          return transfer::import_faces(fdb, job.file, spec, assets, n, c);
        });
        faces_scanned_.clear();
        for (std::int64_t a : faces_->scanned_assets(spec)) faces_scanned_.insert(a);
      }
    }
    if ((job.flags & MV_AI_TRANSFER_THUMBS) && info->thumbs && host_.has_thumbnail_bytes()) {
      transfer::thumb_io io;
      io.stat = [this](const std::string& path, std::int64_t& mtime, std::uint64_t& size) {
        auto s = host_.stat(path);
        if (!s || s->is_directory) return false;
        mtime = s->mtime;
        size = s->size;
        return true;
      };
      io.store = [this](const std::string& path, std::int64_t pts_ms, std::span<const std::uint8_t> jpeg) {
        return host_.store_thumbnail_jpeg(path, pts_ms, jpeg);
      };
      io.yield = [this] {
        for (int i = 0; i < 500 && !stopping_ && host_.should_yield(); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
      };
      (void)transfer::import_thumbs(job.file, assets, io, n, c);
    }
  } else {
    st = r.error();
  }
  // Whatever committed, every in-memory view comes from the rows again, and
  // the folders are rescanned: a file that differs here is indexed again.
  reload_from_db();
  transferring_ = false;
  work_cv_.notify_all();
  {
    std::lock_guard lock(control_m_);
    for (const transfer::root_target& t : targets) rescan_roots_.insert(t.local_root);
  }
  if (!r) return {};
  json::writer w;
  w.begin_object();
  w.key("assets").integer(static_cast<std::int64_t>(n.assets));
  w.key("added").integer(static_cast<std::int64_t>(n.added));
  w.key("replaced").integer(static_cast<std::int64_t>(n.replaced));
  w.key("kept").integer(static_cast<std::int64_t>(n.kept));
  w.key("frames").integer(static_cast<std::int64_t>(n.frames));
  w.key("speech").integer(static_cast<std::int64_t>(n.speech));
  w.key("skipped_rows").integer(static_cast<std::int64_t>(n.skipped_rows));
  w.key("picture_usable").boolean(!plan.picture.empty());
  w.key("why_not").string(plan.why_not);
  w.key("adopted_quality").integer(adopted ? plan.adopt : 0);
  w.key("faces").integer(static_cast<std::int64_t>(n.faces));
  w.key("people_new").integer(static_cast<std::int64_t>(n.people_new));
  w.key("people_joined").integer(static_cast<std::int64_t>(n.people_joined));
  w.key("people_why").string(people_why);
  w.key("thumbs").integer(static_cast<std::int64_t>(n.thumbs));
  w.key("thumbs_skipped").integer(static_cast<std::int64_t>(n.thumbs_skipped));
  w.end_object();
  return w.take();
}

void engine::reload_from_db() {
  {
    std::lock_guard lock(assets_m_);
    assets_.clear();
    for (asset_row& a : db_->all_assets()) {
      asset_meta m;
      m.key = path_key(a.path);
      m.dir_key = key_parent(m.key);
      m.path = std::move(a.path);
      m.kind = a.kind;
      m.root = a.root_id;
      m.mtime = a.mtime;
      assets_.emplace(a.id, std::move(m));
    }
  }
  std::string active, sound_spec, speech_spec;
  std::uint32_t dim = 0, sound_dim = 0;
  {
    std::lock_guard lock(models_m_);
    active = answer_.meta.spec_key;
    dim = answer_.meta.dim;
    if (sound_.model) {
      sound_spec = sound_.spec_key;
      sound_dim = sound_.dim;
    }
    if (speech_.model) speech_spec = speech_.spec_key;
  }
  if (!active.empty()) {
    begin_answer_swap();
    load_vectors(active, dim);
    end_answer_swap();
    load_labels();  // the reset dropped them; labels.f32 has this tower's
  }
  if (!sound_spec.empty()) {
    sounds_.reset(sound_dim);
    (void)db_->each_frame(sound_spec, [&](const frame_out& f) {
      sounds_.add(f.asset_id, f.pts_ms, f.generic, f.scale, f.emb);
    });
  }
  if (!speech_spec.empty()) load_speech(speech_spec);
  {
    std::lock_guard lock(work_m_);
    queue_.clear();
    queue_exhausted_ = false;
  }
  refresh_counts();
}

// ---- search ---------------------------------------------------------------------------------------

void engine::search_loop() {
  while (true) {
    search_job job;
    {
      std::unique_lock lock(search_m_);
      search_cv_.wait(lock, [this] { return stopping_.load() || !search_queue_.empty(); });
      if (stopping_) return;
      job = std::move(search_queue_.front());
      search_queue_.pop_front();
    }
    auto st = std::make_shared<search_state>();
    try {
      job.run(*st);
    } catch (...) {
      st->rows.clear();
    }
    st->done = true;
    std::size_t n = st->rows.size();
    {
      std::lock_guard lock(search_m_);
      searches_[job.id] = std::move(st);
      // Keep the last few: a chrome holds one or two at a time.
      while (searches_.size() > 24) searches_.erase(searches_.begin());
    }
    search_cv_.notify_all();
    post(MV_ADDON_EVENT_AI_SEARCH_DONE, job.id, static_cast<std::int64_t>(n));
  }
}

std::uint64_t engine::submit(std::function<void(search_state&)> run) {
  std::lock_guard lock(search_m_);
  const std::uint64_t id = next_search_++;
  search_queue_.push_back(search_job{id, std::move(run)});
  search_cv_.notify_all();
  return id;
}

bool engine::wait_search(std::uint64_t id, int ms) {
  std::unique_lock lock(search_m_);
  return search_cv_.wait_for(lock, std::chrono::milliseconds(ms), [&] { return searches_.count(id) != 0; });
}

std::shared_ptr<std::set<std::int64_t>> engine::scope_assets(const std::string& scope_dir, std::uint32_t scope,
                                                             std::uint32_t kinds, std::int64_t from_unix,
                                                             std::int64_t to_unix) const {
  const std::string key = scope_dir.empty() ? std::string() : path_key(scope_dir);
  if ((kinds & MV_AI_KIND_ALL) == 0) kinds |= MV_AI_KIND_ALL;
  // A snapshot of the qualifying assets, so the scan never takes assets_m_.
  auto allowed = std::make_shared<std::set<std::int64_t>>();
  {
    std::lock_guard lock(assets_m_);
    for (const auto& [id, m] : assets_) {
      const std::uint32_t k = m.kind == asset_kind::video ? MV_AI_KIND_VIDEOS : MV_AI_KIND_PHOTOS;
      if (!(k & kinds)) continue;
      if (m.mtime < from_unix || m.mtime >= to_unix) continue;
      if (scope != MV_AI_SCOPE_ALL && !key.empty()) {
        if (scope == MV_AI_SCOPE_FOLDER && m.dir_key != key) continue;
        if (scope == MV_AI_SCOPE_TREE && m.dir_key != key && !under(m.dir_key, key)) continue;
      }
      allowed->insert(id);
    }
  }
  return allowed;
}

std::function<bool(std::int64_t)> engine::scope_filter(const std::string& scope_dir, std::uint32_t scope,
                                                       std::uint32_t kinds) const {
  auto allowed = scope_assets(scope_dir, scope, kinds);
  return [allowed](std::int64_t a) { return allowed->count(a) != 0; };
}

void engine::add_row(search_state& st, std::int64_t asset, std::int64_t pts_ms, float score, float rank,
                     std::uint32_t match, std::string snippet) const {
  st.moments[asset].push_back({pts_ms, score});
  if (auto it = st.row_of.find(asset); it != st.row_of.end()) {
    result_row& r = st.rows[it->second];
    ++r.more;
    r.match |= match;
    if (rank > r.rank) {  // a better moment of the same clip leads
      r.pts_ms = pts_ms;
      r.score = score;
      r.rank = rank;
      if (!snippet.empty()) r.snippet = std::move(snippet);
    } else if (r.snippet.empty() && !snippet.empty()) {
      r.snippet = std::move(snippet);
    }
    return;
  }
  if (st.rows.size() >= 1000) return;
  result_row r;
  r.asset = asset;
  r.pts_ms = pts_ms;
  r.score = score;
  r.rank = rank;
  r.match = match;
  r.snippet = std::move(snippet);
  {
    std::lock_guard lock(assets_m_);
    auto found = assets_.find(asset);
    if (found == assets_.end()) return;
    r.path = found->second.path;
    r.kind = found->second.kind == asset_kind::video ? MV_AI_KIND_VIDEOS : MV_AI_KIND_PHOTOS;
  }
  st.row_of[asset] = st.rows.size();
  st.rows.push_back(std::move(r));
}

void engine::group(search_state& st, const std::vector<vector_store::hit>& hits, bool text,
                   float query_margin, std::uint32_t match, bool stands_out) const {
  if (hits.empty()) return;
  if (text) {
    // "Nothing found" (PR 20 calibration): a query that no top-ten row beats
    // the generic prompts by `query_margin` describes nothing in the index,
    // unless its best assets stand out from the rest (`stands_out`): a
    // one-word subject ("dog") sits close to "a photo." and misses the margin
    // while ranking correctly (2026-09-27, plan/17).
    float best = -1;
    for (std::size_t i = 0; i < hits.size() && i < 10; ++i) {
      best = std::max(best, hits[i].score - hits[i].generic);
    }
    if (best < query_margin && !stands_out) return;
  }
  for (const vector_store::hit& h : hits) {
    // Across models the cosines are not comparable; the margin over each
    // model's own generic prompts is (in units of the 0.1 a strong match gets).
    const float rank = text ? (h.score - h.generic) / 0.1f : h.score;
    add_row(st, h.asset, h.pts_ms, h.score, rank, match, {});
  }
}

void engine::finish(search_state& st) {
  std::stable_sort(st.rows.begin(), st.rows.end(),
                   [](const result_row& a, const result_row& b) { return a.rank > b.rank; });
  st.row_of.clear();
  for (auto& [asset, list] : st.moments) std::sort(list.begin(), list.end());
}

void engine::load_speech(const std::string& spec) {
  std::vector<speech_row> rows;
  (void)db_->each_speech(spec, [&](std::int64_t asset, std::int64_t start, std::int64_t, const std::string& text) {
    rows.push_back(speech_row{asset, start, text, infer::speech_words(text)});
  });
  std::lock_guard lock(speech_m_);
  speech_rows_ = std::move(rows);
}

namespace {

// Words a spoken-phrase search ignores ("the", "someone saying ...").
bool stop_word(const std::string& w) {
  static const char* const kStop[] = {"a", "an", "the", "of", "on", "in", "at", "to", "is", "are", "and",
                                      "or", "with", "someone", "somebody", "says", "said", "saying", "say",
                                      "who", "where", "when", "that", "this", "it", "was", "be", "for",
                                      "clip", "video", "videos", "moment", "photos", "photo", "about"};
  for (const char* s : kStop) {
    if (w == s) return true;
  }
  return false;
}

}  // namespace

void engine::merge_audio(search_state& st, const std::string& query, const std::function<bool(std::int64_t)>& allow,
                         std::uint32_t find, std::uint32_t precision) {
  loaded_sound sound;
  {
    std::lock_guard lock(models_m_);
    sound = sound_;
  }
  // What it sounds like: CLAP, with its own generic prompts and margins.
  if ((find & MV_AI_FIND_SOUNDS) && sound.model) {
    if (auto v = sound.model->embed_text(query)) {
      // The margin rule alone (query_z 0: CLAP has no z calibration yet), its
      // margins scaled by Precision as the picture tower's are.
      text_thresholds t;
      t.query_margin = sound.query_margin;
      t.result_margin = sound.result_margin;
      t.query_z = 0;
      t.result_z = 0;
      const auto hits = find_text(sounds_, *v, allow, t, precision_scale::at(precision), true, 2000);
      group(st, hits, true, -1.0f, MV_AI_MATCH_SOUND, true);
    }
  }
  // What is said: the transcript's words. A result must hold most of the
  // query's words (a few-word phrase: all of them); a word of four letters or
  // more also matches its longer forms ("land" -> "landing"). Precision moves
  // the share (speech_coverage_needed).
  if (!(find & MV_AI_FIND_SPEECH)) return;
  std::vector<std::string> want;
  for (std::string& w : infer::speech_words(query)) {
    if (!stop_word(w) && std::find(want.begin(), want.end(), w) == want.end()) want.push_back(std::move(w));
  }
  if (want.empty()) return;
  std::string phrase;
  for (const std::string& w : infer::speech_words(query)) phrase += (phrase.empty() ? "" : " ") + w;
  const double need = speech_coverage_needed(want.size(), precision);
  std::vector<std::tuple<float, std::int64_t, std::int64_t, std::string>> found;
  {
    std::lock_guard lock(speech_m_);
    for (const speech_row& r : speech_rows_) {
      if (!allow(r.asset)) continue;
      std::size_t hit = 0;
      for (const std::string& q : want) {
        for (const std::string& w : r.words) {
          if (w == q || (q.size() >= 4 && w.size() > q.size() && w.compare(0, q.size(), q) == 0)) {
            ++hit;
            break;
          }
        }
      }
      const double coverage = static_cast<double>(hit) / static_cast<double>(want.size());
      if (coverage < need) continue;
      std::string joined;
      for (const std::string& w : r.words) joined += (joined.empty() ? "" : " ") + w;
      const bool exact = joined.find(phrase) != std::string::npos;
      const float rank = static_cast<float>(0.6 + 0.8 * coverage + (exact ? 0.2 : 0.0));
      found.emplace_back(rank, r.asset, r.start_ms, r.text);
    }
  }
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });
  if (found.size() > 2000) found.resize(2000);
  for (auto& [rank, asset, start, text] : found) {
    std::string snippet = text.size() > 160 ? text.substr(0, 157) + "..." : text;
    add_row(st, asset, start, rank / 2.0f, rank, MV_AI_MATCH_SPEECH, std::move(snippet));
  }
}

std::vector<float> engine::query_vector(const loaded_clip& answer, const std::string& text) {
  if (!answer.model) return {};
  // The last noun in both numbers, averaged (query::number_forms): "mountain"
  // and "mountains" are one query, so one of them cannot fall through the
  // "nothing found" rule while the other answers (2026-09-28, plan/17).
  const std::vector<std::string> forms = query::number_forms(text);
  std::string key = answer.meta.spec_key;
  for (const std::string& f : forms) key += "\x1f" + f;
  {
    std::lock_guard lock(text_cache_m_);
    for (const auto& [k, v] : text_cache_) {
      if (k == key) return v;
    }
  }
  auto v = forms.size() == 1 ? answer.model->embed_text(forms.front()) : answer.model->embed_text_mean(forms);
  if (!v) return {};
  std::lock_guard lock(text_cache_m_);
  text_cache_.emplace_front(key, *v);
  if (text_cache_.size() > 64) text_cache_.pop_back();
  return *v;
}

std::vector<vector_store::hit> engine::picture_hits(const std::string& text,
                                                   const std::function<bool(std::int64_t)>& allow, bool gate,
                                                   std::uint32_t precision) {
  // The query is embedded by the tower whose vectors are in the matrix, and
  // both are read as one pair: a migration finishing (or a reload) meanwhile
  // runs the query again on the new pair.
  for (int attempt = 0; attempt < 3 && !stopping_; ++attempt) {
    const std::uint64_t gen = settled_answer_gen();
    loaded_clip answer;
    {
      std::lock_guard lock(models_m_);
      answer.model = answer_.model;
      answer.meta = answer_.meta;
    }
    const std::vector<float> v = query_vector(answer, text);
    std::vector<vector_store::hit> hits;
    if (!v.empty()) {
      text_thresholds t;
      t.result_margin = answer.meta.result_margin;
      t.query_margin = answer.meta.query_margin;
      t.query_z = answer.meta.query_z;
      t.result_z = answer.meta.result_z;
      // The rule, its noise scaling and the Precision setting: find_text
      // (vectors.h), the same function the calibration runs (plan/17).
      hits = find_text(store_, v, allow, t, precision_scale::at(precision), gate);
    }
    if (answer_gen_.load() != gen) continue;
    return hits;
  }
  return {};
}

std::uint64_t engine::search_text(const std::string& query_text, const std::string& scope_dir,
                                  std::uint32_t scope, std::uint32_t kinds) {
  return submit([this, query_text, scope_dir, scope, kinds](search_state& st) {
    if (!models_ready_) return;
    std::uint32_t precision;
    {
      std::lock_guard lock(settings_m_);
      precision = settings_.precision;
    }
    // The query language (query.h, plan/17 "Query syntax"): people, what the
    // picture shows, words said, kinds and file dates, each narrowing the
    // others. Names come from faces.db (PR 24) when people are on.
    const query::parsed parsed = query::parse(query_text);
    query::plan plan;
    std::map<std::int64_t, std::vector<face_row>> faces_by_person;
    {
      std::lock_guard lock(models_m_);
      std::vector<query::person_name> names;
      if (faces_) {
        for (auto& [id, name] : faces_->names()) names.push_back({id, std::move(name)});
      }
      plan = query::resolve(parsed, names);
      if (faces_) {
        const auto want = [&](std::int64_t p) {
          if (!faces_by_person.count(p)) faces_by_person[p] = faces_->faces_of(p);
        };
        for (const auto& group : plan.people) {
          for (std::int64_t p : group) want(p);
        }
        for (std::int64_t p : plan.not_people) want(p);
        for (std::int64_t p : plan.people_first) want(p);
      }
    }
    if (plan.impossible) return;
    // Kinds: the chips' and the query's, both.
    std::uint32_t k = kinds;
    if (plan.kinds != 0) {
      const std::uint32_t chip = (kinds & MV_AI_KIND_ALL) == 0 ? MV_AI_KIND_ALL : (kinds & MV_AI_KIND_ALL);
      const std::uint32_t both = chip & plan.kinds;
      if (both == 0) return;
      k = (kinds & ~MV_AI_KIND_ALL) | both;
    }
    auto allowed = scope_assets(scope_dir, scope, k, plan.from_unix, plan.to_unix);
    // file: / -file: (plan/17 "Query syntax"): the name, one pass over the
    // assets the scope, kind and dates left.
    if (plan.has_files()) {
      std::lock_guard lock(assets_m_);
      for (auto it = allowed->begin(); it != allowed->end();) {
        const auto m = assets_.find(*it);
        const bool keep = m != assets_.end() && query::name_matches(m->second.path, plan.files, plan.not_files);
        it = keep ? std::next(it) : allowed->erase(it);
      }
    }
    const auto keep_only = [&](const std::set<std::int64_t>& keep) {
      for (auto it = allowed->begin(); it != allowed->end();) {
        it = keep.count(*it) ? std::next(it) : allowed->erase(it);
      }
    };
    // Every person named (a group: any of its people).
    for (const auto& group : plan.people) {
      std::set<std::int64_t> any;
      for (std::int64_t p : group) {
        for (const face_row& f : faces_by_person[p]) any.insert(f.asset);
      }
      keep_only(any);
    }
    for (std::int64_t p : plan.not_people) {
      for (const face_row& f : faces_by_person[p]) allowed->erase(f.asset);
    }
    // Words said: every quoted phrase, in the transcript (as consecutive words).
    struct said {
      std::int64_t asset;
      std::int64_t start_ms;
      std::string text;
    };
    std::vector<said> said_rows;
    if (!plan.phrases.empty() || !plan.not_phrases.empty()) {
      std::vector<std::vector<std::string>> want, not_want;
      for (const std::string& p : plan.phrases) want.push_back(query::words_of(p));
      for (const std::string& p : plan.not_phrases) not_want.push_back(query::words_of(p));
      std::vector<std::set<std::int64_t>> have(want.size());
      std::set<std::int64_t> drop;
      {
        std::lock_guard lock(speech_m_);
        for (const speech_row& r : speech_rows_) {
          if (!allowed->count(r.asset)) continue;
          bool any = false;
          for (std::size_t i = 0; i < want.size(); ++i) {
            const bool open = plan.last_phrase_open && i + 1 == want.size();
            if (query::contains_phrase(r.words, want[i], open)) {
              have[i].insert(r.asset);
              any = true;
            }
          }
          for (const auto& w : not_want) {
            if (query::contains_phrase(r.words, w, false)) drop.insert(r.asset);
          }
          if (any) said_rows.push_back({r.asset, r.start_ms, r.text});
        }
      }
      for (const auto& h : have) keep_only(h);
      for (std::int64_t a : drop) allowed->erase(a);
    }
    // -beach: what a picture search for it finds is left out.
    for (const std::string& t : plan.not_text) {
      const auto current = [allowed](std::int64_t a) { return allowed->count(a) != 0; };
      for (const vector_store::hit& h : picture_hits(t, current, true, precision)) allowed->erase(h.asset);
    }
    if (allowed->empty()) return;
    const auto allow = [allowed](std::int64_t a) { return allowed->count(a) != 0; };
    const auto face_hits = [&](const std::vector<std::int64_t>& people) {
      std::vector<vector_store::hit> hits;
      for (std::int64_t p : people) {
        for (const face_row& f : faces_by_person[p]) {
          if (allowed->count(f.asset)) hits.push_back(vector_store::hit{f.asset, f.pts_ms, f.score, 0});
        }
      }
      std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
      return hits;
    };
    // A lone word that starts a name: those people first ("Trist" while it is
    // typed), then whatever the word describes.
    if (!plan.people_first.empty()) {
      for (const vector_store::hit& h : face_hits(plan.people_first)) {
        add_row(st, h.asset, h.pts_ms, h.score, 100.0f + h.score, MV_AI_MATCH_PICTURE, {});
      }
    }
    std::uint32_t find = k & (MV_AI_FIND_PICTURES | MV_AI_FIND_SOUNDS | MV_AI_FIND_SPEECH);
    if (find == 0) find = MV_AI_FIND_PICTURES | MV_AI_FIND_SOUNDS | MV_AI_FIND_SPEECH;
    if (!plan.text.empty()) {
      // A description, among the assets the rest allowed. Narrowed by a
      // person or a phrase, it ranks without "nothing found" (as a name in the
      // words always did).
      if (find & MV_AI_FIND_PICTURES) {
        group(st, picture_hits(plan.text, allow, !plan.narrows(), precision), true, -1.0f, MV_AI_MATCH_PICTURE, true);
      }
      merge_audio(st, plan.text, allow, find, precision);
    } else if (!plan.people.empty()) {
      std::vector<std::int64_t> everyone;
      for (const auto& g : plan.people) everyone.insert(everyone.end(), g.begin(), g.end());
      group(st, face_hits(everyone), false, 0);
    } else if (!plan.phrases.empty()) {
      for (said& r : said_rows) {
        if (!allowed->count(r.asset)) continue;
        std::string snippet = r.text.size() > 160 ? r.text.substr(0, 157) + "..." : std::move(r.text);
        add_row(st, r.asset, r.start_ms, 0.7f, 1.4f, MV_AI_MATCH_SPEECH, std::move(snippet));
      }
      said_rows.clear();
    } else if (plan.people_first.empty() &&
               (plan.kinds != 0 || plan.has_dates() || plan.has_files() || !plan.not_people.empty() ||
                !plan.not_text.empty() || !plan.not_phrases.empty())) {
      // Filters alone ("video in:2024", "file:IMG_12"): everything they allow,
      // newest first.
      std::vector<std::pair<std::int64_t, std::int64_t>> by_date;  // (mtime, asset)
      {
        std::lock_guard lock(assets_m_);
        for (std::int64_t a : *allowed) {
          if (auto it = assets_.find(a); it != assets_.end()) by_date.push_back({it->second.mtime, a});
        }
      }
      std::sort(by_date.begin(), by_date.end(), std::greater<>());
      if (by_date.size() > 1000) by_date.resize(1000);
      for (std::size_t i = 0; i < by_date.size(); ++i) {
        add_row(st, by_date[i].second, -1, 0, static_cast<float>(by_date.size() - i), 0, {});
      }
    }
    // The phrase under a result that matched on something else too.
    for (said& r : said_rows) {
      if (!st.row_of.count(r.asset)) continue;
      std::string snippet = r.text.size() > 160 ? r.text.substr(0, 157) + "..." : std::move(r.text);
      add_row(st, r.asset, r.start_ms, 0.0f, -1.0f, MV_AI_MATCH_SPEECH, std::move(snippet));
    }
    finish(st);
  });
}

std::string engine::suggest_json(const std::string& text) {
  std::vector<query::person_name> names;
  {
    std::lock_guard lock(models_m_);
    if (faces_) {
      for (auto& [id, name] : faces_->names()) names.push_back({id, std::move(name)});
    }
  }
  json::writer w;
  w.begin_array();
  for (const query::suggestion& s : query::suggest(text, names)) {
    w.begin_object();
    w.key("id").integer(s.id);
    w.key("name").string(s.name);
    w.key("completion").string(s.completion);
    w.end_object();
  }
  w.end_array();
  return w.take();
}

std::uint64_t engine::search_similar(const std::string& path, std::int64_t pts_ms, const std::string& scope_dir,
                                     std::uint32_t scope, std::uint32_t kinds) {
  return submit([this, path, pts_ms, scope_dir, scope, kinds](search_state& st) {
    if (!models_ready_) return;
    std::vector<float> q;
    std::int64_t self = 0;
    {
      std::lock_guard lock(assets_m_);
      const std::string key = path_key(path);
      for (const auto& [id, m] : assets_) {
        if (m.key == key) {
          self = id;
          break;
        }
      }
    }
    // An indexed still or sampled moment is its own query; a paused frame
    // that was never sampled is embedded now (plan/17 "Find similar"). The
    // query and the matrix it scans come from one model (answer_gen_).
    std::vector<vector_store::hit> hits;
    std::optional<rgb_frame> img;  // decoded once, embedded again after a swap
    for (int attempt = 0; attempt < 3 && !stopping_; ++attempt) {
      const std::uint64_t gen = settled_answer_gen();
      std::shared_ptr<infer::embedder> model;
      float similar_min = 0;
      {
        std::lock_guard lock(models_m_);
        model = answer_.model;
        similar_min = answer_.meta.similar_min;
      }
      q.clear();
      bool have = false;
      if (self != 0 && pts_ms < 0) have = store_.vector_of(self, -1, q);
      if (!have) {
        if (!model) return;
        if (!img) {
          auto decoded = frame_of(path, pts_ms, 448);
          if (!decoded) return;
          img = std::move(*decoded);
        }
        const infer::rgb_view view{img->rgb.data(), img->width, img->height};
        std::vector<float> e;
        if (!model->embed_images(std::span<const infer::rgb_view>(&view, 1), e)) return;
        q = std::move(e);
      }
      hits = store_.scan(q, scope_filter(scope_dir, scope, kinds), 5000, false, 0, similar_min);
      if (answer_gen_.load() == gen) break;
      hits.clear();
    }
    // Not the query itself: the same still, or the same moment of the clip.
    hits.erase(std::remove_if(hits.begin(), hits.end(),
                              [&](const vector_store::hit& h) {
                                return h.asset == self && (pts_ms < 0 || std::llabs(h.pts_ms - pts_ms) < 1000);
                              }),
               hits.end());
    group(st, hits, false, 0);
    finish(st);
  });
}

std::uint64_t engine::search_person(std::int64_t person, const std::string& scope_dir, std::uint32_t scope) {
  return submit([this, person, scope_dir, scope](search_state& st) {
    std::vector<face_row> faces;
    {
      std::lock_guard lock(models_m_);
      if (!faces_) return;
      faces = faces_->faces_of(person);
    }
    const auto allow = scope_filter(scope_dir, scope, MV_AI_KIND_ALL);
    std::vector<vector_store::hit> hits;
    for (const face_row& f : faces) {
      if (allow(f.asset)) hits.push_back(vector_store::hit{f.asset, f.pts_ms, f.score, 0});
    }
    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    group(st, hits, false, 0);
    finish(st);
  });
}

std::uint64_t engine::search_this_person(const std::string& path, std::int64_t pts_ms) {
  return submit([this, path, pts_ms](search_state& st) {
    std::shared_ptr<face_analyzer> model;
    {
      std::lock_guard lock(models_m_);
      model = faces_model_;
      if (!faces_ || !model) return;
    }
    auto img = frame_of(path, pts_ms, 1024);
    if (!img) return;
    auto found = model->analyze(*img);
    if (!found || found->empty()) return;
    const auto largest = std::max_element(found->begin(), found->end(),
                                          [](const face_in& a, const face_in& b) { return a.w * a.h < b.w * b.h; });
    std::int64_t person = 0;
    std::vector<face_row> faces;
    {
      std::lock_guard lock(models_m_);
      if (!faces_) return;
      person = faces_->nearest_person(largest->emb);
      if (person == 0) return;
      faces = faces_->faces_of(person);
    }
    std::vector<vector_store::hit> hits;
    for (const face_row& f : faces) hits.push_back(vector_store::hit{f.asset, f.pts_ms, f.score, 0});
    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    group(st, hits, false, 0);
    finish(st);
  });
}

result<std::string> engine::tile_of(const std::string& path, std::int64_t ms) const {
  if (is_photos_key(path)) {
    // A Photos still, or a clip Photos only has in iCloud: the chrome draws the
    // tile from PhotoKit's own thumbnail cache (no second cache of ours;
    // plan/17 "do not build a second thumbnail path"), so it gets the key.
    if (ms < 0) return path;
    auto file = file_of(path);
    if (!file) return path;
    if (auto hit = host_.moment_thumbnail(*file, ms, nullptr)) return hit;
    MV_TRY(rgb_frame f, host_.video_frame(*file, ms, 512));
    return host_.moment_thumbnail(*file, ms, &f);
  }
  if (ms < 0) return host_.thumbnail(path);
  if (auto hit = host_.moment_thumbnail(path, ms, nullptr)) return hit;
  MV_TRY(rgb_frame f, host_.video_frame(path, ms, 512));
  return host_.moment_thumbnail(path, ms, &f);
}

result<std::uint32_t> engine::result_count(std::uint64_t id) const {
  std::lock_guard lock(search_m_);
  auto it = searches_.find(id);
  if (it == searches_.end()) return err(status::invalid_arg);
  return static_cast<std::uint32_t>(it->second->rows.size());
}

result<mv_ai_result> engine::result_at(std::uint64_t id, std::uint32_t index) const {
  std::lock_guard lock(search_m_);
  auto it = searches_.find(id);
  if (it == searches_.end() || index >= it->second->rows.size()) return err(status::invalid_arg);
  const result_row& r = it->second->rows[index];
  mv_ai_result out{};
  out.asset_id = static_cast<std::uint64_t>(r.asset);
  out.pts_ms = r.pts_ms;
  out.score = r.score;
  out.kind = r.kind;
  out.more_in_clip = r.more;
  out.match = r.match;
  return out;
}

result<std::string> engine::result_snippet(std::uint64_t id, std::uint32_t index) const {
  std::lock_guard lock(search_m_);
  auto it = searches_.find(id);
  if (it == searches_.end() || index >= it->second->rows.size()) return err(status::invalid_arg);
  return it->second->rows[index].snippet;
}

result<std::string> engine::result_path(std::uint64_t id, std::uint32_t index) const {
  std::lock_guard lock(search_m_);
  auto it = searches_.find(id);
  if (it == searches_.end() || index >= it->second->rows.size()) return err(status::invalid_arg);
  return it->second->rows[index].path;
}

result<std::string> engine::result_thumb(std::uint64_t id, std::uint32_t index) const {
  std::string path;
  std::int64_t ms = -1;
  {
    std::lock_guard lock(search_m_);
    auto it = searches_.find(id);
    if (it == searches_.end() || index >= it->second->rows.size()) return err(status::invalid_arg);
    path = it->second->rows[index].path;
    ms = it->second->rows[index].kind == MV_AI_KIND_VIDEOS ? it->second->rows[index].pts_ms : -1;
  }
  return tile_of(path, ms);
}

result<std::vector<std::pair<std::int64_t, float>>> engine::clip_matches(std::uint64_t id,
                                                                         const std::string& path) const {
  std::int64_t asset = 0;
  {
    std::lock_guard lock(assets_m_);
    const std::string key = path_key(path);
    for (const auto& [aid, m] : assets_) {
      if (m.key == key) {
        asset = aid;
        break;
      }
    }
  }
  std::lock_guard lock(search_m_);
  auto it = searches_.find(id);
  if (it == searches_.end()) return err(status::invalid_arg);
  auto m = it->second->moments.find(asset);
  if (m == it->second->moments.end()) return std::vector<std::pair<std::int64_t, float>>{};
  std::vector<std::pair<std::int64_t, float>> out;
  for (const auto& p : m->second) {
    if (p.first >= 0) out.push_back(p);
  }
  return out;
}

void engine::search_release(std::uint64_t id) {
  std::lock_guard lock(search_m_);
  searches_.erase(id);
}

// ---- people -----------------------------------------------------------------------------------------

expected engine::faces_enable(bool enable) {
  if (importing_elsewhere()) return err(status::busy);
  {
    std::lock_guard lock(settings_m_);
    settings_.faces = enable;
  }
  save_settings();
  if (!enable) {
    // Off deletes every face vector, box and name at once (PR 24 verify:
    // "leaves no face vectors on disk").
    std::lock_guard lock(models_m_);
    faces_.reset();
    faces_model_.reset();
    faces_scanned_.clear();
    publish_pieces_locked();
    faces_db::destroy(join(data_dir_, "faces.db"));
  } else {
    reload_pieces_ = true;  // opens People; the picture towers stay
    control_cv_.notify_all();
  }
  post(MV_ADDON_EVENT_AI_PEOPLE);
  post(MV_ADDON_EVENT_AI_STATUS);
  return {};
}

std::string engine::people_json(const std::string& scope_dir, std::uint32_t scope) {
  json::writer w;
  w.begin_array();
  std::vector<person_row> people;
  // The open folder's assets, as a search would take them; none when the
  // caller wants everyone.
  std::shared_ptr<std::set<std::int64_t>> here;
  if (scope != MV_AI_SCOPE_ALL && !scope_dir.empty()) here = scope_assets(scope_dir, scope, MV_AI_KIND_ALL);
  {
    std::lock_guard lock(models_m_);
    if (faces_) people = faces_->people(kPeopleMinFaces, here.get());
  }
  for (const person_row& p : people) {
    w.begin_object();
    w.key("id").integer(p.id);
    w.key("name").string(p.name);
    w.key("faces").integer(p.faces);
    w.key("cover_face").integer(p.cover.id);
    w.key("cover_path").string(p.cover.path);
    w.key("cover_ms").integer(p.cover.pts_ms);
    w.key("cover_box").begin_array();
    w.number(p.cover.x).number(p.cover.y).number(p.cover.w).number(p.cover.h);
    w.end_array();
    w.end_object();
  }
  w.end_array();
  return w.take();
}

std::string engine::person_faces_json(std::int64_t person) {
  json::writer w;
  w.begin_array();
  std::vector<face_row> faces;
  {
    std::lock_guard lock(models_m_);
    if (faces_) faces = faces_->faces_of(person);
  }
  for (const face_row& f : faces) {
    w.begin_object();
    w.key("face").integer(f.id);
    w.key("path").string(f.path);
    w.key("pts_ms").integer(f.pts_ms);
    w.key("box").begin_array();
    w.number(f.x).number(f.y).number(f.w).number(f.h);
    w.end_array();
    w.key("score").number(f.score);
    w.end_object();
  }
  w.end_array();
  return w.take();
}

expected engine::person_rename(std::int64_t person, const std::string& name) {
  std::lock_guard lock(models_m_);
  if (!faces_) return err(status::invalid_arg);
  MV_TRY_VOID(faces_->rename(person, name));
  post(MV_ADDON_EVENT_AI_PEOPLE);
  return {};
}

expected engine::person_merge(std::int64_t into, std::int64_t from) {
  std::lock_guard lock(models_m_);
  if (!faces_) return err(status::invalid_arg);
  MV_TRY_VOID(faces_->merge(into, from));
  post(MV_ADDON_EVENT_AI_PEOPLE);
  return {};
}

expected engine::face_reject(std::int64_t face) {
  std::lock_guard lock(models_m_);
  if (!faces_) return err(status::invalid_arg);
  MV_TRY_VOID(faces_->reject(face));
  post(MV_ADDON_EVENT_AI_PEOPLE);
  return {};
}

result<std::string> engine::face_thumb(std::int64_t face) const {
  face_row f;
  {
    std::lock_guard lock(models_m_);
    if (!faces_) return err(status::invalid_arg);
    MV_TRY(face_row got, faces_->face(face));
    f = std::move(got);
  }
  return tile_of(f.path, f.pts_ms);
}

result<std::int64_t> engine::face_split(const std::vector<std::int64_t>& faces) {
  std::lock_guard lock(models_m_);
  if (!faces_) return err(status::invalid_arg);
  MV_TRY(std::int64_t id, faces_->split(faces));
  post(MV_ADDON_EVENT_AI_PEOPLE);
  return id;
}

}  // namespace mv::ai
