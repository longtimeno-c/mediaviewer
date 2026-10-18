// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/sharepoint/engine.h"

#include <sodium.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <unordered_map>

#include "addons/sharepoint/meet.h"
#include "addons/sharepoint/race.h"
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

// "123 456", "123-456", " 123456 " -> "123456".
std::string digits_of(std::string_view s) {
  std::string out;
  for (char c : s) {
    if (c >= '0' && c <= '9') out.push_back(c);
  }
  return out;
}

}  // namespace

engine::engine(engine_host& host, options opts) : host_(host), opts_(std::move(opts)) {
  (void)sodium_init();
  id_ = random_id();
  randombytes_buf(static_key_.data(), static_key_.size());
  if (opts_.my_name.empty()) opts_.my_name = net::machine_name();
  v_.my_name = clean_name(opts_.my_name);
  // What was kept from the last run (#442). A saved folder that is gone
  // falls back to the default; it is not forgotten until another is picked.
  const prefs kept = opts_.initial ? *opts_.initial : load_prefs(opts_.prefs_path).value_or(prefs{});
  v_.receiving = kept.receiving || kept.background;  // presence starts on the control thread, below
  v_.background = kept.background;
  v_.internet = kept.internet;  // #472 PR 2: the receiver of codes, made on the control thread
  v_.dht = kept.dht;
  v_.forwarded = kept.forwarded;
  v_.destination = host_.default_destination();
  if (!kept.destination.empty()) {
    if (auto st = io::stat_path(kept.destination); st && st->is_directory) v_.destination = kept.destination;
  }
  control_thread_ = std::thread([this] { control_loop(); });
  // Keep receiving was on at the last launch (#442): presence and the status
  // text come up on the control thread, never the caller's. Off: nothing.
  if (v_.receiving || v_.internet) request_reconcile();
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
  {
    // The code, its listener and every router mapping go (joins its threads).
    std::shared_ptr<internet_receiver> in;
    {
      std::lock_guard lock(m_);
      in = std::move(internet_);
    }
    in.reset();
  }
  stop_presence();
  if (save_wanted_) save_now();  // a change the control thread had not written yet
  sodium_memzero(static_key_.data(), static_key_.size());
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
    p.background = v_.background;
    p.destination = v_.destination == host_.default_destination() ? std::string() : v_.destination;
    p.internet = v_.internet;
    p.forwarded = v_.forwarded;
    p.dht = v_.dht;
  }
  {
    std::lock_guard lock(control_m_);
    save_wanted_ = false;
  }
  if (!save_prefs(opts_.prefs_path, p)) host_.log(1, "sharepoint: could not keep the settings");
}

void engine::sync_status() {
  std::string want;
  bool keep = false;
  {
    std::lock_guard lock(m_);
    want = v_.receiving && !stopping_.load() ? kReceivingStatus : "";
    keep = v_.background && !stopping_.load();
  }
  if (want == status_shown_ && keep == keep_shown_) return;
  bool taken = true;
  if (want != status_shown_) {
    if (host_.post_status(want)) {
      status_shown_ = want;
    } else {
      taken = false;
    }
  }
  // #452: the host keeps this process (menu-bar item / tray icon) with every
  // window closed while Receive when MediaViewer is closed is on.
  if (keep != keep_shown_) {
    if (host_.keep_alive(keep)) {
      keep_shown_ = keep;
    } else {
      taken = false;
    }
  }
  if (taken) {
    status_tries_ = 0;
    std::lock_guard lock(control_m_);
    status_due_ = {};
    return;
  }
  // The add-on loads before the host attaches its items: try again shortly,
  // for a while (40 x 250 ms).
  std::lock_guard lock(control_m_);
  status_due_ = ++status_tries_ < 40 ? std::chrono::steady_clock::now() + std::chrono::milliseconds(250)
                                     : std::chrono::steady_clock::time_point{};
}

void engine::control_loop() {
  std::unique_lock lock(control_m_);
  while (!stopping_.load()) {
    const auto none = std::chrono::steady_clock::time_point{};
    const auto due = post_pending_ && status_due_ != none ? std::min(post_due_, status_due_)
                     : post_pending_                     ? post_due_
                                                         : status_due_;
    const auto woken = [&] { return stopping_.load() || reconcile_wanted_ || save_wanted_; };
    if (due != none) {
      control_cv_.wait_until(lock, due, woken);
    } else {
      control_cv_.wait(lock, [&] { return woken() || post_pending_; });
    }
    if (stopping_.load()) break;
    if (status_due_ != none && std::chrono::steady_clock::now() >= status_due_) {
      status_due_ = none;
      lock.unlock();
      sync_status();
      lock.lock();
    }
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
  sync_internet();
  sync_status();
}

void engine::sync_internet() {
  std::shared_ptr<internet_receiver> drop;
  bool make = false;
  internet_receiver::options o;
  {
    std::lock_guard lock(m_);
    const bool want = v_.internet && !stopping_.load() && !opts_.offline;
    if (!want && internet_) {
      drop = std::move(internet_);
      v_.code_phase = 0;
      v_.code_text.clear();
      v_.code_link.clear();
      v_.code_note.clear();
      v_.code_how.clear();
    } else if (want && !internet_) {
      make = true;
      o.loopback_only = opts_.internet_loopback;
      o.forwarded = v_.forwarded;
      o.mapping = opts_.internet_mapping;
      o.lifetime_ms = opts_.code_lifetime_ms;
      o.dht = v_.dht;
      o.dht_client = dht_options();
      o.dht_fast_kdf = opts_.dht_fast_kdf;
      o.force_words = opts_.internet_force_words;
    }
  }
  if (drop) {
    drop.reset();  // its code, listener and mappings go, on this thread
    post();
    return;
  }
  if (!make) return;
  auto in = std::make_shared<internet_receiver>(
      o, [this](std::unique_ptr<link> l) { on_internet_link(std::move(l)); },
      [this] {
        // The receiver's state, into the window (never under the engine's lock
        // while it is asked).
        std::shared_ptr<internet_receiver> r;
        {
          std::lock_guard lock(m_);
          r = internet_;
        }
        if (!r) return;
        const auto s = r->state();
        {
          std::lock_guard lock(m_);
          v_.code_phase = static_cast<std::uint8_t>(s.p);
          v_.code_text = s.text;
          v_.code_link = s.link;
          v_.code_note = s.note;
          std::string how;
          for (const auto& h : s.how) how += (how.empty() ? "" : ", ") + h;
          v_.code_how = how;
        }
        post_throttled();
      });
  std::lock_guard lock(m_);
  internet_ = std::move(in);
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
    self.protocol = kProtocolVersion;  // #472: "p", beside the "v" protocol 1 checks
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
    const bool resumable = (wifi && p.wifi_protocol >= 2) || (bt && p.bt_protocol >= 2);
    rows.push_back({id, p.name, wifi && bt ? "Wi-Fi and Bluetooth" : wifi ? "Wi-Fi" : "Bluetooth", wifi, bt, resumable});
  }
  std::sort(rows.begin(), rows.end(), [](const view::peer_row& a, const view::peer_row& b) {
    return a.name != b.name ? a.name < b.name : a.id < b.id;
  });
  const bool same = rows.size() == v_.peers.size() &&
                    std::equal(rows.begin(), rows.end(), v_.peers.begin(), [](const auto& a, const auto& b) {
                      return a.id == b.id && a.name == b.name && a.via == b.via && a.resumable == b.resumable;
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
    const bool was_fresh = p.wifi && now - p.wifi_seen < std::chrono::milliseconds(kPeerForgetMs);
    p.wifi = net::endpoint{address, b.port};
    p.wifi_seen = std::chrono::steady_clock::now();
    p.wifi_protocol = b.protocol;
    refresh_peers_locked();
    changed = std::exchange(peers_changed_, false);
    ready = b.id == waiting_peer_ && !transfer_running_;
    // #472: the peer of a send is on Wi-Fi now: the mover may move there.
    if (!was_fresh && b.id == v_.out_peer_id && transfer_running_) {
      mover_kick_ = true;
      mover_cv_.notify_all();
    }
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
    p.bt_protocol = b.protocol;
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

void engine::on_internet_link(std::unique_ptr<link> l) {
  if (!l) return;
  on_incoming(std::make_unique<bearer_as>(std::move(l), transport::internet), true);
}

void engine::on_incoming(std::unique_ptr<link> l, bool internet) {
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
    helpers_.push_back({std::thread([this, done, internet, link = std::shared_ptr<sharepoint::link>(std::move(l))]() mutable {
                          try {
                            route_busy(std::move(link), internet);
                          } catch (...) {
                          }
                          done->store(true);
                        }),
                        done});
    return;
  }
  if (transfer_thread_.joinable()) transfer_thread_.join();  // the last one has ended
  transfer_thread_ = std::thread([this, internet, link = std::move(l)]() mutable {
    try {
      run_receive(std::move(link), internet);
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

void engine::route_busy(std::shared_ptr<link> l, bool internet) {
  {
    std::lock_guard lock(m_);
    if (stopping_.load()) return;
    opening_.push_back(l);
  }
  auto h = channel::read_hello(*l, std::chrono::steady_clock::now() + std::chrono::milliseconds(kConnectMs));
  if (h && h->rejoin) {
    // #472: the sender of the transfer under way, back on another link (or
    // the same bearer, after a drop). It goes to the session it names; a
    // rejoin naming anything else is closed.
    std::shared_ptr<channel> ch;
    {
      std::lock_guard lock(m_);
      ch = live_;
    }
    if (ch && ch->names_this(*h)) {
      if (auto r = ch->accept_rejoin(l, *h); !r) host_.log(1, "sharepoint: a rejoin did not complete");
    } else {
      l->shutdown();
    }
    forget_opening(l);
    return;
  }
  if (!h) {
    l->shutdown();
    forget_opening(l);
    return;
  }
  if (h->internet) {
    // #472 PR 2: a code's sender while a transfer runs here. The code dies
    // (its first attempt), and the link closes: one transfer at a time.
    std::shared_ptr<internet_receiver> in;
    {
      std::lock_guard lock(m_);
      in = internet_;
    }
    if (internet && in && in->spend()) in->release();
    l->shutdown();
    forget_opening(l);
    return;
  }
  // At once, after the handshake: the sender learns before its person is
  // asked to check a code that this side will not show.
  auto ch = open_tracked(l, false, kProtocolVersion, &*h);
  forget_opening(l);
  if (!ch) return;
  (void)send_decline(**ch, outcome::busy);
  (*ch)->shutdown();
}

result<std::unique_ptr<channel>> engine::open_tracked(std::shared_ptr<link> l, bool client, std::uint32_t version,
                                                      const hello* read,
                                                      const std::vector<std::uint8_t>* internet_secret) {
  {
    std::lock_guard lock(m_);
    if (stopping_.load()) return err(status::cancelled);
    opening_.push_back(l);
  }
  channel::options o;
  o.version = version;
  o.static_key = static_key_;
  if (internet_secret) o.internet_secret = *internet_secret;
  auto ch = read ? channel::open_server(l, *read, id_, opts_.my_name, o)
                 : channel::open(l, client, id_, opts_.my_name, o);
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
         v_.out == view::out_phase::waiting || v_.out == view::out_phase::sending || v_.out == view::out_phase::reaching ||
         v_.out == view::out_phase::naming;
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
    v_.file_paths.clear();
    for (const auto& f : files_) {
      v_.file_bytes += f.size;
      if (v_.file_names.size() < kListedNames) v_.file_names.push_back(f.rel);
      if (v_.file_paths.size() < kShownThumbs) v_.file_paths.push_back(f.path);
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
  constexpr std::uint32_t kClick = 1, kChange = 2, kSubmit = 3, kSelect = 4, kActivate = 5;

  if (node == "internet" && kind == kChange && value && value->k == json::kind::boolean) {
    // #472 PR 2: opt-in, off by default; off drops any code and its mappings.
    {
      std::lock_guard lock(m_);
      v_.internet = value->b;
    }
    request_reconcile();
    request_save();
    post();
    return;
  }
  if (node == "dht" && kind == kChange && value && value->k == json::kind::boolean) {
    // #472 PR 3: opt-in, off by default; the DHT runs only while a code waits.
    std::shared_ptr<internet_receiver> in;
    {
      std::lock_guard lock(m_);
      v_.dht = value->b;
      in = internet_;
    }
    if (in) in->set_dht(value->b);
    request_save();
    post();
    return;
  }
  if (node == "forwarded" && (kind == kChange || kind == kSubmit) && text) {
    std::shared_ptr<internet_receiver> in;
    {
      std::lock_guard lock(m_);
      v_.forwarded = *text;
      in = internet_;
    }
    if (in) in->set_forwarded(*text);
    request_save();
    return;
  }
  if ((node == "make_code" || node == "stop_code") && kind == kClick) {
    std::shared_ptr<internet_receiver> in;
    {
      std::lock_guard lock(m_);
      in = internet_;
    }
    if (in) node == "make_code" ? in->make_code() : in->drop_code();
    return;
  }
  if (node == "send_code" && (kind == kChange || kind == kSubmit) && text) {
    {
      std::lock_guard lock(m_);
      code_entry_ = *text;
      v_.send_code_note.clear();
    }
    if (kind == kSubmit) start_send_code();
    return;
  }
  if (node == "send_to_code" && kind == kClick) {
    start_send_code();
    return;
  }
  if (node == "out_confirm" && kind == kClick) {
    std::lock_guard lock(m_);
    if (v_.out == view::out_phase::naming && out_decision_ == 0) {
      out_decision_ = 1;
      decided_.notify_all();
    }
    return;
  }
  if ((node == "receiving" || node == "background") && kind == kChange && value &&
      value->k == json::kind::boolean) {
    bool changed = false;
    {
      std::lock_guard lock(m_);
      const bool was_receiving = v_.receiving, was_background = v_.background;
      if (node == "receiving") {
        v_.receiving = value->b;
        if (!value->b) v_.background = false;  // closed MediaViewer cannot receive without it
      } else {
        v_.background = value->b;
        if (value->b) v_.receiving = true;  // #452: receiving with MediaViewer closed is receiving
      }
      changed = v_.receiving != was_receiving || v_.background != was_background;
    }
    // #452: the OS asks, once, the first time either is turned on; never at launch.
    if (value->b) host_.ask_to_notify();
    request_reconcile();
    if (changed) request_save();  // kept for the next launch (#442)
    post();
  } else if (node == "peers" && kind == kActivate && text) {
    // #442: one click on a nearby computer sends to it, as AirDrop.
    {
      std::lock_guard lock(m_);
      if (transfer_running_ || out_busy_locked()) return;
      v_.selected_peer = *text;
    }
    start_send();
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
      v_.file_paths.clear();
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
  } else if (node == "out_code_entry" && (kind == kChange || kind == kSubmit) && text) {
    // The code the receiver's screen shows, typed here (#442). Checked on this
    // machine against the code this link derived; a partial entry is not a try.
    const std::string typed = digits_of(*text);
    {
      std::lock_guard lock(m_);
      if (v_.out != view::out_phase::confirming || out_decision_ != 0 || out_expected_.empty()) return;
      if (typed.size() < out_expected_.size()) {
        if (kind == kSubmit && !typed.empty()) v_.out_entry_note = "The code has 6 digits.";
      } else if (typed.size() == out_expected_.size() &&
                 sodium_memcmp(typed.data(), out_expected_.data(), typed.size()) == 0) {
        out_decision_ = 1;
        v_.out_entry_note.clear();
        decided_.notify_all();
      } else if (--v_.out_tries_left <= 0) {
        out_decision_ = 3;  // the link ends (run_send)
        decided_.notify_all();
      } else {
        v_.out_entry_note = "That is not the code on " + v_.out_peer + "\xE2\x80\x99s screen. " +
                            plural(static_cast<std::uint32_t>(v_.out_tries_left), "try", "tries") + " left.";
      }
    }
    post();
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
      // #472: a link that was suspect and is heard again (no rejoin needed).
      if (live_ && live_->link_up()) (sending ? v_.out_reconnecting : v_.in_reconnecting) = false;
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
    // #472: a protocol 2 peer takes it over Bluetooth, and the send moves to
    // Wi-Fi once both are on one network.
    if (p.id == v.selected_peer) return p.bt && !p.wifi && !p.resumable;
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
    v_.out_peer_id = v_.selected_peer;
    v_.out_note.clear();
    v_.out_entry_note.clear();
    v_.out_tries_left = kCodeTries;
    out_expected_.clear();
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

void engine::start_send_code() {
  std::optional<invite::code> code;
  std::optional<dht::word_code> words;
  std::vector<outgoing_file> files;
  {
    std::lock_guard lock(m_);
    if (!v_.internet || transfer_running_ || out_busy_locked() || v_.reading_folder) return;
    code = invite::decode(code_entry_);
    if (!code && v_.dht) words = dht::parse_words(code_entry_);
    if (!code && !words) {
      v_.send_code_note = code_entry_.empty()
                              ? std::string(v_.dht ? "Paste the code or link, or type the four words, the other person "
                                                     "sent you."
                                                   : "Paste the code or link the other person sent you.")
                              : std::string("That is not a whole code: check it against what they sent (a mistyped "
                                            "code is caught here, before it is spent).");
    } else if (files_.empty()) {
      v_.send_code_note = "Pick the files to send first.";
      code.reset();
      words.reset();
    }
    if ((code || words) && !files_.empty()) {
      v_.send_code_note.clear();
      transfer_running_ = true;
      cancel_.store(false);
      files = files_;
      out_decision_ = 0;
      v_.out = view::out_phase::reaching;
      v_.out_internet = true;
      v_.out_attempt = 0;
      v_.out_peer = "the other computer";
      v_.out_peer_id.clear();
      v_.out_note.clear();
      v_.out_check.clear();
      v_.out_bytes = 0;
      v_.out_files = 0;
      v_.out_count = static_cast<std::uint32_t>(files.size());
      v_.out_total = v_.file_bytes;
    }
  }
  post();
  if (!code && !words) return;
  std::lock_guard threads(threads_m_);
  if (transfer_thread_.joinable()) transfer_thread_.join();
  transfer_thread_ = std::thread([this, files = std::move(files), code = std::move(code), words]() mutable {
    try {
      run_send({}, std::move(files), std::move(code), words);
    } catch (...) {
      fail_transfer(true);
    }
  });
}

dht::client::options engine::dht_options() const {
  dht::client::options o;
  if (opts_.dht_nodes) {
    o.use_routers = false;
    o.extra = *opts_.dht_nodes;
    o.query_ms = 500;
    o.lookup_ms = 5000;
  }
  if (!opts_.prefs_path.empty()) {
    o.table_path = std::string(io::parent_of(opts_.prefs_path));
    o.table_path = o.table_path.empty() ? std::string("sharepoint_dht.dat") : io::join_path(o.table_path, "sharepoint_dht.dat");
  }
  return o;
}

void engine::run_send(std::string peer_id, std::vector<outgoing_file> files, std::optional<invite::code> code,
                      std::optional<dht::word_code> words) {
  std::optional<net::endpoint> wifi;
  std::string bt_handle, name;
  std::uint32_t protocol = kProtocol1;
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
      // #472: protocol 2 when the peer announced it (a peer from before
      // speaks protocol 1, on the LAN and Bluetooth, without moving).
      protocol = std::max(it->second.wifi_protocol, it->second.bt_protocol) >= 2 ? kProtocolVersion : kProtocol1;
    }
  }
  auto finish = [&](std::string note) {
    {
      std::lock_guard lock(m_);
      v_.out = view::out_phase::ended;
      v_.out_note = std::move(note);
      out_expected_.clear();
    }
    end_transfer();
  };

  // #472 PR 2: to a code, over the internet. Every place at once, round
  // after round, until one connects or Cancel; then CPace proves the code.
  std::vector<std::unique_ptr<link_race>> races;  // joined when this send ends
  struct races_guard {
    std::vector<std::unique_ptr<link_race>>& r;
    ~races_guard() {
      for (auto& x : r) x->join();
    }
  } join_races{races};
  std::optional<net::endpoint> internet_place;
  if (words) {
    // #472 PR 3: four words. Meet on the DHT, punch, then the internet
    // handshake over the punched path, keyed by the words.
    const auto stopped = [this] { return cancel_.load() || stopping_.load(); };
    auto say = [this](const std::string& s) {
      {
        std::lock_guard lock(m_);
        v_.out_note = s;
      }
      post_throttled();
    };
    say("Working out the code\xE2\x80\xA6");
    auto k = dht::derive(*words, opts_.dht_fast_kdf);
    auto h = udp_hub::open(0, opts_.internet_loopback);
    if (!k || !h) return finish("Could not use the code on this computer.");
    std::shared_ptr<udp_hub> hub = std::move(*h);
    struct hub_guard {
      std::shared_ptr<udp_hub>& h;
      ~hub_guard() { h->stop(); }
    } stop_hub{hub};
    dht::client client(hub, dht_options());
    say("Joining the public DHT\xE2\x80\xA6");
    if (!client.start(stopped)) {
      if (stopped()) return finish("Cancelled. Nothing was sent.");
      return finish("The public DHT did not answer: this network may block UDP, or there is no connection. "
                    "Nothing was sent.");
    }
    dht::meet_options mo;
    mo.loopback = opts_.internet_loopback;
    mo.wait_ms = opts_.code_lifetime_ms;
    auto met = dht::meet(hub, client, *k, dht::role::sender, mo, stopped, say);
    if (stopped()) return finish("Cancelled. Nothing was sent.");
    if (!met) {
      return finish(met.error() == status::timeout
                        ? "Could not reach the other computer with these words: they may be mistyped, used or "
                          "expired, or both networks block a direct path (two strict NATs, or no UDP). Check the "
                          "words, or ask for a code made where the router can open a port. Nothing was sent."
                        : "Could not reach the other computer. Nothing was sent.");
    }
    std::shared_ptr<link> l(std::move(*met));
    {
      std::lock_guard lock(m_);
      if (stopping_.load()) return finish("Cancelled. Nothing was sent.");
      opening_.push_back(l);
    }
    channel::options o;
    o.static_key = static_key_;
    const std::vector<std::uint8_t> pw(k->password.begin(), k->password.end());
    auto opened = channel::open_internet(l, pw, id_, opts_.my_name, o);
    forget_opening(l);
    if (cancel_.load()) return finish("Cancelled. Nothing was sent.");
    if (!opened) {
      return finish("The words did not work: they were mistyped, used, or have expired. Ask for new ones. "
                    "Nothing was sent.");
    }
    // The link is up: this side's items now say "gone", while the send runs.
    std::thread gone([&client, &k] { dht::withdraw(client, *k, dht::role::sender, {}); });
    std::shared_ptr<channel> ich(std::move(*opened));
    name = ich->peer_name();
    send_on(std::move(ich), std::move(files), name, total, {}, std::nullopt);
    gone.join();
    return;
  }
  if (code) {
    const auto connect = [this](const net::endpoint& to, int ms) -> result<std::unique_ptr<link>> {
      auto c = opts_.connect_wifi ? opts_.connect_wifi(to, ms) : net::connect(to, ms);
      if (!c) return err(c.error());
      return std::unique_ptr<link>(std::make_unique<bearer_as>(std::move(*c), transport::internet));
    };
    const auto stopped = [this] { return cancel_.load() || stopping_.load(); };
    std::unique_ptr<link> reached;
    for (int attempt = 1; !reached; ++attempt) {
      {
        std::lock_guard lock(m_);
        v_.out_attempt = attempt;
      }
      post();
      auto race = std::make_unique<link_race>();
      start_round(*race, code->places, connect);
      reached = race->wait(stopped);
      const int won = race->winner_index();
      const auto outcomes = race->outcomes();
      races.push_back(std::move(race));
      if (stopped()) {
        if (reached) reached->shutdown();
        return finish("Cancelled. Nothing was sent.");
      }
      if (reached) {
        if (won >= 0 && static_cast<std::size_t>(won) < code->places.size()) internet_place = code->places[won];
        break;
      }
      const auto mine = net::local_addresses();
      const bool here_v6 =
          std::any_of(mine.begin(), mine.end(), [](const net::address& a) { return a.v6 && net::is_public(a); });
      {
        std::lock_guard lock(m_);
        v_.out_note = why_unreachable(code->places, outcomes, here_v6);
      }
      post();
      // Then the next round, after 1, 2, 5, 10 and then every 30 seconds.
      std::unique_lock lock(m_);
      decided_.wait_for(lock, std::chrono::milliseconds(backoff_ms(attempt)), stopped);
      if (stopped()) {
        lock.unlock();
        return finish("Cancelled. Nothing was sent.");
      }
    }
    std::shared_ptr<link> l(std::move(reached));
    {
      std::lock_guard lock(m_);
      if (stopping_.load()) return finish("Cancelled. Nothing was sent.");
      opening_.push_back(l);
    }
    channel::options o;
    o.static_key = static_key_;
    auto opened = channel::open_internet(l, code->secret, id_, opts_.my_name, o);
    forget_opening(l);
    if (cancel_.load()) return finish("Cancelled. Nothing was sent.");
    if (!opened) {
      return finish("The code did not work: it was mistyped, has been used, or has expired. Ask for a new one. "
                    "Nothing was sent.");
    }
    std::shared_ptr<channel> ich(std::move(*opened));
    name = ich->peer_name();
    peer_id.clear();
    return send_on(std::move(ich), std::move(files), name, total, peer_id, internet_place);
  }

  // Wi-Fi when the peer is on this network; Bluetooth only for a small set.
  result<std::unique_ptr<link>> l = err(status::not_found);
  if (wifi && !cancel_.load()) {
    l = opts_.connect_wifi ? opts_.connect_wifi(*wifi, kConnectMs) : net::connect(*wifi, kConnectMs);
  }
  if (!l && !bt_handle.empty() && !cancel_.load()) {
    if (total > kBluetoothMaxBytes && protocol < 2) {
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
  auto opened = open_tracked(std::shared_ptr<link>(std::move(*l)), true, protocol);
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
  send_on(std::move(ch), std::move(files), name, total, peer_id, std::nullopt);
}

void engine::send_on(std::shared_ptr<channel> ch, std::vector<outgoing_file> files, std::string name,
                     std::uint64_t total, std::string peer_id, std::optional<net::endpoint> internet_place) {
  auto finish = [&](std::string note) {
    {
      std::lock_guard lock(m_);
      v_.out = view::out_phase::ended;
      v_.out_note = std::move(note);
      out_expected_.clear();
    }
    end_transfer();
  };
  const bool internet = ch->internet();
  {
    std::lock_guard lock(m_);
    live_ = ch;
    // A code typed here (nearby), or, over the internet, the name confirmed
    // here: the CPace code already proved the peer, and the check is there
    // to compare if the two people want (#472).
    v_.out = internet ? view::out_phase::naming : view::out_phase::confirming;
    v_.out_via = transport_name(ch->via());
    v_.out_reconnecting = false;
    v_.out_protocol = ch->version();
    v_.out_peer = name;
    if (internet) {
      v_.out_check = ch->code();
      v_.out_note.clear();
    } else {
      out_expected_ = digits_of(ch->code());
    }
  }
  post();
  // #472: protocol 2 moves between bearers while the send runs (a drop, or
  // Wi-Fi appearing under a Bluetooth send); the mover does it.
  std::atomic<bool> mover_stop{false};
  std::thread mover;
  struct mover_guard {
    engine& e;
    std::atomic<bool>& stop;
    std::thread& t;
    ~mover_guard() {
      stop.store(true);
      {
        std::lock_guard lock(e.m_);
        e.mover_cv_.notify_all();
      }
      if (t.joinable()) t.join();
    }
  } stop_mover{*this, mover_stop, mover};
  if (ch->resumable()) {
    channel* raw = ch.get();
    ch->set_on_link([this, raw](link_event e) { on_link_event(true, e, *raw); });
    mover = std::thread([this, ch, peer_id, internet_place, &mover_stop] {
      try {
        run_mover(ch, peer_id, mover_stop, internet_place);
      } catch (...) {
      }
    });
  }
  // The person types the code the other screen shows before the offer (even
  // the files' names) leaves (#442): a machine that only claimed the peer's
  // id, or one in the middle, shows another code there or none, so what is
  // typed cannot match this link's. Three wrong ones end the link.
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
    if (confirmed == 3) {
      (void)ch->try_send(msg::cancel, "{}");
      ch->shutdown();
      return finish("The code was wrong " + std::to_string(kCodeTries) +
                    " times, so the link to " + name + " ended. Nothing was sent.");
    }
    if (confirmed == 1) break;
    if (std::chrono::steady_clock::now() - asked > kAskFor) {
      (void)ch->try_send(msg::cancel, "{}");
      ch->shutdown();
      return finish(internet ? "Nobody confirmed the name, so nothing was sent."
                             : "Nobody typed the code, so nothing was sent.");
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
    out_expected_.clear();
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

void engine::on_link_event(bool sending, link_event e, const channel& ch) {
  {
    std::lock_guard lock(m_);
    bool& reconnecting = sending ? v_.out_reconnecting : v_.in_reconnecting;
    reconnecting = e != link_event::up;
    if (e == link_event::up) (sending ? v_.out_via : v_.in_via) = transport_name(ch.via());
    if (sending) {
      mover_kick_ = true;
      mover_cv_.notify_all();
    }
  }
  post_throttled();
}

namespace {

// A connect that is not the first of a send is shorter: the bearer that
// answers at all answers quickly, and a race's slow loser is joined at the end.
constexpr int kRaceWifiMs = 4000;
constexpr int kRaceBluetoothMs = kConnectMs;

}  // namespace

void engine::run_mover(std::shared_ptr<channel> ch, std::string peer_id, const std::atomic<bool>& stop,
                       std::optional<net::endpoint> internet_place) {
  using clock = std::chrono::steady_clock;
  clock::time_point last_move{};  // none yet: the first move for speed may come at once
  std::vector<std::unique_ptr<link_race>> races;
  auto stopped = [&] { return stop.load() || stopping_.load() || cancel_.load() || ch->closed(); };
  while (!stopped()) {
    std::optional<net::endpoint> wifi;
    std::string bt_handle;
    {
      std::unique_lock lock(m_);
      mover_cv_.wait_for(lock, std::chrono::milliseconds(500), [&] { return mover_kick_ || stopped(); });
      mover_kick_ = false;
      // Ended (done, cancelled, or gone too long): nothing to rescue.
      if (stopped()) break;
      const auto it = peers_.find(peer_id);
      if (it != peers_.end()) {
        const auto now = clock::now();
        if (it->second.wifi && now - it->second.wifi_seen < std::chrono::milliseconds(kPeerForgetMs)) {
          wifi = it->second.wifi;
        }
        if (!it->second.bt_handle.empty() && now - it->second.bt_seen < std::chrono::milliseconds(kPeerForgetMs)) {
          bt_handle = it->second.bt_handle;
        }
      }
    }
    // Races whose connects have all returned are joined as they go (never
    // waiting on a slow one here); the rest at the end.
    std::erase_if(races, [](const std::unique_ptr<link_race>& r) {
      if (!r->settled()) return false;
      r->join();
      return true;
    });
    const bool up = ch->link_up();
    // Dropped or silent: every bearer at once. Up on Bluetooth with the peer
    // on Wi-Fi: Wi-Fi alongside, then the switch (Wi-Fi is always the faster
    // by far, so this is also the "too slow" rule).
    const bool rescue = !up;
    // A send over the internet moves to this network too, when the peer
    // turns up on it (#472 PR 2).
    const bool faster = up && ch->via() != transport::wifi && wifi &&
                        clock::now() - last_move >= std::chrono::milliseconds(kSwitchHoldMs);
    if (!rescue && !faster) continue;
    std::shared_ptr<bluetooth> bt;
    if (rescue && !bt_handle.empty()) {
      std::lock_guard p(presence_m_);
      bt = bluetooth_;
    }
    const bool over_internet = rescue && internet_place.has_value();
    if (!wifi && !bt && !over_internet) continue;  // nowhere to try yet: the next beacon kicks this
    auto race = std::make_unique<link_race>();
    if (over_internet) {
      // Where the code's sender reached it: the receiver's listener stays up
      // while the transfer runs.
      race->add([this, to = *internet_place]() -> result<std::unique_ptr<link>> {
        auto c = opts_.connect_wifi ? opts_.connect_wifi(to, kRaceWifiMs) : net::connect(to, kRaceWifiMs);
        if (!c) return err(c.error());
        return std::unique_ptr<link>(std::make_unique<bearer_as>(std::move(*c), transport::internet));
      });
    }
    if (wifi) {
      race->add([this, to = *wifi] {
        return opts_.connect_wifi ? opts_.connect_wifi(to, kRaceWifiMs) : net::connect(to, kRaceWifiMs);
      });
    }
    if (bt) race->add([bt, h = bt_handle] { return bt->connect(h, kRaceBluetoothMs); });
    std::unique_ptr<link> l = race->wait(stopped);
    races.push_back(std::move(race));
    if (!l) continue;
    if (stopped()) {
      l->shutdown();
      break;
    }
    if (auto r = ch->rejoin(std::shared_ptr<link>(std::move(l))); r) {
      last_move = clock::now();
    } else if (r.error() == status::cancelled || r.error() == status::busy) {
      break;  // the session ended (or flapped and ended)
    } else {
      host_.log(1, "sharepoint: a rejoin did not complete");
    }
  }
  for (auto& r : races) r->join();
}

// ---- receiving --------------------------------------------------------------------------

void engine::run_receive(std::unique_ptr<link> incoming, bool internet) {
  std::shared_ptr<link> l(std::move(incoming));
  // #472 PR 2: an internet sender spends the code (its first attempt, right
  // or wrong); the listener and mappings stay until this receive ends.
  std::shared_ptr<internet_receiver> in;
  struct release_guard {
    std::shared_ptr<internet_receiver>& in;
    bool held = false;
    ~release_guard() {
      if (held && in) in->release();
    }
  } held{in};
  result<std::unique_ptr<channel>> opened = err(status::not_found);
  if (!internet) {
    opened = open_tracked(l, false);
  } else {
    {
      std::lock_guard lock(m_);
      if (stopping_.load()) return end_transfer();
      opening_.push_back(l);
      in = internet_;
    }
    auto h = channel::read_hello(*l, std::chrono::steady_clock::now() + std::chrono::milliseconds(kConnectMs));
    forget_opening(l);
    // A rejoin with nothing running here, or not a code's sender: closed.
    if (!h || !h->internet || !in) {
      l->shutdown();
      return end_transfer();
    }
    auto secret = in->spend();
    if (!secret) {
      l->shutdown();
      return end_transfer();
    }
    held.held = true;
    opened = open_tracked(l, false, kProtocolVersion, &*h, &*secret);
    sodium_memzero(secret->data(), secret->size());
    if (!opened && opened.error() == status::permission_denied) in->wrong_attempt();
  }
  if (!opened) return end_transfer();  // not a MediaViewer, or gone: nothing to show
  std::shared_ptr<channel> ch(std::move(*opened));
  const std::string name = ch->peer_name();
  if (ch->resumable()) {
    channel* raw = ch.get();
    ch->set_on_link([this, raw](link_event e) { on_link_event(false, e, *raw); });
  }
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
      v_.in_reconnecting = false;
      v_.in_protocol = ch->version();
      v_.in_internet = ch->internet();
      v_.in_note.clear();
      // Nothing of an earlier offer stays on screen while this one connects.
      v_.in_names.clear();
      v_.in_count = 0;
      v_.in_total = 0;
      v_.in_bytes = 0;
      v_.in_files = 0;
      open_window = !screen_open_;
    }
  }
  // #452: a system notification (the host shows it unless this window is in
  // front) that opens the window; one without notifications opens it now.
  // Taken back however this offer ends.
  struct notice_guard {
    engine& e;
    bool shown = false;
    ~notice_guard() {
      if (shown) (void)e.host_.post_notice("", "");
    }
  } shown_notice{*this};
  shown_notice.shown = notice(name, 0, 0);
  if (open_window && !shown_notice.shown) host_.show_screen(true);
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
  // The count and size are known now: the notification says so (still no names).
  if (shown_notice.shown) (void)notice(name, offer->files.size(), offer->bytes);
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
  if (shown_notice.shown) {
    (void)host_.post_notice("", "");  // decided: nothing waits any more
    shown_notice.shown = false;
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

bool engine::notice(const std::string& peer, std::size_t count, std::uint64_t bytes) {
  return host_.post_notice(kNoticeTitle, notice_body(peer, count, bytes));
}

std::string notice_body(const std::string& peer, std::size_t count, std::uint64_t bytes) {
  const std::string who = peer.empty() ? std::string("A computer nearby") : peer;
  if (count == 0) return who + " wants to send you files. Open Sharepoint to see the code.";
  return who + " wants to send you " + std::to_string(count) + (count == 1 ? " file, " : " files, ") +
         size_text(bytes) + ". Open Sharepoint to accept.";
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
