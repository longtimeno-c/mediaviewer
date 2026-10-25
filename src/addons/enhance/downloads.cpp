// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/downloads.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>

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

// What is here of a file: all of it, verified or to adopt (the fetcher
// hashes a right-size file rather than fetch it), or what its .part holds.
std::uint64_t here_of(const std::string& root, const ai::remote_file& f) {
  const fs::path p = fs_path(root + "/" + f.path);
  if (ai::fetch_ready(root, f) || sits_at(p, f.size)) return f.size;
  fs::path part = p;
  part += ".part";
  return std::min(size_of(part), f.size);
}

std::string strip_slash(std::string dir) {
  while (dir.size() > 1 && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
  return dir;
}

// The model ids enhance.json chooses, by kind (unknown kinds and non-text
// values skipped); none when it is missing. nullopt when it is there but
// does not read, or is not schema 1: then no one knows what it chose.
std::optional<std::array<std::string, 3>> read_choices(const std::string& path) {
  std::array<std::string, 3> out;
  std::error_code ec;
  const fs::path p = fs_path(path);
  if (!fs::exists(p, ec)) {
    if (ec) return std::nullopt;
    return out;
  }
  std::ifstream in(p, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream text;
  text << in.rdbuf();
  const auto doc = json::parse(text.str(), 8);
  const auto schema = doc ? doc->integer("schema") : std::nullopt;
  const json::value* models = doc ? doc->find("models") : nullptr;
  if (!schema || *schema != 1 || !models || models->k != json::kind::object) return std::nullopt;
  for (const auto& [key, v] : models->o) {
    const auto k = kind_from(key);
    if (k && v.k == json::kind::string) out[at(*k)] = v.s;
  }
  return out;
}

// A plain folder: not a link, nor (MSVC) a junction, which symlink_status
// reports as a type of its own.
bool plain_folder(const fs::path& p) {
  std::error_code ec;
  return fs::symlink_status(p, ec).type() == fs::file_type::directory;
}

// The download lock. Another window's status() holds it for an instant while
// it looks (lock.h is_held()), a download for minutes: a holder is tried
// again for about 50 ms before it counts as another window's. `held` as
// try_lock's: false when the file cannot be opened, which is not retried.
std::unique_ptr<file_lock> take_download_lock(const std::string& path, bool& held) {
  for (int tries = 1;; ++tries) {
    auto mine = try_lock(path, &held);
    if (mine || !held || tries == 6) return mine;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

// Every regular file under `dir`, as the fetcher's paths under `root`, for
// prune_fetched to keep. No link or junction is followed (prune deletes none).
// False when a name does not convert to UTF-8 (Windows allows names that do
// not): then not every file can be named, and the caller deletes nothing.
bool keep_all_under(const fs::path& root, const fs::path& dir, std::vector<ai::remote_file>& keep) {
  try {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      std::error_code e;
      const fs::file_type type = it->symlink_status(e).type();
      if (type == fs::file_type::directory) continue;  // walked into
      it.disable_recursion_pending();
      if (type != fs::file_type::regular) continue;
      const std::u8string rel = it->path().lexically_relative(root).generic_u8string();
      keep.push_back(ai::remote_file{{}, std::string(rel.begin(), rel.end()), {}, 0});
    }
    return true;
  } catch (const std::system_error&) {
    return false;
  }
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
  // The one read: what was chosen before, in this window or another. A
  // choice the catalogue no longer offers for its kind is not taken (an
  // update removed it).
  (void)adopt_from_disk();
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
  for (const ai::remote_file& f : remote_files(m)) n += here_of(root, f);
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
      st.lock_wait = s.lock_wait;
      // Stopped here, but another window may be fetching it (or another
      // model) right now: a Resume would wait for that window, so say so.
      if (st.state == download_state::stopped && held_elsewhere()) st.state = download_state::other_window;
    }
    out.push_back(std::move(st));
  }
  return out;
}

expected model_store::download(kind k, const std::string& model_id) {
  const model* m = catalog_.find(model_id);
  if (!m || !m->serves(k)) return err(status::invalid_arg);
  {
    // On disk first, so the worker here and every other window's prune
    // know the choice before anything acts on it.
    choices now;
    mv::status why = status::ok;
    const auto held = save_choice(k, m->id, now, why);
    if (!held) return err(why);
    std::lock_guard lock(m_);
    adopt_locked(now, k);
    slot& s = slots_[at(k)];
    s.chosen = m->id;
    s.outcome = download_state::none;
    s.lock_wait = false;
    if (s.running && (s.running_model != m->id || s.cancelled)) {
      run_stop_ = true;  // the old choice, or one already cancelled: this run ends
      s.cancelled = true;
    }
    const bool under_way = s.running && !s.cancelled;
    if (!under_way && !s.queued) {
      s.queued = true;
      queue_.push_back(k);
    }
    start_worker_locked();
  }
  work_cv_.notify_all();
  tell();
  return {};
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
    if (s.lock_wait) {
      s.lock_wait = false;
      s.outcome = download_state::stopped;
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
    s.lock_wait = false;
    s.outcome = download_state::none;
    queue_.push_back(k);
    start_worker_locked();
  }
  work_cv_.notify_all();
  tell();
}

expected model_store::remove(kind k) {
  {
    // Saved before the prune is asked for: the prune keeps what enhance.json
    // names, and must not read the old choice back.
    choices now;
    mv::status why = status::ok;
    const auto held = save_choice(k, {}, now, why);
    if (!held) return err(why);
    std::lock_guard lock(m_);
    adopt_locked(now, k);
    slot& s = slots_[at(k)];
    s.chosen.clear();
    s.outcome = download_state::none;
    s.lock_wait = false;
    if (s.queued) {
      s.queued = false;
      queue_.erase(std::remove(queue_.begin(), queue_.end(), k), queue_.end());
    }
    if (s.running) {
      s.cancelled = true;
      run_stop_ = true;
    }
    prune_pending_ = true;
    start_worker_locked();
  }
  work_cv_.notify_all();
  tell();
  return {};
}

void model_store::reload_choices() {
  const bool changed = adopt_from_disk();
  bool wake = false;
  {
    std::lock_guard lock(m_);
    if (prune_owed_ && !prune_pending_) {
      prune_pending_ = true;  // the worker that owes it is running
      wake = true;
    }
  }
  if (wake) work_cv_.notify_all();
  if (changed) tell();
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

bool model_store::installed() const {
  std::error_code ec;
  return fs::is_directory(fs_path(data_), ec) && (!opt_.still_installed || opt_.still_installed());
}

// Another process holds the download lock now. Never asked while this
// store's worker takes or holds it: then no other process can have it.
bool model_store::held_elsewhere() const {
  const std::unique_lock own(own_m_, std::try_to_lock);
  return own.owns_lock() && is_held(lock_path());
}

// Retried every 10 ms: another window holds it only while it reads or
// writes one small file. Never made in a data folder that has gone (the
// add-on removed with its data in another window), nor waited for when the
// file cannot be opened: status::io.
std::unique_ptr<file_lock> model_store::lock_choices(mv::status& why) const {
  std::error_code ec;
  if (!fs::is_directory(fs_path(data_), ec)) {
    why = status::io;
    return nullptr;
  }
  const auto until = std::chrono::steady_clock::now() + opt_.choices_wait;
  for (;;) {
    bool held = false;
    if (auto mine = try_lock(choices_lock_path(), &held)) return mine;
    if (!held) {
      why = status::io;
      return nullptr;
    }
    if (std::chrono::steady_clock::now() >= until) {
      why = status::busy;
      return nullptr;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

// Under enhance.lock. One temp name serves every window, since only the
// lock's holder writes it. The rename is tried a few times: Windows refuses
// to replace a file something else (a scanner, an indexer) has open.
bool model_store::write_choices(const choices& c) const {
  json::writer w;
  w.begin_object().key("schema").integer(1).key("models").begin_object();
  for (kind k : kKinds) {
    if (!c[at(k)].empty()) w.key(kind_id(k)).string(c[at(k)]);
  }
  w.end_object().end_object();
  const std::string text = w.take() + "\n";
  std::error_code ec;
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
  for (int tries = 0; tries < 5; ++tries) {
    if (tries) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    fs::rename(temp, path, ec);
    if (!ec) return true;
  }
  fs::remove(temp, ec);
  return false;
}

std::unique_ptr<file_lock> model_store::save_choice(kind k, const std::string& model_id, choices& now,
                                                    mv::status& why) {
  auto held = lock_choices(why);
  if (!held) return nullptr;
  // What the file says, every window's; one that does not read is
  // replaced by what this store knows.
  if (auto file = read_choices(choices_path())) {
    now = std::move(*file);
  } else {
    std::lock_guard lock(m_);
    now = chosen_locked();
  }
  now[at(k)] = model_id;
  if (!write_choices(now)) {
    why = status::io;
    return nullptr;
  }
  return held;
}

// enhance.json is only ever replaced by a rename, so a missing one means no
// choice (never one half written) and needs no lock to see.
bool model_store::adopt_from_disk() {
  std::error_code ec;
  if (!fs::exists(fs_path(choices_path()), ec) && !ec) {
    std::lock_guard lock(m_);
    return adopt_locked(choices{}, std::nullopt);
  }
  mv::status why = status::ok;
  const auto held = lock_choices(why);
  if (!held) return false;  // another window is saving: the next open reads it
  const auto file = read_choices(choices_path());
  if (!file) return false;
  std::lock_guard lock(m_);
  return adopt_locked(*file, std::nullopt);
}

// The kinds this store is not running or queueing take the file's choice;
// one this catalogue does not offer for the kind reads as none here (the
// file keeps it). True when a kind changed.
bool model_store::adopt_locked(const choices& c, std::optional<kind> except) {
  bool changed = false;
  for (kind k : kKinds) {
    slot& s = slots_[at(k)];
    if ((except && *except == k) || s.queued || s.running) continue;
    const model* m = catalog_.find(c[at(k)]);
    std::string id = m && m->serves(k) ? m->id : std::string();
    if (id == s.chosen) continue;
    s.chosen = std::move(id);
    s.outcome = download_state::none;
    s.lock_wait = false;  // what waited was this store's old choice
    changed = true;
  }
  return changed;
}

model_store::choices model_store::chosen_locked() const {
  choices out;
  for (kind k : kKinds) out[at(k)] = slots_[at(k)].chosen;
  return out;
}

void model_store::work() {
  if (opt_.background) enter_background();
  std::unique_lock lock(m_);
  for (;;) {
    const auto due = [this] { return quit_ || !queue_.empty() || prune_pending_; };
    const bool lock_waits = std::any_of(slots_.begin(), slots_.end(), [](const slot& s) { return s.lock_wait; });
    if (!prune_owed_ && !lock_waits) {
      work_cv_.wait(lock, due);
    } else if (!work_cv_.wait_for(lock, lock_waits ? opt_.lock_retry : opt_.prune_retry, due)) {
      if (!lock_waits) {
        prune_pending_ = true;  // owed: try again now and then until it runs
      } else {
        // Has the other window let go? Then what waited for it runs, in
        // kind order, and an owed prune with it.
        lock.unlock();
        bool free = false;
        {
          const std::lock_guard own(own_m_);
          free = !is_held(lock_path());
        }
        lock.lock();
        for (kind w : kKinds) {
          slot& s = slots_[at(w)];
          if (!free || !s.lock_wait) continue;
          s.lock_wait = false;
          if (s.queued || s.running) continue;
          s.queued = true;
          s.outcome = download_state::none;
          queue_.push_back(w);
        }
        if (free && prune_owed_) prune_pending_ = true;
      }
    }
    if (quit_) break;
    if (queue_.empty() && !prune_pending_) continue;  // only looked: still held
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
    const bool cancelled = t.cancelled;
    t.running = false;
    t.cancelled = false;
    // A kind queued again or chosen anew meanwhile shows that instead.
    if (!t.queued && t.chosen == id) {
      t.outcome = end.state;
      t.need = end.need;
      t.have = end.have;
      t.reason = end.reason;
      // Refused for the lock alone: it runs once the other window lets go,
      // unless it was stopped meanwhile.
      if (end.state == download_state::other_window) {
        t.lock_wait = !cancelled;
        if (cancelled) t.outcome = download_state::stopped;
      }
    }
    if (prune_owed_) prune_pending_ = true;  // the run has let go of the download lock
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
  // 0. Still installed: after a Remove in another window this one stops,
  // and makes none of the add-on's folders again.
  if (!m || run_stop_.load() || !installed()) return e;
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
  // 3. One process at a time; status() does not probe the lock meanwhile.
  // A lock file that cannot be opened is no other window's: stopped, and
  // nothing waits for it.
  std::error_code ec;
  fs::create_directories(fs_path(data_ + "/cache"), ec);
  const std::lock_guard own(own_m_);
  bool held = false;
  const auto mine = take_download_lock(lock_path(), held);
  if (!mine) {
    e.state = held ? download_state::other_window : download_state::stopped;
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
  const std::string root = weights_root();
  ai::fetcher fetch(root, *net, std::move(fo));
  const std::vector<ai::remote_file> files = remote_files(*m);
  const std::uint64_t total = m->download_bytes();
  // What was here of each file as the run began, as status() counted it
  // while queued: it counts until the fetcher's own figure for the file
  // passes it, and a later file's until the loop reaches it, so the bar
  // never goes back.
  std::vector<std::uint64_t> found;
  std::uint64_t ahead = 0;  // found, of the files not reached yet
  for (const ai::remote_file& file : files) {
    found.push_back(here_of(root, file));
    ahead += found.back();
  }
  // A file at a time, the add-on checked before each: the fetcher makes the
  // file's folder.
  std::uint64_t before = 0;  // the bytes of the files already through
  expected r{};
  for (std::size_t i = 0; i < files.size(); ++i) {
    if (!installed()) {
      r = err(status::cancelled);
      break;
    }
    ahead -= found[i];
    const std::uint64_t others = before + ahead;
    r = fetch.run(std::span<const ai::remote_file>(&files[i], 1), run_stop_,
                  [this, k, others, start = found[i], total](const ai::fetch_progress& p) {
                    on_progress(k, p, others, start, total);
                  });
    if (!r) break;
    before += files[i].size;
  }
  if (r) {
    e.state = download_state::ready;
    prune_unchosen(true, id);  // under the lock still held: the old choice's files go now
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

void model_store::on_progress(kind k, const ai::fetch_progress& p, std::uint64_t others, std::uint64_t start,
                              std::uint64_t total) {
  // While bytes move, at most once a second: removed in another window, the
  // run stops at the fetcher's next chunk.
  const auto t = std::chrono::steady_clock::now();
  if (t - last_check_ >= std::chrono::seconds(1)) {
    last_check_ = t;
    if (!installed()) run_stop_ = true;
  }
  bool now_tell = false;
  {
    std::lock_guard lock(m_);
    slot& s = slots_[at(k)];
    s.done = others + std::max(p.done_bytes, start);
    s.total = total;
    s.retry_in_s = p.retry_in_s;
    // gone / failed / done end the run: run_one reports those.
    if (p.state == ai::fetch_state::downloading || p.state == ai::fetch_state::waiting) {
      const download_state st =
          p.state == ai::fetch_state::waiting ? download_state::waiting : download_state::downloading;
      now_tell = st != s.live || t - last_tell_ >= std::chrono::milliseconds(250);
      s.live = st;
      if (now_tell) last_tell_ = t;
    }
    now_tell = now_tell && !quit_;
  }
  if (now_tell) tell();
}

void model_store::prune_unchosen(bool lock_held, const std::string& fetched) {
  const bool ran = prune_now(lock_held, fetched);
  std::lock_guard lock(m_);
  prune_owed_ = !ran;
}

// Deletes what no kind's chosen model needs: what enhance.json names now
// (every window's choices), this store's and the model just fetched. A
// choice this version does not know keeps its folder whole. Only with the
// download lock (never under another window's download) and enhance.lock
// (no window chooses meanwhile): false, with nothing deleted, when either
// cannot be had, and the prune is owed. Empty folders go too. A weights
// folder that is a link or a junction is left alone, and no link or junction
// in it is followed or removed.
bool model_store::prune_now(bool lock_held, const std::string& fetched) {
  const fs::path root = fs_path(weights_root());
  if (!plain_folder(fs_path(data_ + "/cache")) || !plain_folder(root)) return true;
  // run_one prunes under the lock it holds; otherwise the worker takes it
  // (status() does not probe it meanwhile).
  std::unique_lock own(own_m_, std::defer_lock);
  std::unique_ptr<file_lock> mine;
  if (!lock_held) {
    own.lock();
    bool held = false;
    if (!(mine = take_download_lock(lock_path(), held))) return false;
  }
  mv::status why = status::ok;
  const auto choosing = lock_choices(why);
  if (!choosing) return false;
  const auto file = read_choices(choices_path());
  if (!file) return true;  // what other windows chose is not known: nothing goes (the next save rewrites it)
  std::set<std::string> ids(file->begin(), file->end());
  if (!fetched.empty()) ids.insert(fetched);
  {
    std::lock_guard lock(m_);
    for (const slot& s : slots_) ids.insert(s.chosen);
  }
  ids.erase(std::string());
  std::vector<ai::remote_file> keep;
  std::vector<fs::path> whole;  // the folders of choices this version does not know
  for (const std::string& id : ids) {
    if (const model* m = catalog_.find(id)) {
      auto files = remote_files(*m);
      keep.insert(keep.end(), files.begin(), files.end());
      continue;
    }
    // A choice this version does not know (another version's in another
    // window, or one an update dropped): which of its files it needs is not
    // known here, so everything under models/<id>/ stays, and nothing else
    // for it (a catalogue gives a model only the files under its own
    // folder, and no id it accepts names another).
    if (!model_id_ok(id) || !plain_folder(root / fs_path(id))) continue;
    whole.push_back(root / fs_path(id));
    if (!keep_all_under(root, whole.back(), keep)) return true;  // nothing goes
  }
  ai::prune_fetched(weights_root(), keep);
  std::error_code ec;
  std::vector<fs::path> dirs;
  for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    std::error_code e;
    if (it->symlink_status(e).type() != fs::file_type::directory) {
      it.disable_recursion_pending();  // a file, or a link or junction: never entered
    } else if (std::find(whole.begin(), whole.end(), it->path()) != whole.end()) {
      it.disable_recursion_pending();  // an unknown choice's folder, kept as it is
    } else {
      dirs.push_back(it->path());
    }
  }
  // Deepest first; a folder that still holds something is left (remove fails).
  std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
    return a.native().size() > b.native().size();
  });
  for (const fs::path& d : dirs) fs::remove(d, ec);
  return true;
}

}  // namespace mv::enhance
