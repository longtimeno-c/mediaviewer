// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/sharepoint/engine.h"

#include <sodium.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <unordered_map>

#include "core/json.h"
#include "io/file_port.h"

namespace mv::sharepoint {

namespace {

constexpr auto kPostEvery = std::chrono::milliseconds(100);
// The person has this long to answer an offer (or confirm the code); then it
// is called off.
constexpr auto kAskFor = std::chrono::milliseconds(kAskMs);
// Busy refusals in flight at once, at most; past it a link is just closed.
constexpr int kMaxHelpers = 4;
// A receive keeps this much free beside the files.
constexpr std::uint64_t kRoomMargin = 64ull * 1024 * 1024;

std::string random_id() {
  std::uint8_t b[8];
  randombytes_buf(b, sizeof b);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string s;
  for (std::uint8_t x : b) {
    s.push_back(kHex[x >> 4]);
    s.push_back(kHex[x & 15]);
  }
  return s;
}

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// Names a folder send leaves out at any depth (walk_files already skips
// hidden and system entries): housekeeping of NAS boxes, Windows and macOS,
// and our own temporaries.
bool housekeeping(std::string_view part) {
  if (part.empty() || part.front() == '@' || part.front() == '#' || part.front() == '.') return true;
  const std::string l = lower(part);
  if (l == "$recycle.bin" || l == "system volume information" || l == "lost+found" || l == "__macosx" ||
      l == "thumbs.db" || l == "desktop.ini") {
    return true;
  }
  const auto tmp = l.rfind(".mvtmp");
  return tmp != std::string::npos && tmp + 6 <= l.size() &&
         std::all_of(l.begin() + static_cast<std::ptrdiff_t>(tmp + 6), l.end(),
                     [](char c) { return c >= '0' && c <= '9'; });
}

bool skipped_relative(std::string_view rel) {
  while (!rel.empty()) {
    const auto slash = rel.find('/');
    if (housekeeping(rel.substr(0, slash))) return true;
    if (slash == std::string_view::npos) break;
    rel.remove_prefix(slash + 1);
  }
  return false;
}

std::string stem_of(std::string_view name) {
  const auto dot = name.rfind('.');
  return std::string(dot == std::string_view::npos || dot == 0 ? name : name.substr(0, dot));
}

std::string plural(std::uint32_t n, const char* one, const char* many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

}  // namespace

engine::engine(engine_host& host, options opts) : host_(host), opts_(std::move(opts)) {
  (void)sodium_init();
  id_ = random_id();
  if (opts_.my_name.empty()) opts_.my_name = net::machine_name();
  v_.my_name = clean_name(opts_.my_name);
  // What was kept from the last run (#442). A saved folder that is gone
  // falls back to the default; it is not forgotten until another is picked.
  const prefs kept = opts_.initial ? *opts_.initial : load_prefs(opts_.prefs_path).value_or(prefs{});
  v_.destination = host_.default_destination();
  if (!kept.destination.empty()) {
    if (auto st = io::stat_path(kept.destination); st && st->is_directory) v_.destination = kept.destination;
  }
  control_thread_ = std::thread([this] { control_loop(); });
}

engine::~engine() {
  stopping_.store(true);
  cancel_transfer();  // the live channel and every handshake end
  {
    std::lock_guard lock(m_);
    decided_.notify_all();
  }
  // The transfer and the busy refusals first, while the bearers they may be
  // using still exist; then the control thread, then presence.
  {
    std::lock_guard lock(threads_m_);
    if (transfer_thread_.joinable()) transfer_thread_.join();
    for (auto& h : helpers_) {
      if (h.thread.joinable()) h.thread.join();
    }
    helpers_.clear();
  }
  {
    std::unique_lock lock(files_m_);
    files_cv_.notify_all();
    std::thread reader = std::move(files_thread_);
    lock.unlock();
    if (reader.joinable()) reader.join();
  }
  {
    std::lock_guard lock(control_m_);
    control_cv_.notify_all();
  }
  if (control_thread_.joinable()) control_thread_.join();
  stop_presence();
  if (save_wanted_) save_now();  // a change the control thread had not written yet
}

// ---- posting and the control thread ------------------------------------------------

void engine::post() {
  std::lock_guard serial(post_m_);
  std::string tree;
  {
    std::lock_guard lock(m_);
    tree = build_tree(v_);
  }
  last_post_ = std::chrono::steady_clock::now();
  host_.post_tree(tree);
}

void engine::post_throttled() {
  std::lock_guard lock(control_m_);
  if (!post_pending_) {
    post_pending_ = true;
    post_due_ = std::max(std::chrono::steady_clock::now(), last_post_.load() + kPostEvery);
    control_cv_.notify_all();
  }
}

void engine::request_reconcile() {
  std::lock_guard lock(control_m_);
  reconcile_wanted_ = true;
  control_cv_.notify_all();
}

void engine::request_save() {
  if (opts_.prefs_path.empty()) return;
  std::lock_guard lock(control_m_);
  save_wanted_ = true;
  control_cv_.notify_all();
}

void engine::save_now() {
  prefs p;
  {
    std::lock_guard lock(m_);
    p.receiving = v_.receiving;
    p.destination = v_.destination == host_.default_destination() ? std::string() : v_.destination;
  }
  {
    std::lock_guard lock(control_m_);
    save_wanted_ = false;
  }
  if (!save_prefs(opts_.prefs_path, p)) host_.log(1, "sharepoint: could not keep the settings");
}

void engine::control_loop() {
  std::unique_lock lock(control_m_);
  while (!stopping_.load()) {
    if (post_pending_) {
      control_cv_.wait_until(lock, post_due_, [&] { return stopping_.load() || reconcile_wanted_ || save_wanted_; });
    } else {
      control_cv_.wait(lock, [&] { return stopping_.load() || reconcile_wanted_ || post_pending_ || save_wanted_; });
    }
    if (stopping_.load()) break;
    if (save_wanted_) {
      lock.unlock();
      save_now();
      lock.lock();
    }
    if (reconcile_wanted_) {
      reconcile_wanted_ = false;
      lock.unlock();
      reconcile();
      lock.lock();
    }
    if (post_pending_ && std::chrono::steady_clock::now() >= post_due_) {
      post_pending_ = false;
      lock.unlock();
      post();
      lock.lock();
    }
  }
}

bool engine::visible_locked() const noexcept { return screen_open_ || v_.receiving; }

void engine::reconcile() {
  bool want = false;
  {
    std::lock_guard lock(m_);
    // A send waiting for its peer on Wi-Fi keeps listening, window or not.
    want = !stopping_.load() && (visible_locked() || transfer_running_ || !waiting_peer_.empty());
  }
  if (want && !presence_on_) {
    start_presence();
  } else if (!want && presence_on_) {
    stop_presence();
  }
}

// ---- presence -----------------------------------------------------------------------

void engine::start_presence() {
  std::lock_guard p(presence_m_);
  if (presence_on_) return;
  presence_on_ = true;
  bool wifi = false;
  if (opts_.offline) {
    {
      std::lock_guard lock(m_);
      v_.wifi_up = false;
      v_.bluetooth_note = "Offline.";
    }
    post();
    return;
  }
  if (auto l = net::listen(opts_.loopback_only)) {
    listener_ = std::move(*l);
    accept_thread_ = std::thread([this] { accept_loop(); });
    discovery_ = opts_.make_discovery ? opts_.make_discovery() : wifi_discovery();
    beacon self;
    self.id = id_;
    self.name = opts_.my_name;
    self.port = listener_->port();
    wifi = discovery_ && discovery_->start(
                             self, [this](const beacon& b, const std::string& a) { on_beacon(b, a); },
                             [this] { on_tick(); });
    if (!wifi) host_.log(1, "sharepoint: discovery could not start");
  } else {
    host_.log(1, "sharepoint: could not listen");
  }
  std::string bt_note;
  if (opts_.use_bluetooth) {
    std::unique_ptr<bluetooth> bt = opts_.make_bluetooth ? opts_.make_bluetooth() : make_bluetooth();
    if (!bt) {
      bt_note = "Bluetooth is not available here.";
    } else {
      std::string why;
      if (bt->start(
              id_, opts_.my_name, [this](const bt_peer& peer) { on_bt_peer(peer); },
              [this](std::unique_ptr<link> l) { on_incoming(std::move(l)); }, why)) {
        bluetooth_ = std::shared_ptr<bluetooth>(std::move(bt));
      } else {
        bt_note = why.empty() ? std::string("Bluetooth is off.") : why;
      }
    }
  } else {
    bt_note = "Bluetooth is not used.";
  }
  {
    std::lock_guard lock(m_);
    v_.wifi_up = wifi;
    v_.bluetooth_note = bt_note;
  }
  post();
}

void engine::stop_presence() {
  std::lock_guard p(presence_m_);
  if (!presence_on_) return;
  presence_on_ = false;
  if (discovery_) discovery_->stop();
  if (listener_) listener_->shutdown();
  if (accept_thread_.joinable()) accept_thread_.join();
  listener_.reset();
  discovery_.reset();
  if (bluetooth_) bluetooth_->stop();
  bluetooth_.reset();
  {
    std::lock_guard lock(m_);
    peers_.clear();
    v_.peers.clear();
    v_.selected_peer.clear();
    v_.wifi_up = false;
  }
  if (!stopping_.load()) post();
}

void engine::refresh_peers_locked() {
  std::vector<view::peer_row> rows;
  const auto now = std::chrono::steady_clock::now();
  const auto fresh = [&](std::chrono::steady_clock::time_point t) {
    return t.time_since_epoch().count() != 0 && now - t < std::chrono::milliseconds(kPeerForgetMs);
  };
  for (const auto& [id, p] : peers_) {
    const bool wifi = p.wifi && fresh(p.wifi_seen);
    const bool bt = !p.bt_handle.empty() && fresh(p.bt_seen);
    if (!wifi && !bt) continue;
    rows.push_back({id, p.name, wifi && bt ? "Wi-Fi and Bluetooth" : wifi ? "Wi-Fi" : "Bluetooth", wifi, bt});
  }
  std::sort(rows.begin(), rows.end(), [](const view::peer_row& a, const view::peer_row& b) {
    return a.name != b.name ? a.name < b.name : a.id < b.id;
  });
  const bool same = rows.size() == v_.peers.size() &&
                    std::equal(rows.begin(), rows.end(), v_.peers.begin(), [](const auto& a, const auto& b) {
                      return a.id == b.id && a.name == b.name && a.via == b.via;
                    });
  if (same) return;
  v_.peers = std::move(rows);
  if (std::none_of(v_.peers.begin(), v_.peers.end(), [&](const auto& r) { return r.id == v_.selected_peer; })) {
    v_.selected_peer.clear();
  }
  peers_changed_ = true;
}

void engine::on_beacon(const beacon& b, const std::string& address) {
  if (b.id == id_) return;  // our own, looped back
  bool changed = false;
  bool ready = false;
  {
    std::lock_guard lock(m_);
    const auto now = std::chrono::steady_clock::now();
    auto it = peers_.find(b.id);
    if (it == peers_.end()) {
      if (peers_.size() >= kMaxPeers) return;
      it = peers_.emplace(b.id, peer{}).first;
    } else if (it->second.wifi && now - it->second.wifi_seen < std::chrono::milliseconds(kPeerForgetMs) &&
               (it->second.wifi->address != address || it->second.wifi->port != b.port)) {
      return;  // a fresh peer keeps its address: another machine cannot take its id over
    }
    peer& p = it->second;
    p.id = b.id;
    p.name = b.name;
    p.wifi = net::endpoint{address, b.port};
    p.wifi_seen = std::chrono::steady_clock::now();
    refresh_peers_locked();
    changed = std::exchange(peers_changed_, false);
    ready = b.id == waiting_peer_ && !transfer_running_;
  }
  if (ready) {
    start_waiting_send();  // the peer the big set waited for is on Wi-Fi now
  } else if (changed) {
    post_throttled();
  }
}

void engine::on_tick() {
  bool changed = false;
  {
    std::lock_guard lock(m_);
    const auto now = std::chrono::steady_clock::now();
    for (auto it = peers_.begin(); it != peers_.end();) {
      const bool old_wifi = now - it->second.wifi_seen > std::chrono::milliseconds(kPeerForgetMs);
      const bool old_bt = now - it->second.bt_seen > std::chrono::milliseconds(kPeerForgetMs);
      it = old_wifi && old_bt ? peers_.erase(it) : std::next(it);
    }
    refresh_peers_locked();
    changed = std::exchange(peers_changed_, false);
  }
  if (changed) post_throttled();
}

void engine::on_bt_peer(const bt_peer& b) {
  if (b.id == id_ || !valid_id(b.id)) return;
  bool changed = false;
  {
    std::lock_guard lock(m_);
    const auto now = std::chrono::steady_clock::now();
    auto it = peers_.find(b.id);
    if (it == peers_.end()) {
      if (peers_.size() >= kMaxPeers) return;
      it = peers_.emplace(b.id, peer{}).first;
    } else if (!it->second.bt_handle.empty() && it->second.bt_handle != b.handle &&
               now - it->second.bt_seen < std::chrono::milliseconds(kPeerForgetMs)) {
      return;  // as for Wi-Fi: a fresh peer keeps its device
    }
    peer& p = it->second;
    p.id = b.id;
    if (p.name.empty() || !p.wifi) p.name = clean_name(b.name);
    p.bt_handle = b.handle;
    p.bt_seen = std::chrono::steady_clock::now();
    refresh_peers_locked();
    changed = std::exchange(peers_changed_, false);
  }
  if (changed) post_throttled();
}

void engine::accept_loop() {
  for (;;) {
    net::listener* l = listener_.get();
    if (!l || stopping_.load()) return;
    auto c = l->accept(500);
    if (c) {
      on_incoming(std::move(*c));
      continue;
    }
    if (c.error() == status::cancelled) return;
    if (c.error() != status::timeout) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

void engine::on_incoming(std::unique_ptr<link> l) {
  if (!l || stopping_.load()) return;
  bool busy = false;
  {
    std::lock_guard lock(m_);
    busy = transfer_running_;
    if (!busy) {
      transfer_running_ = true;
      cancel_.store(false);
    }
  }
  std::lock_guard threads(threads_m_);
  if (busy) {
    reap_helpers();
    if (static_cast<int>(helpers_.size()) >= kMaxHelpers) return;  // the link closes as it goes
    auto done = std::make_shared<std::atomic<bool>>(false);
    helpers_.push_back({std::thread([this, done, link = std::shared_ptr<sharepoint::link>(std::move(l))]() mutable {
                          try {
                            refuse_busy(std::move(link));
                          } catch (...) {
                          }
                          done->store(true);
                        }),
                        done});
    return;
  }
  if (transfer_thread_.joinable()) transfer_thread_.join();  // the last one has ended
  transfer_thread_ = std::thread([this, link = std::move(l)]() mutable {
    try {
      run_receive(std::move(link));
    } catch (...) {
      fail_transfer(false);
    }
  });
}

void engine::reap_helpers() {
  for (auto it = helpers_.begin(); it != helpers_.end();) {
    if (it->done->load()) {
      it->thread.join();
      it = helpers_.erase(it);
    } else {
      ++it;
    }
  }
}

void engine::refuse_busy(std::shared_ptr<link> l) {
  // At once, after the handshake: the sender learns before its person is
  // asked to check a code that this side will not show.
  auto ch = open_tracked(std::move(l), false);
  if (!ch) return;
  (void)send_decline(**ch, outcome::busy);
  (*ch)->shutdown();
}

result<std::unique_ptr<channel>> engine::open_tracked(std::shared_ptr<link> l, bool client) {
  {
    std::lock_guard lock(m_);
    if (stopping_.load()) return err(status::cancelled);
    opening_.push_back(l);
  }
  auto ch = channel::open(l, client, id_, opts_.my_name);
  forget_opening(l);
  return ch;
}

void engine::forget_opening(const std::shared_ptr<link>& l) {
  std::lock_guard lock(m_);
  std::erase(opening_, l);
}

void engine::fail_transfer(bool sending) noexcept {
  try {
    {
      std::lock_guard lock(m_);
      if (sending) {
        v_.out = view::out_phase::ended;
        v_.out_note = "Something went wrong. Nothing partial was left.";
      } else if (v_.in != view::in_phase::none) {
        v_.in = view::in_phase::ended;
        v_.in_note = "Something went wrong. Nothing partial was kept.";
      }
    }
    end_transfer();
  } catch (...) {
  }
}

// ---- the person's side ----------------------------------------------------------------

void engine::screen_opened(const std::string& path) {
  bool want_file = false;
  {
    std::lock_guard lock(m_);
    screen_open_ = true;
    want_file = files_.empty() && !v_.reading_folder && !path.empty();
  }
  if (want_file) set_files({path});
  request_reconcile();
  post();
}

void engine::screen_closed() {
  bool closing_connecting = false;
  {
    std::lock_guard lock(m_);
    screen_open_ = false;
    // Closing the window while an offer waits declines it.
    if (v_.in == view::in_phase::asking && decision_ == 0) {
      decision_ = 2;
      decided_.notify_all();
    }
    closing_connecting = v_.in == view::in_phase::connecting;
  }
  if (closing_connecting) cancel_transfer();  // and a sender still checking the code is told no
  request_reconcile();
}

void engine::set_files(const std::vector<std::string>& paths) {
  std::uint64_t gen = 0;
  {
    std::lock_guard lock(m_);
    if (out_busy_locked()) return;  // the set is fixed while it is being sent
    gen = ++files_generation_;
    v_.reading_folder = true;
  }
  // Handed to the folder reader; this returns at once, whatever it is reading.
  queue_files({paths, {}, gen});
  post_throttled();
}

void engine::queue_files(files_job job) {
  std::lock_guard lock(files_m_);
  if (stopping_.load()) return;
  files_next_ = std::move(job);  // a set waiting is replaced: only the newest is read
  if (!files_thread_.joinable()) files_thread_ = std::thread([this] { files_loop(); });
  files_cv_.notify_all();
}

bool engine::files_stale(std::uint64_t gen) const noexcept {
  return stopping_.load() || gen != files_generation_.load();
}

void engine::files_loop() {
  for (;;) {
    files_job job;
    {
      std::unique_lock lock(files_m_);
      files_cv_.wait(lock, [&] { return stopping_.load() || files_next_.has_value(); });
      if (stopping_.load()) return;
      job = std::move(*files_next_);
      files_next_.reset();
    }
    if (files_stale(job.gen)) continue;
    try {
      if (!job.folder.empty()) {
        walk_folder(job.folder, job.gen);
      } else {
        build_files(std::move(job.paths), job.gen);
      }
    } catch (...) {
      publish_files({}, job.gen);  // out of memory: an empty set, not a stuck "Reading"
    }
  }
}

bool engine::out_busy_locked() const noexcept {
  return v_.out == view::out_phase::waiting_wifi || v_.out == view::out_phase::connecting || v_.out == view::out_phase::confirming ||
         v_.out == view::out_phase::waiting || v_.out == view::out_phase::sending;
}

void engine::publish_files(std::vector<outgoing_file> files, std::uint64_t gen) {
  {
    std::lock_guard lock(m_);
    if (gen != files_generation_) return;  // a newer set replaced this one
    files_ = std::move(files);
    v_.reading_folder = false;
    v_.file_count = files_.size();
    v_.file_bytes = 0;
    v_.file_names.clear();
    for (const auto& f : files_) {
      v_.file_bytes += f.size;
      if (v_.file_names.size() < kListedNames) v_.file_names.push_back(f.rel);
    }
    if (v_.out == view::out_phase::done || v_.out == view::out_phase::ended) v_.out = view::out_phase::idle;
  }
  post();
}

void engine::build_files(std::vector<std::string> paths, std::uint64_t gen) {
  std::vector<outgoing_file> out;
  std::set<std::string> taken;
  auto add = [&](const std::string& path) {
    if (out.size() >= kMaxFiles || !taken.insert(path).second) return;
    if (files_stale(gen)) return;  // a newer set replaced this one
    auto st = io::stat_path(path);
    if (!st || st->is_directory) return;
    std::string rel(leaf_name(path));
    if (!safe_relative(rel) || housekeeping(rel)) return;
    out.push_back({path, std::move(rel), st->size, st->mtime_unix});
  };
  for (const auto& p : paths) {
    if (files_stale(gen)) return;
    add(p);
  }
  // Each file with its pair half and its sidecars (docs/design/28 "What
  // moves"): one listing per folder, never per file.
  std::map<std::string, std::vector<std::string>> by_dir;
  for (const auto& f : out) by_dir[std::string(io::parent_of(f.path))].push_back(std::string(leaf_name(f.path)));
  for (const auto& [dir, chosen] : by_dir) {
    if (files_stale(gen)) return;
    std::vector<std::string> names;
    (void)io::walk_files(dir, 0, [&](const io::tree_entry& e) {
      if (!e.cloud_only) names.push_back(e.name_utf8);
      return names.size() < 20000 && !files_stale(gen);
    });
    if (files_stale(gen)) return;
    std::unordered_map<std::string, std::size_t> index;
    std::unordered_map<std::string, std::vector<std::size_t>> by_stem;  // lower-case stem -> names
    for (std::size_t i = 0; i < names.size(); ++i) {
      index[names[i]] = i;
      by_stem[lower(stem_of(names[i]))].push_back(i);
    }
    const std::vector<std::uint32_t> partner = host_.pair_partners(names);
    for (const std::string& name : chosen) {
      const auto it = index.find(name);
      if (it == index.end()) continue;
      if (partner.size() == names.size() && partner[it->second] < names.size()) {
        add(io::join_path(dir, names[partner[it->second]]));
      }
      // IMG_1234.xmp and darktable's IMG_1234.CR2.xmp.
      const std::string l = lower(name);
      for (const std::string& key : {lower(stem_of(name)), l}) {
        const auto s = by_stem.find(key);
        if (s == by_stem.end()) continue;
        for (std::size_t j : s->second) {
          const std::string other = lower(names[j]);
          if (other.size() > 4 && other.compare(other.size() - 4, 4, ".xmp") == 0) add(io::join_path(dir, names[j]));
        }
      }
    }
  }
  publish_files(std::move(out), gen);
}

void engine::read_folder(std::string folder) {
  std::uint64_t gen = 0;
  {
    std::lock_guard lock(m_);
    if (out_busy_locked()) return;
    gen = ++files_generation_;
    v_.reading_folder = true;
  }
  queue_files({{}, std::move(folder), gen});
  post_throttled();
}

void engine::walk_folder(const std::string& folder, std::uint64_t gen) {
  std::vector<outgoing_file> out;
  std::string top(leaf_name(folder));
  while (!top.empty() && (top.back() == '/' || top.back() == '\\')) top.pop_back();
  if (!safe_relative(top)) top = "Folder";
  (void)io::walk_files(folder, 32, [&](const io::tree_entry& e) {
    if (files_stale(gen)) return false;
    if (e.cloud_only || skipped_relative(e.relative_utf8)) return true;
    std::string rel = top + "/" + e.relative_utf8;
    if (safe_relative(rel)) out.push_back({e.path_utf8, std::move(rel), e.size, e.mtime_unix});
    return out.size() < kMaxFiles;
  });
  publish_files(std::move(out), gen);
}

void engine::on_event(const std::string& node, std::uint32_t kind, const std::string& value_json) {
  const auto value = value_json.empty() ? std::nullopt : json::parse(value_json, 4);
  const std::string* text = value && value->k == json::kind::string ? &value->s : nullptr;
  constexpr std::uint32_t kClick = 1, kChange = 2, kSubmit = 3, kSelect = 4;

  if (node == "receiving" && kind == kChange && value && value->k == json::kind::boolean) {
    {
      std::lock_guard lock(m_);
      v_.receiving = value->b;
    }
    request_reconcile();
    post();
  } else if (node == "peers" && kind == kSelect && value && value->k == json::kind::array) {
    std::lock_guard lock(m_);
    v_.selected_peer = !value->a.empty() && value->a[0].k == json::kind::string ? value->a[0].s : std::string();
  } else if (node == "send" && kind == kClick) {
    start_send();
  } else if (node == "clear" && kind == kClick) {
    {
      std::lock_guard lock(m_);
      if (out_busy_locked()) return;
      ++files_generation_;
      files_.clear();
      v_.file_count = 0;
      v_.file_bytes = 0;
      v_.file_names.clear();
      v_.reading_folder = false;
    }
    post();
  } else if (node == "send_folder" && (kind == kChange || kind == kSubmit) && text && !text->empty()) {
    auto st = io::stat_path(*text);
    if (st && st->is_directory) read_folder(*text);
  } else if (node == "destination" && (kind == kChange || kind == kSubmit) && text) {
    auto st = io::stat_path(*text);
    bool changed = false;
    {
      std::lock_guard lock(m_);
      if (st && st->is_directory && v_.destination != *text) {
        v_.destination = *text;
        changed = true;
      }
    }
    if (changed) request_save();  // kept for the next launch (#442)
    post();  // a folder that is not there puts the last good one back
  } else if ((node == "accept" || node == "decline") && kind == kClick) {
    bool connecting = false;
    {
      std::lock_guard lock(m_);
      if (v_.in == view::in_phase::asking && decision_ == 0) {
        decision_ = node == "accept" ? 1 : 2;
        decided_.notify_all();
      }
      connecting = node == "decline" && v_.in == view::in_phase::connecting;
    }
    if (connecting) cancel_transfer();
  } else if (node == "out_confirm" && kind == kClick) {
    std::lock_guard lock(m_);
    if (v_.out == view::out_phase::confirming && out_decision_ == 0) {
      out_decision_ = 1;
      decided_.notify_all();
    }
  } else if ((node == "in_cancel" || node == "out_cancel") && kind == kClick) {
    cancel_transfer();
  } else if (node == "in_dismiss" && kind == kClick) {
    {
      std::lock_guard lock(m_);
      if (v_.in == view::in_phase::done || v_.in == view::in_phase::ended) v_.in = view::in_phase::none;
    }
    post();
  }
}

void engine::cancel_transfer() {
  std::shared_ptr<channel> ch;
  std::vector<std::shared_ptr<link>> opening;
  {
    std::lock_guard lock(m_);
    cancel_.store(true);
    ch = live_;
    opening = opening_;
    if (v_.in == view::in_phase::asking && decision_ == 0) {
      decision_ = 2;
    }
    if (v_.out == view::out_phase::confirming && out_decision_ == 0) out_decision_ = 2;
    decided_.notify_all();
  }
  for (const auto& l : opening) l->shutdown();  // a handshake a peer stalls
  bool was_waiting = false;
  {
    std::lock_guard lock(m_);
    if (v_.out == view::out_phase::waiting_wifi) {
      was_waiting = true;
      waiting_peer_.clear();
      v_.out = view::out_phase::ended;
      v_.out_note = "Cancelled. Nothing was sent.";
    }
  }
  if (was_waiting) {
    request_reconcile();
    post();
  }
  if (ch) {
    // Said if the link is free now; never waits behind a chunk being sent.
    (void)ch->try_send(msg::cancel, "{}");
    ch->shutdown();
  }
}

hooks engine::hooks_for(bool sending) {
  hooks h;
  h.cancel = &cancel_;
  h.note = [this](const char* text) { host_.log(1, text); };
  h.yield = [this] {
    // While the viewer presents, wait between chunks (as Import does); the
    // channel's pinger keeps the link alive meanwhile.
    for (int i = 0; i < 200 && host_.should_yield() && !cancel_.load() && !stopping_.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  };
  h.progress = [this, sending](std::uint64_t bytes, std::uint32_t files) {
    {
      std::lock_guard lock(m_);
      if (sending) {
        v_.out_bytes = bytes;
        v_.out_files = files;
      } else {
        v_.in_bytes = bytes;
        v_.in_files = files;
      }
    }
    post_throttled();
  };
  return h;
}

// ---- sending ------------------------------------------------------------------------

void engine::start_waiting_send() {
  {
    std::lock_guard lock(m_);
    if (waiting_peer_.empty() || v_.out != view::out_phase::waiting_wifi) return;
    v_.selected_peer = waiting_peer_;
    waiting_peer_.clear();
    v_.out = view::out_phase::idle;
  }
  start_send();  // the peer is fresh on Wi-Fi now, so this connects there
}

bool needs_wifi(const view& v) noexcept {
  if (v.selected_peer.empty() || v.file_bytes <= kBluetoothMaxBytes) return false;
  for (const auto& p : v.peers) {
    if (p.id == v.selected_peer) return p.bt && !p.wifi;
  }
  return false;
}

void engine::start_send() {
  std::string peer_id;
  std::vector<outgoing_file> files;
  bool waiting = false;
  {
    std::lock_guard lock(m_);
    if (transfer_running_ || files_.empty() || v_.reading_folder || v_.selected_peer.empty()) return;
    if (v_.out == view::out_phase::waiting_wifi || peers_.find(v_.selected_peer) == peers_.end()) return;
    v_.out_peer = peers_[v_.selected_peer].name;
    v_.out_code.clear();
    v_.out_note.clear();
    if (needs_wifi(v_)) {
      // Too big for Bluetooth: wait for this peer on Wi-Fi (on_beacon starts
      // the send), with presence kept up meanwhile.
      waiting_peer_ = v_.selected_peer;
      v_.out = view::out_phase::waiting_wifi;
      waiting = true;
    } else {
      transfer_running_ = true;
      cancel_.store(false);
      peer_id = v_.selected_peer;
      files = files_;
      out_decision_ = 0;
      v_.out = view::out_phase::connecting;
      v_.out_bytes = 0;
      v_.out_files = 0;
      v_.out_count = static_cast<std::uint32_t>(files.size());
      v_.out_total = v_.file_bytes;
    }
  }
  post();
  if (waiting) {
    request_reconcile();
    return;
  }
  std::lock_guard threads(threads_m_);
  if (transfer_thread_.joinable()) transfer_thread_.join();
  transfer_thread_ = std::thread([this, peer_id, files = std::move(files)]() mutable {
    try {
      run_send(std::move(peer_id), std::move(files));
    } catch (...) {
      fail_transfer(true);
    }
  });
}

void engine::end_transfer() {
  {
    std::lock_guard lock(m_);
    live_.reset();
    transfer_running_ = false;
    idle_.notify_all();
  }
  request_reconcile();
  post();
}

void engine::run_send(std::string peer_id, std::vector<outgoing_file> files) {
  std::optional<net::endpoint> wifi;
  std::string bt_handle, name;
  std::uint64_t total = 0;
  for (const auto& f : files) total += f.size;
  {
    std::lock_guard lock(m_);
    const auto it = peers_.find(peer_id);
    if (it != peers_.end()) {
      const auto now = std::chrono::steady_clock::now();
      if (it->second.wifi && now - it->second.wifi_seen < std::chrono::milliseconds(kPeerForgetMs)) {
        wifi = it->second.wifi;
      }
      bt_handle = it->second.bt_handle;
      name = it->second.name;
    }
  }
  auto finish = [&](std::string note) {
    {
      std::lock_guard lock(m_);
      v_.out = view::out_phase::ended;
      v_.out_note = std::move(note);
    }
    end_transfer();
  };

  // Wi-Fi when the peer is on this network; Bluetooth only for a small set.
  result<std::unique_ptr<link>> l = err(status::not_found);
  if (wifi && !cancel_.load()) {
    l = net::connect(*wifi, kConnectMs);
  }
  if (!l && !bt_handle.empty() && !cancel_.load()) {
    if (total > kBluetoothMaxBytes) {
      return finish(wifi ? "Could not reach " + name + " on the local network, and " + size_text(total) +
                               " is too much for Bluetooth (32 MB at most). Join the same Wi-Fi network and try again."
                         : size_text(total) + " is too much for Bluetooth (32 MB at most). Join the same Wi-Fi "
                                              "network as " + name + " and try again.");
    }
    std::shared_ptr<bluetooth> bt;
    {
      std::lock_guard p(presence_m_);
      bt = bluetooth_;
    }
    if (bt) l = bt->connect(bt_handle, kConnectMs * 2);
  }
  if (cancel_.load()) return finish("Cancelled. Nothing was sent.");
  if (!l) return finish("Could not reach " + name + ". Check that its Sharepoint window is still open.");
  auto opened = open_tracked(std::shared_ptr<link>(std::move(*l)), true);
  if (cancel_.load()) return finish("Cancelled. Nothing was sent.");
  if (!opened) {
    return finish(opened.error() == status::unsupported_format
                      ? name + " has a different version of Sharepoint. Update MediaViewer on both computers."
                      : "Could not reach " + name + ".");
  }
  std::shared_ptr<channel> ch(std::move(*opened));
  if (ch->peer_id() != peer_id) {
    // Another client now answers at that address: not the one the person picked.
    ch->shutdown();
    return finish("Could not reach " + name + ".");
  }
  {
    std::lock_guard lock(m_);
    live_ = ch;
    v_.out = view::out_phase::confirming;
    v_.out_code = ch->code();
  }
  post();
  // The person checks that the other screen shows the same code before the
  // offer (even the files' names) leaves: a machine that only claimed the
  // peer's id, or one in the middle, shows another code there or none.
  const auto asked = std::chrono::steady_clock::now();
  int confirmed = 0;
  for (;;) {
    {
      std::unique_lock lock(m_);
      decided_.wait_for(lock, std::chrono::milliseconds(250),
                        [&] { return out_decision_ != 0 || stopping_.load() || cancel_.load(); });
      confirmed = out_decision_;
    }
    if (cancel_.load() || stopping_.load() || confirmed == 2) {
      (void)ch->try_send(msg::cancel, "{}");
      ch->shutdown();
      return finish("Cancelled. Nothing was sent.");
    }
    if (confirmed == 1) break;
    if (std::chrono::steady_clock::now() - asked > kAskFor) {
      (void)ch->try_send(msg::cancel, "{}");
      ch->shutdown();
      return finish("Nobody confirmed the code, so nothing was sent.");
    }
    message m;
    auto r = ch->receive(m, 0);
    if (r) {
      ch->shutdown();
      if (m.type == msg::decline) {
        const auto doc = json::parse(m.text(), 4);
        const std::string* why = doc ? doc->str("why") : nullptr;
        if (why && *why == "busy") return finish(name + " is receiving from someone else. Try again in a moment.");
      }
      return finish(name + " declined.");
    }
    if (r.error() != status::timeout) return finish("The connection to " + name + " was lost. Nothing was sent.");
  }
  {
    std::lock_guard lock(m_);
    v_.out = view::out_phase::waiting;
  }
  post();
  const transfer_result r = send_files(*ch, files, hooks_for(true), [this] {
    {
      std::lock_guard lock(m_);
      v_.out = view::out_phase::sending;
    }
    post();
  });
  std::string note;
  const std::string sent = plural(r.done, "file", "files");
  switch (r.how) {
    case outcome::delivered:
      note = "Sent " + sent + " to " + name + ".";
      if (r.skipped) note += " " + plural(r.skipped, "file", "files") + " could not be read and stayed here.";
      break;
    case outcome::declined: note = name + " declined."; break;
    case outcome::busy: note = name + " is receiving from someone else. Try again in a moment."; break;
    case outcome::no_room: note = name + " does not have room for " + size_text(total) + "."; break;
    case outcome::cancelled:
      note = r.done ? "Cancelled after " + sent + ". Those arrived whole; nothing partial was left."
                    : "Cancelled. Nothing was sent.";
      break;
    case outcome::failed:
      note = "The connection to " + name + " was lost. " +
             (r.done ? sent + " arrived whole; nothing partial was left." : std::string("Nothing was sent."));
      break;
  }
  {
    std::lock_guard lock(m_);
    v_.out = r.how == outcome::delivered ? view::out_phase::done : view::out_phase::ended;
    v_.out_note = std::move(note);
    v_.out_files = r.done;
  }
  end_transfer();
}

// ---- receiving --------------------------------------------------------------------------

void engine::run_receive(std::unique_ptr<link> l) {
  auto opened = open_tracked(std::shared_ptr<link>(std::move(l)), false);
  if (!opened) return end_transfer();  // not a MediaViewer, or gone: nothing to show
  std::shared_ptr<channel> ch(std::move(*opened));
  const std::string name = ch->peer_name();
  // The code shows now: the person there checks it against this screen
  // before anything about the files is sent.
  bool open_window = false;
  {
    std::lock_guard lock(m_);
    if (cancel_.load() || stopping_.load()) {
      ch->shutdown();
    } else {
      live_ = ch;
      decision_ = 0;
      v_.in = view::in_phase::connecting;
      v_.in_peer = name;
      v_.in_code = ch->code();
      v_.in_via = transport_name(ch->via());
      v_.in_note.clear();
      open_window = !screen_open_;
    }
  }
  if (open_window) host_.show_screen(true);
  post();
  auto offer = read_offer(*ch, kAskMs);
  if (!offer) {
    const bool declined = cancel_.load();
    {
      std::lock_guard lock(m_);
      if (v_.in == view::in_phase::connecting) {
        v_.in = view::in_phase::ended;
        v_.in_note = declined ? std::string("Declined. Nothing was received.")
                              : name + " did not send anything. Nothing was received.";
      }
    }
    ch->shutdown();
    return end_transfer();
  }
  {
    std::lock_guard lock(m_);
    v_.in = view::in_phase::asking;
    v_.in_peer = name;
    v_.in_code = ch->code();
    v_.in_via = transport_name(ch->via());
    v_.in_count = offer->files.size();
    v_.in_total = offer->bytes;
    v_.in_bytes = 0;
    v_.in_files = 0;
    v_.in_note.clear();
    v_.in_names.clear();
    for (const auto& f : offer->files) {
      if (v_.in_names.size() >= kListedNames) break;
      v_.in_names.emplace_back(leaf_name(f.rel));
    }
  }
  post();

  auto ended = [&](std::string note) {
    {
      std::lock_guard lock(m_);
      v_.in = view::in_phase::ended;
      v_.in_note = std::move(note);
    }
    ch->shutdown();
    end_transfer();
  };

  // The person decides; meanwhile a cancel or a drop from the sender ends it.
  const auto asked = std::chrono::steady_clock::now();
  int decision = 0;
  for (;;) {
    {
      std::unique_lock lock(m_);
      decided_.wait_for(lock, std::chrono::milliseconds(250),
                        [&] { return decision_ != 0 || stopping_.load() || cancel_.load(); });
      decision = decision_;
    }
    if (stopping_.load()) return ended("");
    if (decision != 0) break;
    if (cancel_.load()) {
      decision = 2;
      break;
    }
    if (std::chrono::steady_clock::now() - asked > kAskFor) {
      decision = 2;
      break;
    }
    message m;
    auto r = ch->receive(m, 0);
    if (r) {
      if (m.type == msg::cancel) return ended(name + " cancelled.");
      return ended("Something went wrong with the offer from " + name + ". Nothing was received.");
    }
    if (r.error() != status::timeout) return ended("The connection to " + name + " was lost. Nothing was received.");
  }
  if (decision != 1) {
    (void)send_decline(*ch, outcome::declined);
    return ended("Declined. Nothing was received.");
  }

  std::string dest;
  {
    std::lock_guard lock(m_);
    dest = v_.destination;
  }
  const std::string fallback = host_.default_destination();
  if (!dest.empty() && dest == fallback) (void)io::make_directories(dest);  // ours to create
  auto st = io::stat_path(dest);
  if (dest.empty() || !st || !st->is_directory) {
    (void)send_decline(*ch, outcome::declined);
    return ended("The folder to save into is not there. Nothing was received.");
  }
  if (const auto room = host_.free_bytes(dest); room && *room < offer->bytes + kRoomMargin) {
    (void)send_decline(*ch, outcome::no_room);
    return ended("Not enough space for " + size_text(offer->bytes) + " in that folder. Nothing was received.");
  }
  if (!send_accept(*ch)) return ended("The connection to " + name + " was lost. Nothing was received.");
  {
    std::lock_guard lock(m_);
    v_.in = view::in_phase::receiving;
  }
  post();
  const transfer_result r = receive_files(*ch, *offer, dest, hooks_for(false));
  std::string note;
  const std::string got = plural(r.done, "file", "files");
  switch (r.how) {
    case outcome::delivered:
      note = "Received " + got + " from " + name + " into " + dest + ".";
      if (r.skipped) note += " " + plural(r.skipped, "file", "files") + " could not be read on " + name + ".";
      break;
    case outcome::cancelled:
      note = r.done ? "Cancelled. " + got + " arrived whole in " + dest + "; nothing partial was kept."
                    : "Cancelled. Nothing was received.";
      break;
    default:
      note = "The transfer from " + name + " stopped. " +
             (r.done ? got + " arrived whole in " + dest + "; nothing partial was kept."
                     : std::string("Nothing partial was kept."));
      break;
  }
  {
    std::lock_guard lock(m_);
    v_.in = r.how == outcome::delivered ? view::in_phase::done : view::in_phase::ended;
    v_.in_note = std::move(note);
    v_.in_files = r.done;
  }
  ch->shutdown();
  end_transfer();
}

// ---- tests ------------------------------------------------------------------------------

view engine::snapshot() const {
  std::lock_guard lock(m_);
  return v_;
}

void engine::wait_idle() {
  std::unique_lock lock(m_);
  idle_.wait(lock, [&] { return !transfer_running_; });
}

}  // namespace mv::sharepoint
