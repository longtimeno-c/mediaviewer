// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/import/engine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "addons/import/naming.h"
#include "addons/import/paths.h"
#include "addons/import/planner.h"
#include "addons/import/scanner.h"
#include "core/json.h"

namespace mv::import {
namespace {

using clock_type = std::chrono::steady_clock;

std::int64_t now_unix() { return static_cast<std::int64_t>(std::time(nullptr)); }

const char* state_word(std::uint32_t s) {
  switch (s) {
    case MV_IMPORT_JOB_QUEUED: return "queued";
    case MV_IMPORT_JOB_RUNNING: return "running";
    case MV_IMPORT_JOB_PAUSED: return "paused";
    case MV_IMPORT_JOB_DONE: return "done";
    case MV_IMPORT_JOB_FAILED: return "failed";
    case MV_IMPORT_JOB_CANCELLED: return "cancelled";
    case MV_IMPORT_JOB_INTERRUPTED: return "interrupted";
    default: return "none";
  }
}

const char* outcome_reason(std::uint32_t o) {
  switch (o) {
    case MV_COPY_NAME_TAKEN: return "a file with this name appeared at the destination";
    case MV_COPY_VERIFY_FAILED: return "the copy did not match the source when read back, twice";
    case MV_COPY_CANCELLED: return "cancelled";
    default: return "could not be written, twice";
  }
}

bool same_char(char a, char b) {
  if (a == '\\') a = '/';
  if (b == '\\') b = '/';
#if defined(_WIN32)
  if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
  if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
#endif
  return a == b;
}

// Whether `path` is `dir` or inside it.
bool under(const std::string& dir, const std::string& path) {
  const std::string d = normalize_root(dir);
  if (path.size() < d.size()) return false;
  for (std::size_t i = 0; i < d.size(); ++i) {
    if (!same_char(d[i], path[i])) return false;
  }
  if (path.size() == d.size()) return true;
  const char next = path[d.size()];
  return next == '/' || next == '\\' || d.back() == '/' || d.back() == '\\';
}

// `target` relative to `root`, '/'-separated (the library index's key).
std::string rel_under(const std::string& root, const std::string& target) {
  std::size_t start = root.size();
  while (start < target.size() && (target[start] == '/' || target[start] == '\\')) ++start;
  std::string rel = target.substr(std::min(start, target.size()));
  for (char& c : rel) {
    if (c == '\\') c = '/';
  }
  return rel;
}

// io/verified_copy.h's temporary names; the add-on does not link io, so the
// rule is restated here and pinned by a test.
std::string temp_name(const std::string& final_path, int attempt) {
  return final_path + ".mvtmp" + (attempt > 0 ? std::to_string(attempt + 1) : std::string());
}
constexpr int kTempAttempts = 16;

}  // namespace

// ---------------------------------------------------------------------------
struct engine::scan_slot {
  std::uint64_t id = 0;
  bool done = false;
  status result = status::ok;
  scan_result scan;
};

struct engine::plan_slot {
  std::uint64_t id = 0;
  std::shared_ptr<scan_slot> scan;
  bool done = false;
  status result = status::ok;
  plan_result plan;
  double rate = 0;
};

struct engine::job {
  std::uint64_t id = 0;
  std::uint32_t kind = 0;  // 0 import, 1 verify-a-folder
  std::atomic<std::uint32_t> state{MV_IMPORT_JOB_QUEUED};
  std::atomic<bool> cancel{false};
  std::atomic<bool> paused{false};
  std::atomic<bool> fast{false};
  std::atomic<bool> shutting_down{false};
  std::thread thread;
  bool finished = false;  // guarded by engine::mutex_
  bool persisted = false;

  preset settings;
  std::string source_root;
  std::string volume_id;
  std::string label;
  std::string device_key;
  bool source_removable = false;
  bool explicit_files = false;
  std::vector<std::string> dest_roots;
  std::vector<std::string> dest_keys;

  std::mutex m;  // guards everything below
  std::vector<journal_row> rows;
  mv_import_progress prog{};
  std::deque<std::pair<clock_type::time_point, std::uint64_t>> window;
  clock_type::time_point started{};
  clock_type::time_point last_post{};
  std::uint64_t read_total = 0;  // monotonic, for the rate window
  bool ejected = false;
  bool eject_failed = false;
  std::string verify_json;
  std::string report;
  std::string summary;
};

engine::engine(const mv_host_api* api) : host_(api) {}

engine::~engine() { shutdown(); }

expected engine::start(const std::string& db_path_override) {
  std::string db_path = db_path_override;
  if (auto dir = host_.data_dir()) data_dir_ = *dir;
  if (db_path.empty()) {
    if (data_dir_.empty()) return err(status::io);
    db_path = join_native(data_dir_, "import.db");
  } else if (data_dir_.empty()) {
    data_dir_ = parent_native(db_path);
  }
  MV_TRY(auto idx, library_index::open(db_path));
  idx_ = std::move(idx);
  next_job_ = idx_->max_job_id() + 1;
  // A job still "running" in the journal is one a crash, an unplug or a
  // closed app cut short. It is resumable, not lost (plan/18).
  for (const job_row& j : idx_->unfinished_jobs()) {
    if (j.state != MV_IMPORT_JOB_INTERRUPTED) {
      idx_->set_job_state(j.id, MV_IMPORT_JOB_INTERRUPTED, 0, "");
    }
  }
  control_ = std::thread([this] { control_loop(); });
  if (host_.api()->watch_volumes) {
    watching_ = host_.watch_volumes(
                         [](void* user, std::uint32_t event, const char* root) {
                           static_cast<engine*>(user)->on_volume(event, root ? root : "");
                         },
                         this)
                    .has_value();
  }
  return {};
}

void engine::shutdown() noexcept {
  if (watching_) {
    (void)host_.watch_volumes(nullptr, nullptr);
    watching_ = false;
  }
  std::vector<std::shared_ptr<job>> jobs;
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    for (auto& [id, j] : jobs_) jobs.push_back(j);
  }
  cancel_control_ = true;
  cv_.notify_all();
  for (auto& j : jobs) {
    // Closing the app is not a cancel: the job stays resumable.
    j->shutting_down = true;
    j->cancel = true;
    j->paused = false;
  }
  for (auto& j : jobs) {
    if (j->thread.joinable()) j->thread.join();
  }
  if (control_.joinable()) control_.join();
}

void engine::post_task(std::function<void()> fn) {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    tasks_.push_back(std::move(fn));
  }
  cv_.notify_all();
}

void engine::control_loop() {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
      if (stopping_) return;
      task = std::move(tasks_.front());
      tasks_.pop_front();
      busy_ = true;
    }
    task();
    {
      std::lock_guard lock(mutex_);
      busy_ = false;
    }
    cv_.notify_all();
  }
}

void engine::wait_idle() {
  for (;;) {
    bool any_running = false;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] { return stopping_ || (tasks_.empty() && !busy_); });
      for (auto& [id, j] : jobs_) {
        if (j->thread.joinable() && !j->finished) any_running = true;
      }
      if (!any_running && tasks_.empty() && !busy_) return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

std::string engine::default_library() const {
  if (auto d = host_.default_library_dir()) return *d;
  return {};
}

preset engine::last_preset() {
  preset p;
  const std::string name = idx_->setting("last_preset");
  if (!name.empty()) {
    if (auto text = idx_->preset_json(name)) (void)parse_preset(*text, p);
  }
  // plan/18 "Painless by default": the last destination used.
  if (p.destination.empty()) p.destination = idx_->setting("last_destination");
  return p;
}

std::mutex& engine::device_lock(const std::string& key) {
  std::lock_guard lock(mutex_);
  auto& slot = device_locks_[key];
  if (!slot) slot = std::make_unique<std::mutex>();
  return *slot;
}

std::shared_ptr<engine::job> engine::find_job(std::uint64_t id) {
  std::lock_guard lock(mutex_);
  auto it = jobs_.find(id);
  return it == jobs_.end() ? nullptr : it->second;
}

// ---------------------------------------------------------------------------
// Sources

std::string engine::sources_json() {
  std::map<std::string, std::int64_t> counts;
  {
    std::lock_guard lock(mutex_);
    counts = new_counts_;
  }
  const auto count_of = [&](const std::string& root) -> std::int64_t {
    auto it = counts.find(root);
    return it == counts.end() ? -1 : it->second;
  };
  json::writer w;
  w.begin_array();
  if (auto volumes = host_.list_volumes()) {
    for (const mv_addon_volume& v : *volumes) {
      // Cards, USB drives and shares; an internal disk is added as a folder.
      if (!v.removable && !v.network) continue;
      const auto binding = idx_->card_binding(v.volume_id);
      w.begin_object();
      w.key("root").string(v.root_utf8);
      w.key("label").string(v.label_utf8);
      w.key("volume_id").string(v.volume_id);
      w.key("kind").string(v.network ? "network" : "card");
      w.key("removable").boolean(v.removable != 0);
      w.key("total_bytes").integer(static_cast<std::int64_t>(v.total_bytes));
      w.key("free_bytes").integer(static_cast<std::int64_t>(v.free_bytes));
      w.key("new_count").integer(count_of(v.root_utf8));
      w.key("preset").string(binding ? binding->preset : std::string());
      w.key("auto_import").boolean(binding && binding->auto_import);
      w.end_object();
    }
  }
  for (const std::string& dir : idx_->folder_sources()) {
    const auto slash = dir.find_last_of("/\\");
    w.begin_object();
    w.key("root").string(dir);
    w.key("label").string(slash == std::string::npos ? dir : dir.substr(slash + 1));
    w.key("volume_id").string("");
    w.key("kind").string("folder");
    w.key("removable").boolean(false);
    w.key("total_bytes").integer(0);
    w.key("free_bytes").integer(0);
    w.key("new_count").integer(count_of(dir));
    w.key("preset").string("");
    w.key("auto_import").boolean(false);
    w.end_object();
  }
  w.end_array();
  return w.take();
}

expected engine::add_folder_source(const std::string& dir) {
  if (dir.empty()) return err(status::invalid_arg);
  idx_->add_folder_source(normalize_root(dir));
  return {};
}

expected engine::remove_folder_source(const std::string& dir) {
  idx_->remove_folder_source(normalize_root(dir));
  return {};
}

result<std::string> engine::arrival_root(std::uint64_t seq) {
  std::lock_guard lock(mutex_);
  auto it = arrivals_.find(seq);
  if (it == arrivals_.end()) return err(status::invalid_arg);
  return it->second;
}

void engine::on_volume(std::uint32_t event, const std::string& root) {
  if (event != 0) {
    {
      std::lock_guard lock(mutex_);
      new_counts_.erase(root);
    }
    host_.post(MV_ADDON_EVENT_VOLUME_REMOVED, MV_OK, 0, 0);
    return;
  }
  std::uint64_t seq = 0;
  {
    std::lock_guard lock(mutex_);
    seq = next_arrival_++;
    arrivals_[seq] = root;
  }
  // The watcher thread must not block: the decision runs on the control thread.
  post_task([this, seq, root] {
    auto vol = host_.volume_of(root);
    if (!vol || (!vol->removable && !vol->network)) {
      host_.post(MV_ADDON_EVENT_VOLUME_ARRIVED, MV_OK, seq, MV_IMPORT_ARRIVAL_NOTHING);
      return;
    }
    const auto binding = idx_->card_binding(vol->volume_id);
    if (binding && binding->auto_import) {
      // Opt-in per card, remembered by volume id. Never deletes: nothing in
      // Import does.
      preset p = last_preset();
      if (auto text = idx_->preset_json(binding->preset)) (void)parse_preset(*text, p);
      p.selection = selection_mode::new_only;
      auto j = make_job(0, p);
      pipeline_into(j, {}, root, p);
      host_.post(MV_ADDON_EVENT_VOLUME_ARRIVED, MV_OK, j->id, MV_IMPORT_ARRIVAL_AUTO);
      return;
    }
    const std::string on_insert = idx_->setting("on_insert");
    host_.post(MV_ADDON_EVENT_VOLUME_ARRIVED, MV_OK, seq,
               on_insert == "nothing" ? MV_IMPORT_ARRIVAL_NOTHING : MV_IMPORT_ARRIVAL_OPEN);
    // The source's "N new" badge, from the card memory: a lookup, not a hash.
    if (auto s = scan_folder(host_, *idx_, root, cancel_control_)) {
      std::int64_t fresh = 0;
      for (const unit& u : s->units) fresh += u.imported_before ? 0 : 1;
      {
        std::lock_guard lock(mutex_);
        new_counts_[root] = fresh;
      }
      host_.post(MV_ADDON_EVENT_SCAN_DONE, MV_OK, 0, fresh);
    }
  });
}

// ---------------------------------------------------------------------------
// Scan and plan

result<std::uint64_t> engine::scan(const std::string& root) {
  if (root.empty()) return err(status::invalid_arg);
  auto slot = std::make_shared<scan_slot>();
  {
    std::lock_guard lock(mutex_);
    slot->id = next_scan_++;
    scans_[slot->id] = slot;
  }
  post_task([this, slot, root] {
    auto s = scan_folder(host_, *idx_, root, cancel_control_);
    std::int64_t fresh = 0;
    {
      std::lock_guard lock(mutex_);
      slot->result = s ? status::ok : s.error();
      if (s) {
        slot->scan = std::move(*s);
        slot->scan.id = slot->id;
        for (const unit& u : slot->scan.units) fresh += u.imported_before ? 0 : 1;
        new_counts_[root] = fresh;
      }
      slot->done = true;
    }
    host_.post(MV_ADDON_EVENT_SCAN_DONE, static_cast<mv_status>(slot->result), slot->id, fresh);
  });
  return slot->id;
}

result<std::uint64_t> engine::scan_files(const std::vector<std::string>& paths) {
  if (paths.empty()) return err(status::invalid_arg);
  auto slot = std::make_shared<scan_slot>();
  {
    std::lock_guard lock(mutex_);
    slot->id = next_scan_++;
    scans_[slot->id] = slot;
  }
  post_task([this, slot, paths] {
    auto s = mv::import::scan_files(host_, *idx_, paths, cancel_control_);
    {
      std::lock_guard lock(mutex_);
      slot->result = s ? status::ok : s.error();
      if (s) {
        slot->scan = std::move(*s);
        slot->scan.id = slot->id;
      }
      slot->done = true;
    }
    host_.post(MV_ADDON_EVENT_SCAN_DONE, static_cast<mv_status>(slot->result), slot->id, 0);
  });
  return slot->id;
}

result<std::uint64_t> engine::plan(std::uint64_t scan_id, const std::string* preset_text,
                                   const std::vector<std::string>* marked) {
  preset parsed;
  if (preset_text && !parse_preset(*preset_text, parsed)) return err(status::invalid_arg);
  std::set<std::string> marks;
  if (marked) marks.insert(marked->begin(), marked->end());
  auto slot = std::make_shared<plan_slot>();
  {
    std::lock_guard lock(mutex_);
    auto it = scans_.find(scan_id);
    if (it == scans_.end()) return err(status::invalid_arg);
    slot->scan = it->second;
    slot->id = next_plan_++;
    plans_[slot->id] = slot;
  }
  const bool use_last = preset_text == nullptr;
  post_task([this, slot, parsed, use_last, marks] {
    status st = status::ok;
    {
      std::lock_guard lock(mutex_);
      // Scans run first on this same thread, so a known scan is done here.
      if (!slot->scan->done) st = status::invalid_arg;
      else if (slot->scan->result != status::ok) st = slot->scan->result;
    }
    if (st == status::ok) {
      const preset p = use_last ? last_preset() : parsed;
      // The scan is only mutated here, on the control thread (hashes learnt).
      auto planned = make_plan(host_, *idx_, slot->scan->scan, p, marks, default_library(),
                               cancel_control_);
      const std::string measured = idx_->setting("rate." + slot->scan->scan.device_key);
      std::lock_guard lock(mutex_);
      if (planned) {
        slot->plan = std::move(*planned);
        slot->plan.id = slot->id;
        slot->rate = measured.empty() ? 0.0 : std::strtod(measured.c_str(), nullptr);
      } else {
        st = planned.error();
      }
    }
    {
      std::lock_guard lock(mutex_);
      slot->result = st;
      slot->done = true;
    }
    host_.post(MV_ADDON_EVENT_PLAN_READY, static_cast<mv_status>(st), slot->id, 0);
  });
  return slot->id;
}

result<std::string> engine::plan_json(std::uint64_t plan_id) {
  std::lock_guard lock(mutex_);
  auto it = plans_.find(plan_id);
  if (it == plans_.end() || !it->second->done) return err(status::invalid_arg);
  const plan_slot& s = *it->second;
  if (s.result != status::ok) return err(s.result);
  return plan_to_json(s.scan->scan, s.plan, s.rate);
}

expected engine::select(std::uint64_t plan_id, int unit, const std::string* day, bool selected) {
  std::shared_ptr<plan_slot> slot;
  {
    std::lock_guard lock(mutex_);
    auto it = plans_.find(plan_id);
    if (it == plans_.end() || !it->second->done || it->second->result != status::ok) {
      return err(status::invalid_arg);
    }
    slot = it->second;
    if (unit >= 0 && static_cast<std::size_t>(unit) >= slot->plan.units.size()) {
      return err(status::invalid_arg);
    }
    if (!apply_selection(slot->plan, unit, day, selected)) return {};
  }
  // Names can move ({seq}; clashes a newly selected unit brings): recomputed
  // on the control thread, then PLAN_READY again.
  post_task([this, slot] {
    plan_result copy;
    {
      std::lock_guard lock(mutex_);
      copy = slot->plan;
    }
    assign_names(host_, *idx_, slot->scan->scan, copy, cancel_control_);
    {
      std::lock_guard lock(mutex_);
      for (std::size_t i = 0; i < copy.units.size() && i < slot->plan.units.size(); ++i) {
        plan_unit& live = slot->plan.units[i];
        live.folder = copy.units[i].folder;
        live.names = copy.units[i].names;
        live.seq = copy.units[i].seq;
        live.renamed_for_clash = copy.units[i].renamed_for_clash;
        if (copy.units[i].state == unit_state::duplicate) {
          live.state = unit_state::duplicate;
          live.matched = copy.units[i].matched;
          live.selected = false;
        }
        live.backup_needed = copy.units[i].backup_needed;
        live.backup_only = copy.units[i].backup_only;
      }
    }
    host_.post(MV_ADDON_EVENT_PLAN_READY, MV_OK, slot->id, 0);
  });
  return {};
}

result<std::string> engine::thumbnail(std::uint64_t plan_id, std::uint32_t unit) {
  std::string path;
  {
    std::lock_guard lock(mutex_);
    auto it = plans_.find(plan_id);
    if (it == plans_.end() || !it->second->done || unit >= it->second->plan.units.size()) {
      return err(status::invalid_arg);
    }
    const plan_slot& s = *it->second;
    const auto& u = s.scan->scan.units[s.plan.units[unit].unit];
    path = s.scan->scan.files[u.files.front()].path;
  }
  return host_.thumbnail(path);
}

// ---------------------------------------------------------------------------
// Jobs

std::shared_ptr<engine::job> engine::make_job(std::uint32_t kind, const preset& settings) {
  auto j = std::make_shared<job>();
  j->kind = kind;
  j->settings = settings;
  j->fast = settings.fast;
  std::lock_guard lock(mutex_);
  j->id = next_job_++;
  j->prog.job_id = j->id;
  j->prog.state = MV_IMPORT_JOB_QUEUED;
  j->prog.eta_seconds = -1;
  jobs_[j->id] = j;
  return j;
}

void engine::persist(const std::shared_ptr<job>& j) {
  if (j->persisted) return;
  job_row row;
  row.id = j->id;
  row.created = now_unix();
  row.state = MV_IMPORT_JOB_QUEUED;
  row.source_root = j->source_root;
  row.volume_id = j->volume_id;
  row.label = j->label;
  row.device_key = j->device_key;
  row.preset_json = preset_to_json(j->settings);
  row.kind = j->kind;
  j->persisted = idx_->create_job(row) != 0;
}

void engine::fill_job_from_plan(const std::shared_ptr<job>& j, const scan_result& scan,
                                const plan_result& plan) {
  j->source_root = scan.root;
  j->volume_id = scan.volume_id;
  j->label = scan.label;
  j->device_key = scan.device_key.empty() ? "src:" + scan.root : scan.device_key;
  j->source_removable = scan.removable;
  j->explicit_files = scan.explicit_files;
  j->settings = plan.settings;
  j->dest_roots = {plan.destination};
  if (!plan.backup.empty()) j->dest_roots.push_back(plan.backup);
  j->dest_keys.clear();
  for (const std::string& root : j->dest_roots) {
    (void)host_.make_directories(root);
    auto v = host_.volume_of(root);
    j->dest_keys.push_back(v && v->device_key[0] ? std::string(v->device_key) : "dst:" + root);
  }
  persist(j);

  std::vector<journal_row> rows;
  std::uint32_t unit_no = 0;
  std::uint64_t bytes = 0;
  std::uint32_t units = 0;
  std::uint32_t skipped = 0;
  const std::string& backup = plan.backup;
  for (const plan_unit& pu : plan.units) {
    const unit& u = scan.units[pu.unit];
    const std::string display = scan.files[u.files.front()].rel;
    // A duplicate on the main destination that the backup lacks: reported as
    // skipped (with what it matched) and, as a unit of its own, copied to
    // the backup alone.
    const bool backup_copy = !pu.selected && pu.backup_only && !backup.empty();
    if (!pu.selected) {
      // A duplicate decided up front is reported with what it matched.
      if (pu.state == unit_state::duplicate) {
        journal_row r;
        r.job = j->id;
        r.unit = unit_no++;
        r.src = scan.files[u.files.front()].path;
        r.rel = scan.files[u.files.front()].vol_rel;
        r.size = u.bytes;
        r.state = member_state::skipped;
        r.reason = pu.matched;
        r.display = display;
        rows.push_back(std::move(r));
        ++skipped;
      }
      if (!backup_copy) continue;
    }
    ++units;
    for (std::size_t m = 0; m < u.files.size(); ++m) {
      const source_file& f = scan.files[u.files[m]];
      journal_row r;
      r.job = j->id;
      r.unit = unit_no;
      r.member = static_cast<std::uint32_t>(m);
      r.src = f.path;
      r.rel = f.vol_rel;
      r.size = f.size;
      r.mtime = f.mtime;
      const std::string rel = pu.folder.empty() ? pu.names[m] : pu.folder + "/" + pu.names[m];
      for (const std::string& root : j->dest_roots) {
        if (backup_copy && root != backup) continue;
        r.targets.push_back(join_native(root, rel));
        r.target_roots.push_back(root);
      }
      r.display = display;
      bytes += f.size;
      rows.push_back(std::move(r));
    }
    // {seq} is committed when the job starts, so a second plan cannot reuse
    // it; a cancelled import leaves a gap, never a repeat.
    if (pu.seq > 0) idx_->commit_seq(plan.destination, pu.day, pu.seq);
    ++unit_no;
  }
  idx_->journal_add(rows);
  idx_->set_setting("last_preset", plan.settings.name);
  idx_->set_setting("last_destination", plan.destination);
  std::lock_guard lock(j->m);
  j->rows = std::move(rows);
  j->prog.units_total = units;
  j->prog.units_skipped = skipped;
  j->prog.bytes_total = bytes;
  j->prog.destination_count = static_cast<std::uint32_t>(j->dest_roots.size());
}

void engine::launch(const std::shared_ptr<job>& j) {
  std::lock_guard lock(mutex_);
  if (stopping_) return;
  if (j->thread.joinable()) j->thread.join();  // a resumed job's previous run
  j->finished = false;
  j->cancel = false;
  j->thread = std::thread([this, j] {
    if (j->kind == 1) {
      run_verify(j, j->source_root);
    } else {
      run_job(j);
    }
  });
}

result<std::uint64_t> engine::start_job(std::uint64_t plan_id) {
  std::shared_ptr<plan_slot> slot;
  {
    std::lock_guard lock(mutex_);
    auto it = plans_.find(plan_id);
    if (it == plans_.end() || !it->second->done || it->second->result != status::ok) {
      return err(status::invalid_arg);
    }
    slot = it->second;
  }
  auto j = make_job(0, slot->plan.settings);
  post_task([this, j, slot] {
    scan_result scan;
    plan_result plan;
    {
      std::lock_guard lock(mutex_);
      scan = slot->scan->scan;
      plan = slot->plan;
    }
    fill_job_from_plan(j, scan, plan);
    launch(j);
  });
  return j->id;
}

void engine::pipeline_into(const std::shared_ptr<job>& j, const std::vector<std::string>& paths,
                           const std::string& root, const preset& settings) {
  // Runs on the control thread.
  auto scanned = paths.empty() ? scan_folder(host_, *idx_, root, cancel_control_)
                               : mv::import::scan_files(host_, *idx_, paths, cancel_control_);
  if (!scanned) {
    j->source_root = root;
    persist(j);
    finish_job(j, MV_IMPORT_JOB_FAILED);
    return;
  }
  auto planned = make_plan(host_, *idx_, *scanned, settings, {}, default_library(),
                           cancel_control_);
  if (!planned) {
    j->source_root = scanned->root;
    persist(j);
    finish_job(j, MV_IMPORT_JOB_FAILED);
    return;
  }
  fill_job_from_plan(j, *scanned, *planned);
  launch(j);
}

result<std::uint64_t> engine::import_now(const std::vector<std::string>& paths) {
  if (paths.empty()) return err(status::invalid_arg);
  auto j = make_job(0, preset{});
  post_task([this, j, paths] {
    preset p = last_preset();
    p.selection = selection_mode::all;  // explicit files: "these"
    pipeline_into(j, paths, {}, p);
  });
  return j->id;
}

expected engine::pause(std::uint64_t id, bool paused) {
  auto j = find_job(id);
  if (!j) return err(status::invalid_arg);
  j->paused = paused;
  std::uint32_t from = paused ? MV_IMPORT_JOB_RUNNING : MV_IMPORT_JOB_PAUSED;
  j->state.compare_exchange_strong(from, paused ? MV_IMPORT_JOB_PAUSED : MV_IMPORT_JOB_RUNNING);
  return {};
}

expected engine::cancel(std::uint64_t id) {
  auto j = find_job(id);
  if (!j) return err(status::invalid_arg);
  j->cancel = true;
  j->paused = false;
  return {};
}

expected engine::set_priority(std::uint64_t id, bool fast) {
  auto j = find_job(id);
  if (!j) return err(status::invalid_arg);
  j->fast = fast;
  return {};
}

expected engine::progress(std::uint64_t id, mv_import_progress& out) {
  auto j = find_job(id);
  if (!j) return err(status::invalid_arg);
  std::lock_guard lock(j->m);
  out = j->prog;
  out.state = j->state.load();
  if (j->started != clock_type::time_point{} && out.state == MV_IMPORT_JOB_RUNNING) {
    out.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() -
                                                                           j->started)
                         .count();
  }
  return {};
}

void engine::progress_tick(job& j, std::uint64_t delta) {
  bool post = false;
  {
    std::lock_guard lock(j.m);
    const auto now = clock_type::now();
    j.prog.bytes_read += delta;
    j.read_total += delta;
    j.window.emplace_back(now, j.read_total);
    while (j.window.size() > 2 && now - j.window.front().first > std::chrono::seconds(10)) {
      j.window.pop_front();
    }
    const double span =
        std::chrono::duration<double>(now - j.window.front().first).count();
    if (span > 0.5) {
      j.prog.bytes_per_second =
          static_cast<double>(j.read_total - j.window.front().second) / span;
      const std::uint64_t left =
          j.prog.bytes_total > j.prog.bytes_read ? j.prog.bytes_total - j.prog.bytes_read : 0;
      j.prog.eta_seconds = j.prog.bytes_per_second > 0
                               ? static_cast<std::int64_t>(static_cast<double>(left) /
                                                           j.prog.bytes_per_second)
                               : -1;
    }
    j.prog.elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - j.started).count();
    if (now - j.last_post > std::chrono::milliseconds(250)) {
      j.last_post = now;
      post = true;
    }
  }
  if (post) host_.post(MV_ADDON_EVENT_JOB_PROGRESS, MV_OK, j.id, 0);
}

void engine::run_job(const std::shared_ptr<job>& j) {
  // One reader per physical source (plan/18: never parallel reads of a card).
  std::unique_lock source_lock(device_lock(j->device_key));
  {
    std::lock_guard lock(j->m);
    j->started = clock_type::now();
    j->window.clear();
    j->read_total = 0;
  }
  j->state = j->paused ? MV_IMPORT_JOB_PAUSED : MV_IMPORT_JOB_RUNNING;
  idx_->set_job_state(j->id, MV_IMPORT_JOB_RUNNING, 0, "");
  host_.post(MV_ADDON_EVENT_JOB_PROGRESS, MV_OK, j->id, 0);

  copy_callbacks cb;
  cb.cancelled = [&j] { return j->cancel.load(); };
  cb.progress = [this, &j](std::uint64_t delta) { progress_tick(*j, delta); };
  cb.yield = [this, &j] {
    // Pause, and background priority: wait between buffers while the viewer
    // needs the machine (plan/18 "Priority"). Both present-loop gates are
    // measured with this in place.
    while (!j->cancel.load() && (j->paused.load() || (!j->fast.load() && host_.should_yield()))) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  };

  // Units in journal order; a resumed job skips members already verified.
  std::vector<std::size_t> order;
  {
    std::lock_guard lock(j->m);
    order.resize(j->rows.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
  }
  bool interrupted = false;
  bool any_failed = false;

  const auto root_present = [this](const std::string& root) {
    auto st = host_.stat(root);
    return st && st->is_directory;
  };

  std::size_t at = 0;
  while (at < order.size() && !j->cancel && !interrupted) {
    // One unit: rows [at, end) share a unit number.
    std::size_t end = at;
    std::uint32_t unit_no = 0;
    {
      std::lock_guard lock(j->m);
      unit_no = j->rows[order[at]].unit;
      while (end < order.size() && j->rows[order[end]].unit == unit_no) ++end;
    }
    bool unit_ok = true;
    bool unit_has_work = false;
    std::string reason;
    std::vector<std::size_t> copied_now;

    for (std::size_t k = at; k < end && unit_ok; ++k) {
      journal_row row;
      {
        std::lock_guard lock(j->m);
        row = j->rows[order[k]];
      }
      if (row.state != member_state::pending) continue;
      unit_has_work = true;
      cb.yield();
      if (j->cancel) break;
      {
        std::lock_guard lock(j->m);
        std::snprintf(j->prog.current_name_utf8, sizeof j->prog.current_name_utf8, "%s",
                      row.display.c_str());
      }
      for (const std::string& t : row.targets) (void)host_.make_directories(parent_native(t));

      // One writer per physical destination: hold each destination device
      // for the length of this file (sorted, so two jobs cannot deadlock).
      std::vector<std::string> keys = j->dest_keys;
      std::sort(keys.begin(), keys.end());
      keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
      std::vector<std::unique_lock<std::mutex>> dest_locks;
      for (const std::string& key : keys) {
        if (key != j->device_key) dest_locks.emplace_back(device_lock(key));
      }
      mv_addon_copy_result res{};
      const auto copied = host_.copy(row.src, row.targets, j->settings.full_verify, cb,
                                     copy_fault_request{}, res);
      dest_locks.clear();

      if (!copied && copied.error() == status::cancelled) break;
      bool member_ok = copied.has_value();
      std::uint32_t bad = MV_COPY_WRITE_FAILED;
      const auto written = [&res](std::uint32_t t) {
        return res.outcome[t] == MV_COPY_VERIFIED || res.outcome[t] == MV_COPY_WRITTEN_UNVERIFIED;
      };
      if (member_ok) {
        for (std::uint32_t t = 0; t < res.target_count; ++t) {
          if (!written(t)) {
            member_ok = false;
            bad = res.outcome[t];
          }
        }
        // The retry read different bytes: targets that verified on the two
        // attempts hold different content, and neither can be trusted.
        if (res.source_unstable) member_ok = false;
      }
      if (!member_ok && copied) {
        // Never half a member either: a destination that did take this file
        // while another failed (a backup drive, a cancel during read-back)
        // gives it back, so a retry or resume finds the name free. Only what
        // this copy renamed into place is removed; a taken name never is.
        for (std::uint32_t t = 0; t < res.target_count && t < row.targets.size(); ++t) {
          if (written(t)) (void)host_.remove_file(row.targets[t]);
        }
      }
      if (member_ok) {
        digest d;
        std::memcpy(d.data(), res.source_hash, d.size());
        idx_->journal_set(j->id, row.unit, row.member, member_state::done, &d, "");
        std::lock_guard lock(j->m);
        j->rows[order[k]].state = member_state::done;
        j->rows[order[k]].hash = d;
        // Per destination, not per target: a backup-only or resumed row
        // lists fewer targets than the job has destinations.
        for (const std::string& root : row.target_roots) {
          const auto di = static_cast<std::size_t>(
              std::find(j->dest_roots.begin(), j->dest_roots.end(), root) - j->dest_roots.begin());
          if (di < MV_IMPORT_MAX_DESTINATIONS) j->prog.bytes_verified[di] += row.size;
        }
        copied_now.push_back(order[k]);
        continue;
      }
      if (bad == MV_COPY_CANCELLED) break;
      // A pulled card or destination is an interruption, not a failure: the
      // rest stays pending and the job resumes when it is back.
      bool gone = !root_present(j->source_root) && !j->source_root.empty();
      for (const std::string& root : j->dest_roots) gone = gone || !root_present(root);
      if (gone) {
        interrupted = true;
        unit_ok = false;
        break;
      }
      unit_ok = false;
      reason = copied ? outcome_reason(bad) : "the source could not be read";
      if (res.source_unstable) reason = "the source read differently twice: the card may be failing";
      idx_->journal_set(j->id, row.unit, row.member, member_state::failed, nullptr, reason);
      std::lock_guard lock(j->m);
      j->rows[order[k]].state = member_state::failed;
      j->rows[order[k]].reason = reason;
    }

    if (j->cancel || interrupted || !unit_ok) {
      // Never half a unit: take back what this unit wrote. A resumed unit's
      // earlier members stay (they are verified and journaled).
      if (!unit_ok || j->cancel || interrupted) {
        for (std::size_t idx : copied_now) {
          journal_row row;
          {
            std::lock_guard lock(j->m);
            row = j->rows[idx];
            j->rows[idx].state = interrupted || j->cancel ? member_state::pending : member_state::failed;
            j->rows[idx].reason = interrupted || j->cancel ? "" : "rolled back with its pair: " + reason;
          }
          for (const std::string& t : row.targets) (void)host_.remove_file(t);
          idx_->journal_set(j->id, row.unit, row.member,
                            interrupted || j->cancel ? member_state::pending : member_state::failed,
                            nullptr, interrupted || j->cancel ? "" : "rolled back with its pair");
        }
      }
      if (!unit_ok && !interrupted && !j->cancel) {
        any_failed = true;
        // Mark the unit's untouched members failed too, so it reads as one.
        // The journal is written outside j->m: progress() takes that lock on
        // the UI thread and must never wait on a database write.
        std::vector<journal_row> marked;
        {
          std::lock_guard lock(j->m);
          ++j->prog.units_failed;
          for (std::size_t k = at; k < end; ++k) {
            if (j->rows[order[k]].state == member_state::pending) {
              j->rows[order[k]].state = member_state::failed;
              j->rows[order[k]].reason = "not copied: " + reason;
              marked.push_back(j->rows[order[k]]);
            }
          }
        }
        for (const journal_row& r : marked) {
          idx_->journal_set(j->id, r.unit, r.member, member_state::failed, nullptr, r.reason);
        }
      }
      if (j->cancel || interrupted) break;
      at = end;
      continue;
    }

    // The whole unit is verified: now it is in the library and the card
    // memory, and "new since last import" no longer lists it.
    // (A unit a crash cut short after its last member verified has no work
    // left but may still owe these rows; upserts are idempotent.)
    bool unit_done = true;
    std::vector<journal_row> members;
    {
      std::lock_guard lock(j->m);
      for (std::size_t k = at; k < end; ++k) {
        members.push_back(j->rows[order[k]]);
        if (j->rows[order[k]].state != member_state::done) unit_done = false;
      }
    }
    if (unit_done) {
      const std::int64_t now = now_unix();
      for (const journal_row& r : members) {
        for (std::size_t t = 0; t < r.targets.size(); ++t) {
          idx_->upsert(library_row{r.target_roots[t], rel_under(r.target_roots[t], r.targets[t]),
                                   r.size, r.mtime, r.hash});
        }
        if (!j->volume_id.empty()) idx_->card_record(j->volume_id, r.rel, r.size, r.mtime, r.hash, now);
      }
      if (unit_has_work) {
        std::lock_guard lock(j->m);
        ++j->prog.units_done;
      }
    }
    at = end;
  }
  source_lock.unlock();

  std::uint32_t state = MV_IMPORT_JOB_DONE;
  if (interrupted || (j->cancel && j->shutting_down)) state = MV_IMPORT_JOB_INTERRUPTED;
  else if (j->cancel) state = MV_IMPORT_JOB_CANCELLED;
  else if (any_failed) state = MV_IMPORT_JOB_FAILED;

  if (state == MV_IMPORT_JOB_DONE && j->settings.eject_after && j->source_removable &&
      !j->explicit_files && !j->source_root.empty()) {
    // plan/18 "After import: Eject card" (the default). Unmount and eject
    // only; Import never formats or erases.
    const bool ok = host_.eject(j->source_root).has_value();
    std::lock_guard lock(j->m);
    j->ejected = ok;
    j->eject_failed = !ok;
  }
  finish_job(j, state);
}

std::string engine::make_summary(job& j, std::uint32_t state) {
  std::lock_guard lock(j.m);
  if (j.kind == 1) return j.verify_json;
  std::uint32_t copied_units = 0;
  std::uint32_t copied_files = 0;
  std::uint64_t copied_bytes = 0;
  std::map<std::uint32_t, bool> unit_done;
  for (const journal_row& r : j.rows) {
    if (r.state == member_state::done) {
      ++copied_files;
      copied_bytes += r.size;
      unit_done.emplace(r.unit, true);
    }
  }
  copied_units = static_cast<std::uint32_t>(unit_done.size());

  json::writer w;
  w.begin_object();
  w.key("job").integer(static_cast<std::int64_t>(j.id));
  w.key("state").string(state_word(state));
  w.key("source").string(j.source_root);
  w.key("label").string(j.label);
  w.key("destinations").begin_array();
  for (const std::string& d : j.dest_roots) w.string(d);
  w.end_array();
  w.key("verified").boolean(j.settings.full_verify);
  w.key("copied").begin_object();
  w.key("units").integer(copied_units);
  w.key("files").integer(copied_files);
  w.key("bytes").integer(static_cast<std::int64_t>(copied_bytes));
  w.end_object();
  w.key("skipped").begin_array();
  for (const journal_row& r : j.rows) {
    if (r.state != member_state::skipped) continue;
    w.begin_object();
    w.key("name").string(r.display);
    w.key("matched").string(r.reason);
    w.end_object();
  }
  w.end_array();
  w.key("failed").begin_array();
  for (const journal_row& r : j.rows) {
    if (r.state != member_state::failed) continue;
    w.begin_object();
    w.key("name").string(r.src.substr(r.src.find_last_of("/\\") + 1));
    w.key("reason").string(r.reason);
    w.end_object();
  }
  w.end_array();
  std::uint32_t pending = 0;
  for (const journal_row& r : j.rows) pending += r.state == member_state::pending ? 1 : 0;
  w.key("pending_files").integer(pending);
  w.key("ejected").boolean(j.ejected);
  w.key("eject_failed").boolean(j.eject_failed);
  w.key("elapsed_ms").integer(j.prog.elapsed_ms);
  w.key("bytes_per_second").number(j.prog.bytes_per_second);
  w.key("report").string(j.report);
  w.end_object();
  return w.take();
}

void engine::finish_job(const std::shared_ptr<job>& j, std::uint32_t state) {
  double rate = 0;
  {
    std::lock_guard lock(j->m);
    if (j->started != clock_type::time_point{}) {
      j->prog.elapsed_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - j->started)
              .count();
    }
    if (state == MV_IMPORT_JOB_DONE || state == MV_IMPORT_JOB_FAILED) j->prog.eta_seconds = 0;
    j->prog.current_name_utf8[0] = '\0';
    // The measured rate on this source device is the next plan's ETA.
    if (j->kind == 0 && j->prog.bytes_read > (64u << 20) && j->prog.elapsed_ms > 0) {
      rate = static_cast<double>(j->prog.bytes_read) * 1000.0 /
             static_cast<double>(j->prog.elapsed_ms);
    }
  }
  // Outside j->m, which the UI thread's progress() takes.
  if (rate > 0) idx_->set_setting("rate." + j->device_key, std::to_string(rate));
  j->state = state;

  // The local report: plain text beside import.db, never sent anywhere (rule 6).
  if (!data_dir_.empty() && j->persisted) {
    const std::string dir = join_native(data_dir_, "reports");
    (void)host_.make_directories(dir);
    std::string text = "MediaViewer Import report\n";
    {
      std::lock_guard lock(j->m);
      text += "Job " + std::to_string(j->id) + ": " + state_word(state) + "\n";
      text += "Source: " + j->source_root + (j->label.empty() ? "" : " (" + j->label + ")") + "\n";
      for (const std::string& d : j->dest_roots) text += "Destination: " + d + "\n";
      if (j->kind == 1) {
        text += j->verify_json + "\n";
      }
      for (const journal_row& r : j->rows) {
        const char* word = r.state == member_state::done      ? "copied   "
                           : r.state == member_state::skipped ? "skipped  "
                           : r.state == member_state::failed  ? "FAILED   "
                                                              : "pending  ";
        text += word + r.src;
        if (!r.targets.empty() && r.state == member_state::done) text += " -> " + r.targets.front();
        if (!r.reason.empty()) text += "  (" + r.reason + ")";
        text += "\n";
      }
    }
    for (int n = 0; n < 100; ++n) {
      const std::string path = join_native(
          dir, "import-" + std::to_string(j->id) + (n ? "-" + std::to_string(n) : "") + ".txt");
      if (host_.write_new_file(path, text)) {
        std::lock_guard lock(j->m);
        j->report = path;
        break;
      }
    }
  }
  const std::string summary = make_summary(*j, state);
  {
    std::lock_guard lock(j->m);
    j->summary = summary;
  }
  if (j->persisted) idx_->set_job_state(j->id, state, now_unix(), summary);
  {
    std::lock_guard lock(mutex_);
    j->finished = true;
  }
  host_.post(j->kind == 1 ? MV_ADDON_EVENT_VERIFY_DONE : MV_ADDON_EVENT_JOB_DONE, MV_OK, j->id,
             state);
  cv_.notify_all();
}

result<std::string> engine::summary_json(std::uint64_t id) {
  if (auto j = find_job(id)) {
    std::lock_guard lock(j->m);
    if (!j->summary.empty()) return j->summary;
  }
  auto row = idx_->job(id);
  if (!row || row->summary_json.empty()) return err(status::invalid_arg);
  return row->summary_json;
}

result<std::string> engine::report_path(std::uint64_t id) {
  auto j = find_job(id);
  if (!j) return err(status::invalid_arg);
  std::lock_guard lock(j->m);
  if (j->report.empty()) return err(status::invalid_arg);
  return j->report;
}

result<std::uint64_t> engine::retry_failed(std::uint64_t id) {
  // The old job is read on the control thread: this call comes from the UI
  // thread, which never waits on import.db (rule 1).
  auto j = make_job(0, preset{});
  post_task([this, j, id] {
    auto old = idx_->job(id);
    preset p;
    if (old) (void)parse_preset(old->preset_json, p);
    p.selection = selection_mode::all;
    j->settings = p;
    j->fast = p.fast;
    // Re-planned from the files themselves, so a name that is now taken gets
    // a safe name and a file that now matches is skipped.
    std::vector<std::string> paths;
    for (const journal_row& r : old ? idx_->journal(id) : std::vector<journal_row>{}) {
      if (r.state == member_state::failed || r.state == member_state::cancelled ||
          r.state == member_state::pending) {
        if (std::find(paths.begin(), paths.end(), r.src) == paths.end()) paths.push_back(r.src);
      }
    }
    if (paths.empty()) {
      persist(j);
      finish_job(j, old ? MV_IMPORT_JOB_DONE : MV_IMPORT_JOB_FAILED);
      return;
    }
    pipeline_into(j, paths, {}, p);
  });
  return j->id;
}

std::string engine::unfinished_json() {
  json::writer w;
  w.begin_array();
  for (const job_row& r : idx_->unfinished_jobs()) {
    if (auto live = find_job(r.id)) {
      std::lock_guard lock(mutex_);
      if (!live->finished) continue;  // still running
    }
    std::uint32_t pending = 0;
    std::uint32_t done = 0;
    for (const journal_row& row : idx_->journal(r.id)) {
      pending += row.state == member_state::pending ? 1 : 0;
      done += row.state == member_state::done ? 1 : 0;
    }
    w.begin_object();
    w.key("job").integer(static_cast<std::int64_t>(r.id));
    w.key("source").string(r.source_root);
    w.key("label").string(r.label);
    w.key("volume_id").string(r.volume_id);
    w.key("pending").integer(pending);
    w.key("done").integer(done);
    w.key("created").integer(r.created);
    w.end_object();
  }
  w.end_array();
  return w.take();
}

expected engine::resume(std::uint64_t id) {
  {
    std::lock_guard lock(mutex_);  // guards `finished` and the thread handle
    auto it = jobs_.find(id);
    if (it != jobs_.end() && !it->second->finished && it->second->thread.joinable()) {
      return err(status::invalid_arg);  // running
    }
  }
  post_task([this, id] { resume_on_control(id); });
  return {};
}

void engine::resume_on_control(std::uint64_t id) {
  auto row = idx_->job(id);
  if (!row) return;
  preset p;
  (void)parse_preset(row->preset_json, p);
  std::shared_ptr<job> j = find_job(id);
  if (!j) {
    j = std::make_shared<job>();
    j->id = id;
    std::lock_guard lock(mutex_);
    jobs_[id] = j;
  }
  j->kind = 0;
  j->settings = p;
  j->fast = p.fast;
  j->persisted = true;
  j->source_root = row->source_root;
  j->volume_id = row->volume_id;
  j->label = row->label;
  j->device_key = row->device_key;
  j->cancel = false;
  j->shutting_down = false;
  j->paused = false;
  if (auto v = host_.volume_of(j->source_root)) j->source_removable = v->removable != 0;

  std::vector<journal_row> rows = idx_->journal(id);
  // Destinations in the job's order (main, then backup): rows that list
  // every destination set it; a backup-only row lists the backup alone.
  j->dest_roots.clear();
  for (const bool full : {true, false}) {
    for (const journal_row& r : rows) {
      if (full && r.target_roots.size() < 2) continue;
      for (const std::string& root : r.target_roots) {
        if (std::find(j->dest_roots.begin(), j->dest_roots.end(), root) == j->dest_roots.end()) {
          j->dest_roots.push_back(root);
        }
      }
    }
  }
  j->dest_keys.clear();
  for (const std::string& root : j->dest_roots) {
    auto v = host_.volume_of(root);
    j->dest_keys.push_back(v && v->device_key[0] ? std::string(v->device_key) : "dst:" + root);
  }

  // Reconcile what a crash may have left: temporaries go; a final name that
  // is already there is this file only if it hashes the same as the source.
  std::uint64_t bytes = 0;
  std::uint32_t units_total = 0;
  std::uint32_t units_done = 0;
  std::uint32_t skipped = 0;
  std::map<std::uint32_t, bool> unit_complete;
  for (journal_row& r : rows) {
    if (r.state == member_state::skipped) {
      ++skipped;
      continue;
    }
    if (r.state == member_state::failed) continue;  // Retry failed is its own job
    unit_complete.emplace(r.unit, true);
    if (r.state == member_state::done) continue;
    r.state = member_state::pending;
    bytes += r.size;
    for (const std::string& t : r.targets) {
      for (int n = 0; n < kTempAttempts; ++n) (void)host_.remove_file(temp_name(t, n));
    }
    // Per destination: a crash between the two renames leaves the file on
    // one destination and not the other. What is there and matches the
    // source is kept; only the missing destinations are copied.
    copy_callbacks none;
    std::optional<digest> src;
    bool src_failed = false;
    std::vector<std::size_t> present;
    for (std::size_t t = 0; t < r.targets.size() && !src_failed; ++t) {
      auto st = host_.stat(r.targets[t]);
      if (!st || st->size != r.size) continue;  // missing, or a clash the copy will report
      if (!src) {
        auto h = host_.hash(r.src, false, none);
        if (!h) {
          src_failed = true;
          break;
        }
        src = *h;
      }
      auto dst = host_.hash(r.targets[t], true, none);
      if (dst && *dst == *src) present.push_back(t);
    }
    if (!present.empty() && present.size() == r.targets.size()) {
      r.state = member_state::done;
      r.hash = *src;
      idx_->journal_set(id, r.unit, r.member, member_state::done, &r.hash, "");
      bytes -= r.size;
    } else if (!present.empty()) {
      // Index the verified copies now (the unit's completion only indexes the
      // targets it copies) and leave just the missing ones to copy. The
      // journal keeps the full list, so another resume reconciles again.
      for (auto it = present.rbegin(); it != present.rend(); ++it) {
        const std::string root = r.target_roots[*it];
        idx_->upsert(library_row{root, rel_under(root, r.targets[*it]), r.size, r.mtime, *src});
        r.targets.erase(r.targets.begin() + static_cast<std::ptrdiff_t>(*it));
        r.target_roots.erase(r.target_roots.begin() + static_cast<std::ptrdiff_t>(*it));
      }
    }
    if (r.state != member_state::done) unit_complete[r.unit] = false;
  }
  for (const auto& [unit_no, complete] : unit_complete) {
    ++units_total;
    units_done += complete ? 1 : 0;
  }
  {
    std::lock_guard lock(j->m);
    j->rows = std::move(rows);
    j->prog = mv_import_progress{};
    j->prog.job_id = id;
    j->prog.units_total = units_total;
    j->prog.units_done = units_done;
    j->prog.units_skipped = skipped;
    j->prog.bytes_total = bytes;
    j->prog.eta_seconds = -1;
    j->prog.destination_count = static_cast<std::uint32_t>(j->dest_roots.size());
  }
  j->state = MV_IMPORT_JOB_QUEUED;
  launch(j);
}

expected engine::eject(const std::string& root) { return host_.eject(root); }

// ---------------------------------------------------------------------------
// Presets

std::string engine::presets_json() {
  json::writer w;
  w.begin_object();
  w.key("last").string(idx_->setting("last_preset"));
  w.key("last_destination").string(idx_->setting("last_destination"));
  w.key("default_destination").string(default_library());
  w.key("on_insert").string(idx_->setting("on_insert").empty() ? "open" : idx_->setting("on_insert"));
  w.key("presets").begin_array();
  bool have_default = false;
  for (const std::string& text : idx_->preset_jsons()) {
    preset p;
    if (!parse_preset(text, p)) continue;
    have_default = have_default || p.name == "Default";
    w.raw(preset_to_json(p));
  }
  if (!have_default) w.raw(preset_to_json(preset{}));
  w.end_array();
  w.end_object();
  return w.take();
}

expected engine::save_preset(const std::string& text) {
  preset p;
  if (!parse_preset(text, p)) return err(status::invalid_arg);
  idx_->save_preset(p.name, preset_to_json(p));
  return {};
}

expected engine::delete_preset(const std::string& name) {
  if (name.empty()) return err(status::invalid_arg);
  idx_->delete_preset(name);
  return {};
}

expected engine::bind_card(const std::string& volume_id, const std::string& preset_name,
                           bool auto_import) {
  if (volume_id.empty()) return err(status::invalid_arg);
  if (!preset_name.empty() && preset_name != "Default" && !idx_->preset_json(preset_name)) {
    return err(status::invalid_arg);
  }
  idx_->bind_card(volume_id, preset_name, auto_import);
  return {};
}

result<std::string> engine::preview_names_json(const std::string& text) {
  preset p;
  if (!parse_preset(text, p)) return err(status::invalid_arg);
  return preview_json(p, default_library());
}

// ---------------------------------------------------------------------------
// Library tools (PR 19)

std::string engine::history_json() {
  json::writer w;
  w.begin_array();
  for (const job_row& r : idx_->jobs(500)) {
    w.begin_object();
    w.key("job").integer(static_cast<std::int64_t>(r.id));
    w.key("kind").string(r.kind == 1 ? "verify" : "import");
    w.key("created").integer(r.created);
    w.key("finished").integer(r.finished);
    w.key("state").string(state_word(r.state));
    w.key("source").string(r.source_root);
    w.key("label").string(r.label);
    w.key("volume_id").string(r.volume_id);
    w.key("summary").raw(r.summary_json);
    w.end_object();
  }
  w.end_array();
  return w.take();
}

result<std::uint64_t> engine::verify_folder(const std::string& dir) {
  if (dir.empty()) return err(status::invalid_arg);
  auto j = make_job(1, preset{});
  j->source_root = normalize_root(dir);
  j->label = "verify";
  j->device_key = "verify:" + j->source_root;
  post_task([this, j] {
    if (auto v = host_.volume_of(j->source_root); v && v->device_key[0]) j->device_key = v->device_key;
    persist(j);
    launch(j);
  });
  return j->id;
}

void engine::run_verify(const std::shared_ptr<job>& j, const std::string& dir) {
  std::unique_lock source_lock(device_lock(j->device_key));
  {
    std::lock_guard lock(j->m);
    j->started = clock_type::now();
  }
  j->state = MV_IMPORT_JOB_RUNNING;
  idx_->set_job_state(j->id, MV_IMPORT_JOB_RUNNING, 0, "");

  std::vector<library_row> rows;
  for (library_row& r : idx_->all_rows()) {
    if (under(dir, join_native(r.root, r.rel))) rows.push_back(std::move(r));
  }
  std::uint64_t total = 0;
  for (const library_row& r : rows) total += r.size;
  {
    std::lock_guard lock(j->m);
    j->prog.units_total = static_cast<std::uint32_t>(rows.size());
    j->prog.bytes_total = total;
  }

  copy_callbacks cb;
  cb.cancelled = [&j] { return j->cancel.load(); };
  cb.yield = [this, &j] {
    while (!j->cancel.load() && (j->paused.load() || (!j->fast.load() && host_.should_yield()))) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  };
  struct finding {
    std::string rel;
    const char* what;
  };
  std::vector<finding> bad;
  std::uint32_t ok = 0;
  std::set<std::string> indexed;
  for (const library_row& r : rows) {
    if (j->cancel) break;
    const std::string full = join_native(r.root, r.rel);
    indexed.insert(full);
    {
      std::lock_guard lock(j->m);
      std::snprintf(j->prog.current_name_utf8, sizeof j->prog.current_name_utf8, "%s", r.rel.c_str());
    }
    auto st = host_.stat(full);
    if (!st) {
      bad.push_back({full, "missing"});
      continue;
    }
    // Read back from the device, not the cache: silent corruption on an old
    // drive is what this is for.
    auto h = host_.hash(full, true, cb);
    progress_tick(*j, r.size);
    if (!h) {
      if (h.error() == status::cancelled) break;
      bad.push_back({full, "unreadable"});
      continue;
    }
    if (*h == r.hash) {
      ++ok;
      std::lock_guard lock(j->m);
      ++j->prog.units_done;
      continue;
    }
    // Same size and time but different bytes: the disk changed it. A newer
    // time means someone edited it, which is not corruption.
    bad.push_back({full, st->mtime == r.mtime && st->size == r.size ? "corrupt" : "changed"});
    std::lock_guard lock(j->m);
    ++j->prog.units_failed;
  }
  std::uint32_t not_indexed = 0;
  (void)host_.walk(dir, 16, [&](const mv_addon_file_entry& e) {
    if (classify(e.name_utf8) != file_type::none && !indexed.count(e.path_utf8)) ++not_indexed;
    return !j->cancel.load();
  });

  json::writer w;
  w.begin_object();
  w.key("job").integer(static_cast<std::int64_t>(j->id));
  w.key("kind").string("verify");
  w.key("folder").string(dir);
  w.key("checked").integer(static_cast<std::int64_t>(rows.size()));
  w.key("ok").integer(ok);
  w.key("not_indexed").integer(not_indexed);
  w.key("problems").begin_array();
  for (const finding& f : bad) {
    w.begin_object();
    w.key("path").string(f.rel);
    w.key("problem").string(f.what);
    w.end_object();
  }
  w.end_array();
  w.key("cancelled").boolean(j->cancel.load());
  w.end_object();
  {
    std::lock_guard lock(j->m);
    j->verify_json = w.take();
  }
  source_lock.unlock();
  finish_job(j, j->cancel ? MV_IMPORT_JOB_CANCELLED
                          : (bad.empty() ? MV_IMPORT_JOB_DONE : MV_IMPORT_JOB_FAILED));
}

}  // namespace mv::import
