// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/downloads.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "addons/enhance/lock.h"
#include "core/json.h"

namespace mv::enhance {
namespace {

namespace fs = std::filesystem;

fs::path fs_path(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

std::size_t at(kind k) noexcept { return static_cast<std::size_t>(k); }

std::uint64_t size_of(const fs::path& p) {
  std::error_code ec;
  const auto n = fs::file_size(p, ec);
  return ec ? 0 : static_cast<std::uint64_t>(n);
}

// A regular file (not a link) of exactly `size` bytes: one the fetcher would
// hash and adopt rather than fetch.
bool sits_at(const fs::path& p, std::uint64_t size) {
  std::error_code ec;
  return fs::is_regular_file(fs::symlink_status(p, ec)) && size_of(p) == size;
}

std::string strip_slash(std::string dir) {
  while (dir.size() > 1 && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
  return dir;
}

// The model ids enhance.json chooses, by kind (unknown kinds and non-text
// values skipped). Empty when it is missing or not schema 1.
std::array<std::string, 3> read_choices(const std::string& path) {
  std::array<std::string, 3> out;
  std::ifstream in(fs_path(path), std::ios::binary);
  if (!in) return out;
  std::ostringstream text;
  text << in.rdbuf();
  const auto doc = json::parse(text.str(), 8);
  const auto schema = doc ? doc->integer("schema") : std::nullopt;
  const json::value* models = doc ? doc->find("models") : nullptr;
  if (!schema || *schema != 1 || !models || models->k != json::kind::object) return out;
  for (const auto& [key, v] : models->o) {
    const auto k = kind_from(key);
    if (k && v.k == json::kind::string) out[at(*k)] = v.s;
  }
  return out;
}

}  // namespace

const char* state_id(download_state s) noexcept {
  switch (s) {
    case download_state::none: return "none";
    case download_state::ready: return "ready";
    case download_state::queued: return "queued";
    case download_state::downloading: return "downloading";
    case download_state::waiting: return "waiting";
    case download_state::stopped: return "stopped";
    case download_state::gone: return "gone";
    case download_state::failed: return "failed";
    case download_state::refused: return "refused";
    case download_state::no_space: return "no_space";
    case download_state::other_window: return "other_window";
    case download_state::unsupported: return "unsupported";
    case download_state::unfit: return "unfit";
  }
  return "none";
}

model_store::model_store(std::string data_dir, catalog models, transport_factory transport,
                         std::function<machine()> probe, std::function<void()> changed, store_options options)
    : data_(strip_slash(std::move(data_dir))),
      catalog_(std::move(models)),
      transport_(std::move(transport)),
      probe_(std::move(probe)),
      changed_(std::move(changed)),
      opt_(std::move(options)) {
  // The one read: what was chosen before. A choice the catalogue no longer
  // offers for its kind is dropped (an update removed it).
  const std::array<std::string, 3> saved = read_choices(choices_path());
  for (kind k : kKinds) {
    const model* m = catalog_.find(saved[at(k)]);
    if (m && m->serves(k)) slots_[at(k)].chosen = m->id;
  }
}

model_store::~model_store() {
  {
    std::lock_guard lock(m_);
    quit_ = true;
    run_stop_ = true;
  }
  work_cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

std::vector<ai::remote_file> model_store::remote_files(const model& m) {
  std::vector<ai::remote_file> out;
  constexpr std::string_view kPrefix = "models/";
  for (const pinned& p : m.files) {
    std::string path = p.path;
    if (path.compare(0, kPrefix.size(), kPrefix) == 0) path.erase(0, kPrefix.size());
    out.push_back(ai::remote_file{p.url, std::move(path), p.sha256, p.size});
  }
  return out;
}

std::uint64_t model_store::here_bytes(const model& m) const {
  const std::string root = weights_root();
  std::uint64_t n = 0;
  for (const ai::remote_file& f : remote_files(m)) {
    const fs::path p = fs_path(root + "/" + f.path);
    if (ai::fetch_ready(root, f) || sits_at(p, f.size)) {
      n += f.size;
    } else {
      fs::path part = p;
      part += ".part";
      n += std::min(size_of(part), f.size);
    }
  }
  return n;
}

std::uint64_t model_store::still_to_fetch(const model& m) const {
  const std::uint64_t total = m.download_bytes();
  const std::uint64_t here = here_bytes(m);
  return total > here ? total - here : 0;
}

bool model_store::all_ready(const model& m) const {
  const std::string root = weights_root();
  const auto files = remote_files(m);
  return std::all_of(files.begin(), files.end(), [&](const ai::remote_file& f) { return ai::fetch_ready(root, f); });
}

bool model_store::ready(kind k) const {
  const model* m = catalog_.find(chosen(k));
  return m && all_ready(*m);
}

std::string model_store::chosen(kind k) const {
  std::lock_guard lock(m_);
  return slots_[at(k)].chosen;
}

std::vector<kind_status> model_store::status() const {
  std::array<slot, 3> snap;
  {
    std::lock_guard lock(m_);
    snap = slots_;
  }
  std::vector<kind_status> out;
  for (kind k : kKinds) {
    const slot& s = snap[at(k)];
    kind_status st;
    st.k = k;
    st.model = s.chosen;
    const model* m = catalog_.find(s.chosen);
    if (!m) {
      out.push_back(std::move(st));
      continue;
    }
    st.total_bytes = m->download_bytes();
    if (s.queued) {
      st.state = download_state::queued;
      st.done_bytes = here_bytes(*m);
    } else if (s.running && !s.cancelled && s.running_model == s.chosen) {
      st.state = s.live;  // queued while the gates run, then downloading / waiting
      st.done_bytes = s.live == download_state::queued ? here_bytes(*m) : s.done;
      st.retry_in_s = s.retry_in_s;
    } else if (all_ready(*m)) {
      st.state = download_state::ready;
      st.done_bytes = st.total_bytes;
    } else {
      // Nothing running here: how the last run ended, or after a restart
      // (or a finished run whose files have since gone) stopped.
      const bool ended = s.outcome != download_state::none && s.outcome != download_state::ready;
      st.state = ended ? s.outcome : download_state::stopped;
      st.done_bytes = here_bytes(*m);
      st.need = s.need;
      st.have = s.have;
      st.reason = s.reason;
    }
    out.push_back(std::move(st));
  }
  return out;
}

expected model_store::download(kind k, const std::string& model_id) {
  const model* m = catalog_.find(model_id);
  if (!m || !m->serves(k)) return err(status::invalid_arg);
  std::string text;
  std::uint64_t gen = 0;
  {
    std::lock_guard lock(m_);
    slot& s = slots_[at(k)];
    s.chosen = m->id;
    s.outcome = download_state::none;
    if (s.running && (s.running_model != m->id || s.cancelled)) {
      run_stop_ = true;  // the old choice, or one already cancelled: this run ends
      s.cancelled = true;
    }
    const bool under_way = s.running && !s.cancelled;
    if (!under_way && !s.queued) {
      s.queued = true;
      queue_.push_back(k);
    }
    text = choices_json_locked();
    gen = ++save_gen_;
    start_worker_locked();
  }
  work_cv_.notify_all();
  const bool saved = save(text, gen);
  tell();
  return saved ? expected{} : expected{err(status::io)};
}

void model_store::cancel(kind k) {
  {
    std::lock_guard lock(m_);
    slot& s = slots_[at(k)];
    if (s.queued) {
      s.queued = false;
      queue_.erase(std::remove(queue_.begin(), queue_.end(), k), queue_.end());
      s.outcome = download_state::stopped;
    }
    if (s.running) {
      s.cancelled = true;
      run_stop_ = true;
    }
  }
  idle_cv_.notify_all();
  tell();
}

void model_store::resume(kind k) {
  {
    std::lock_guard lock(m_);
    slot& s = slots_[at(k)];
    if (s.chosen.empty() || s.queued || (s.running && !s.cancelled && s.running_model == s.chosen)) return;
    s.queued = true;
    s.outcome = download_state::none;
    queue_.push_back(k);
    start_worker_locked();
  }
  work_cv_.notify_all();
  tell();
}

void model_store::remove(kind k) {
  std::string text;
  std::uint64_t gen = 0;
  {
    std::lock_guard lock(m_);
    slot& s = slots_[at(k)];
    s.chosen.clear();
    s.outcome = download_state::none;
    if (s.queued) {
      s.queued = false;
      queue_.erase(std::remove(queue_.begin(), queue_.end(), k), queue_.end());
    }
    if (s.running) {
      s.cancelled = true;
      run_stop_ = true;
    }
    text = choices_json_locked();
    gen = ++save_gen_;
  }
  // Saved before the prune is asked for: the prune keeps what enhance.json
  // names, and must not read the old choice back.
  (void)save(text, gen);
  {
    std::lock_guard lock(m_);
    prune_pending_ = true;
    start_worker_locked();
  }
  work_cv_.notify_all();
  tell();
}

bool model_store::wait_idle(std::chrono::milliseconds timeout) {
  std::unique_lock lock(m_);
  return idle_cv_.wait_for(lock, timeout, [this] { return idle_locked(); });
}

bool model_store::idle_locked() const noexcept {
  return queue_.empty() && !prune_pending_ && !pruning_ &&
         std::none_of(slots_.begin(), slots_.end(), [](const slot& s) { return s.running; });
}

void model_store::start_worker_locked() {
  if (!worker_.joinable()) worker_ = std::thread([this] { work(); });
}

void model_store::tell() const {
  if (changed_) changed_();
}

std::string model_store::choices_json_locked() const {
  json::writer w;
  w.begin_object().key("schema").integer(1).key("models").begin_object();
  for (kind k : kKinds) {
    if (!slots_[at(k)].chosen.empty()) w.key(kind_id(k)).string(slots_[at(k)].chosen);
  }
  w.end_object().end_object();
  return w.take() + "\n";
}

// The newest snapshot wins: one taken earlier and written later is dropped.
bool model_store::save(const std::string& text, std::uint64_t gen) {
  std::lock_guard lock(save_m_);
  if (gen <= saved_gen_) return true;
  std::error_code ec;
  fs::create_directories(fs_path(data_), ec);
  const fs::path path = fs_path(choices_path());
  fs::path temp = path;
  temp += ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    if (out.fail()) {
      fs::remove(temp, ec);
      return false;
    }
  }
  fs::rename(temp, path, ec);
  if (ec) {
    fs::remove(temp, ec);
    return false;
  }
  saved_gen_ = gen;
  return true;
}

void model_store::work() {
  if (opt_.background) enter_background();
  std::unique_lock lock(m_);
  for (;;) {
    work_cv_.wait(lock, [this] { return quit_ || !queue_.empty() || prune_pending_; });
    if (quit_) break;
    if (prune_pending_) {
      prune_pending_ = false;
      pruning_ = true;
      lock.unlock();
      prune_unchosen(false);
      lock.lock();
      pruning_ = false;
      idle_cv_.notify_all();
      continue;
    }
    const kind k = queue_.front();
    queue_.pop_front();
    slot& s = slots_[at(k)];
    s.queued = false;
    s.running = true;
    s.cancelled = false;
    s.running_model = s.chosen;
    s.live = download_state::queued;
    s.done = 0;
    s.total = 0;
    s.retry_in_s = 0;
    run_stop_ = false;
    const std::string id = s.chosen;
    lock.unlock();
    tell();

    const run_end end = run_one(k, id);

    lock.lock();
    slot& t = slots_[at(k)];
    t.running = false;
    t.cancelled = false;
    // A kind queued again or chosen anew meanwhile shows that instead.
    if (!t.queued && t.chosen == id) {
      t.outcome = end.state;
      t.need = end.need;
      t.have = end.have;
      t.reason = end.reason;
    }
    idle_cv_.notify_all();
    if (quit_) break;
    lock.unlock();
    tell();
    lock.lock();
  }
}

model_store::run_end model_store::run_one(kind k, const std::string& id) {
  run_end e;
  const model* m = catalog_.find(id);
  if (!m || run_stop_.load()) return e;
  // 1. The cap: a list naming a file over it is never requested.
  if (std::any_of(m->files.begin(), m->files.end(), [](const pinned& p) { return p.size > kMaxWeightsFile; })) {
    e.state = download_state::refused;
    e.reason = why::too_large;
    return e;
  }
  // 2. The model fits this machine, and its bytes fit the disk.
  const machine here = probe_ ? probe_() : machine{};
  const fit f = offer(*m, catalog_.of(k), here, still_to_fetch(*m));
  if (!f.ok()) {
    e.state = f.reason == why::disk        ? download_state::no_space
              : f.reason == why::too_large ? download_state::refused
                                           : download_state::unfit;
    e.need = f.need;
    e.have = f.have;
    e.reason = f.reason;
    return e;
  }
  // 3. One process at a time.
  std::error_code ec;
  fs::create_directories(fs_path(data_ + "/cache"), ec);
  const auto held = try_lock(lock_path());
  if (!held) {
    e.state = download_state::other_window;
    return e;
  }
  // 4. A transport.
  std::unique_ptr<ai::http_transport> net = transport_ ? transport_() : nullptr;
  if (!net) {
    e.state = download_state::unsupported;
    return e;
  }
  ai::fetch_options fo = opt_.fetch;
  fo.max_file_bytes = kMaxWeightsFile;
  ai::fetcher fetch(weights_root(), *net, std::move(fo));
  const std::vector<ai::remote_file> files = remote_files(*m);
  const expected r = fetch.run(files, run_stop_, [this, k](const ai::fetch_progress& p) { on_progress(k, p); });
  if (r) {
    e.state = download_state::ready;
    prune_unchosen(true);  // under the lock still held: the old choice's files go now
    return e;
  }
  switch (r.error()) {
    case status::not_found: e.state = download_state::gone; break;
    case status::corrupt: e.state = download_state::failed; break;
    case status::invalid_arg:
      e.state = download_state::refused;
      e.reason = why::too_large;
      break;
    default: e.state = download_state::stopped; break;  // cancelled, or the disk said no
  }
  return e;
}

void model_store::on_progress(kind k, const ai::fetch_progress& p) {
  bool now_tell = false;
  {
    std::lock_guard lock(m_);
    slot& s = slots_[at(k)];
    s.done = p.done_bytes;
    s.total = p.total_bytes;
    s.retry_in_s = p.retry_in_s;
    // gone / failed / done end the run: run_one reports those.
    if (p.state == ai::fetch_state::downloading || p.state == ai::fetch_state::waiting) {
      const download_state st =
          p.state == ai::fetch_state::waiting ? download_state::waiting : download_state::downloading;
      const auto t = std::chrono::steady_clock::now();
      now_tell = st != s.live || t - last_tell_ >= std::chrono::milliseconds(250);
      s.live = st;
      if (now_tell) last_tell_ = t;
    }
    now_tell = now_tell && !quit_;
  }
  if (now_tell) tell();
}

// Deletes what no kind's chosen model needs: this store's choices and the
// ones enhance.json names now (another window may have chosen since). Only
// with the lock: never under another window's download. Empty folders go too.
void model_store::prune_unchosen(bool lock_held) {
  std::error_code ec;
  if (!fs::exists(fs_path(weights_root()), ec)) return;
  // run_one prunes under the lock it holds; after remove() the worker takes
  // it, and leaves the files to the window downloading when it cannot.
  std::unique_ptr<file_lock> mine;
  if (!lock_held && !(mine = try_lock(lock_path()))) return;
  std::set<std::string> ids;
  {
    std::lock_guard lock(m_);
    for (const slot& s : slots_) {
      if (!s.chosen.empty()) ids.insert(s.chosen);
    }
  }
  for (const std::string& id : read_choices(choices_path())) {
    if (!id.empty()) ids.insert(id);
  }
  std::vector<ai::remote_file> keep;
  for (const std::string& id : ids) {
    if (const model* m = catalog_.find(id)) {
      auto files = remote_files(*m);
      keep.insert(keep.end(), files.begin(), files.end());
    }
  }
  ai::prune_fetched(weights_root(), keep);
  const fs::path root = fs_path(weights_root());
  std::vector<fs::path> dirs;
  for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it->is_symlink(ec)) {
      it.disable_recursion_pending();
      continue;
    }
    if (it->is_directory(ec)) dirs.push_back(it->path());
  }
  // Deepest first; a folder that still holds something is left (remove fails).
  std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
    return a.native().size() > b.native().size();
  });
  for (const fs::path& d : dirs) fs::remove(d, ec);
}

}  // namespace mv::enhance
