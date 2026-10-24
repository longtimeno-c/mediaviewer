// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/engine.h"

#include <sodium.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <set>
#include <unordered_map>

#include "addons/driftwire/handover.h"
#include "addons/driftwire/meet.h"
#include "addons/driftwire/race.h"
#include "core/json.h"
#include "io/file_port.h"

namespace mv::driftwire {

namespace {

constexpr auto kPostEvery = std::chrono::milliseconds(100);
// The person has this long to answer an offer (or confirm the code); then it
// is called off.
constexpr auto kAskFor = std::chrono::milliseconds(kAskMs);
// Busy refusals in flight at once, at most; past it a link is just closed.
constexpr int kMaxHelpers = 4;
// #497: a sender that knows the code but never sends (a machine in the middle
// trying for a code that matches the one it shows its victim, or a flood of
// notifications). Five such links in a minute stop new nearby links for 30 s:
// at most about ten tries in the 3 minutes a person has to type a code.
constexpr std::size_t kUnconfirmedMax = 5;
constexpr auto kUnconfirmedWindow = std::chrono::seconds(60);
constexpr auto kRefuseNearbyFor = std::chrono::seconds(30);
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

std::int64_t unix_now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string changed_key_text(const std::string& name, bool sending) {
  return "This is not the " + name + " you remembered: its security key has changed, so the link was ended. " +
         (sending ? "Nothing was sent. " : "Nothing was received. ") +
         "If " + name + " was set up again, forget it here and send once nearby or with a code to remember it "
         "again. If not, someone may be pretending to be " + name + ".";
}

}  // namespace

engine::engine(engine_host& host, options opts) : host_(host), opts_(std::move(opts)) {
  (void)sodium_init();
  id_ = random_id();
  // #472 PR 4: the identity kept in the secret store, or one for this run.
  if (opts_.make_secrets) secrets_ = opts_.make_secrets();
  bool identity_kept = false;
  static_key_ = known::identity(secrets_.get(), identity_kept);
  crypto_scalarmult_base(public_key_.data(), static_key_.data());
  if (!identity_kept) secrets_.reset();  // nothing kept: nothing to remember either
  v_.can_remember = identity_kept;
  if (secrets_) devices_ = known::load(*secrets_);
  publish_known_locked();
  if (opts_.my_name.empty()) opts_.my_name = net::machine_name();
  v_.my_name = clean_name(opts_.my_name);
  // What was kept from the last run (#442). A saved folder that is gone
  // falls back to the default; it is not forgotten until another is picked.
  const prefs kept = opts_.initial ? *opts_.initial : load_prefs(opts_.prefs_path).value_or(prefs{});
  v_.receiving = kept.receiving || kept.background;  // presence starts on the control thread, below
  v_.background = kept.background;
  v_.internet = kept.internet;  // #472 PR 2: the receiver of codes, made on the control thread
  v_.dht = kept.dht;
  v_.handover = kept.handover;
  v_.relay = kept.relay;  // #472 relays
  v_.relay_server = kept.relay_server;
  // #500: the relay server's key is kept in the secret store, the address in
  // the file (one written before kept both there, until the next save).
  if (secrets_ && !v_.relay_server.empty() && v_.relay_server.find('#') == std::string::npos) {
    if (auto k = secrets_->get(known::kRelayKeyName)) {
      v_.relay_server += '#';
      v_.relay_server.append(k->begin(), k->end());
      sodium_memzero(k->data(), k->size());
    }
  }
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
  // Joined outside threads_m_: a transfer's way in (#472 PR 5) joins its
  // accept thread, which may be waiting for threads_m_ in on_incoming. A
  // link that slipped past stopping_ there is joined on the next round.
  for (;;) {
    std::thread transfer;
    std::vector<helper> helpers;
    std::vector<std::unique_ptr<dht_handover>> retired;
    {
      std::lock_guard lock(threads_m_);
      transfer = std::move(transfer_thread_);
      helpers = std::move(helpers_);
      helpers_.clear();
      retired = std::move(retired_);
      retired_.clear();
    }
    if (!transfer.joinable() && helpers.empty() && retired.empty()) break;
    if (transfer.joinable()) transfer.join();
    for (auto& h : helpers) {
      if (h.thread.joinable()) h.thread.join();
    }
    retired.clear();  // each joins its thread (#472's DHT handovers)
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
  // The wait for remembered devices (the control thread has stopped: nothing
  // starts it again).
  known_stop_.store(true);
  if (known_thread_.joinable()) known_thread_.join();
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
  std::string tree, settings;
  bool busy = false;
  const bool with_settings = host_.settings_shown();
  {
    std::lock_guard lock(m_);
    tree = build_tree(v_);
    if (with_settings) settings = build_settings_tree(v_);
    busy = transfer_running_ && !stopping_.load();
  }
  last_post_ = std::chrono::steady_clock::now();
  host_.post_tree(tree);
  if (with_settings) host_.post_settings(settings);
  // #510: every transfer's start and end posts the window, open or not
  // (end_transfer posts too), so the host hears of it here.
  if (busy != busy_shown_) {
    busy_shown_ = busy;
    host_.post_busy(busy ? kBusyWhat : "");
  }
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
    p.dht = v_.dht;
    p.handover = v_.handover;
    p.relay = v_.relay;
    p.relay_server = v_.relay_server;
  }
  {
    std::lock_guard lock(control_m_);
    save_wanted_ = false;
  }
  // #500: the key after '#' goes to the secret store and the file keeps the
  // address; with no store (the portable build) the file keeps both.
  {
    std::lock_guard s(secrets_m_);
    if (secrets_) {
      const auto hash = p.relay_server.rfind('#');
      if (hash != std::string::npos) {
        const std::span<const std::uint8_t> key(reinterpret_cast<const std::uint8_t*>(p.relay_server.data()) + hash + 1,
                                                p.relay_server.size() - hash - 1);
        auto kept = secrets_->get(known::kRelayKeyName);
        const bool same = kept && std::equal(kept->begin(), kept->end(), key.begin(), key.end());
        if (kept) sodium_memzero(kept->data(), kept->size());
        if (same || secrets_->put(known::kRelayKeyName, key)) p.relay_server.resize(hash);
      } else {
        (void)secrets_->erase(known::kRelayKeyName);
      }
    }
  }
  if (!save_prefs(opts_.prefs_path, p)) host_.log(1, "driftwire: could not keep the settings");
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
  load_unfinished();  // #530: off the caller's thread, before any receive
  std::unique_lock lock(control_m_);
  while (!stopping_.load()) {
    const auto none = std::chrono::steady_clock::time_point{};
    auto due = post_pending_ && status_due_ != none ? std::min(post_due_, status_due_)
               : post_pending_                     ? post_due_
                                                   : status_due_;
    if (announce_due_ != none) due = due == none ? announce_due_ : std::min(due, announce_due_);
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
    if (announce_due_ != none && std::chrono::steady_clock::now() >= announce_due_) {
      announce_due_ = none;
      lock.unlock();
      sync_announce();
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
  sync_announce();
  sync_internet();
  sync_known();
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
      o.mapping = opts_.internet_mapping;
      o.lifetime_ms = opts_.code_lifetime_ms;
      o.dht = v_.dht;
      o.dht_client = dht_options();
      o.dht_fast_kdf = opts_.dht_fast_kdf;
      o.force_words = opts_.internet_force_words;
      o.relay = v_.relay_server;  // #472 relays
      // #499: the router mappings held, listed while held, and what a crash left.
      o.ledger = data_file("driftwire_mappings.dat");
      if (!o.ledger.empty()) o.leftovers = {o.ledger, data_file("driftwire_way_in.dat")};
    }
  }
  if (drop) {
    drop.reset();  // its code, listener and mappings go, on this thread
    post();
    return;
  }
  if (!make) return;
  auto in = std::make_shared<internet_receiver>(
      o, [this](std::unique_ptr<link> l) { on_internet_link(std::move(l), true); },
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
  std::string relay_now;
  bool dht_now = false;
  {
    std::lock_guard lock(m_);
    internet_ = in;
    relay_now = v_.relay_server;
    dht_now = v_.dht;
  }
  // A setting changed while the receiver was made, when there was no
  // receiver to tell: it hears it now.
  if (relay_now != o.relay) in->set_relay(std::move(relay_now));
  if (dht_now != o.dht) in->set_dht(dht_now);
}

// ---- presence -----------------------------------------------------------------------

std::string engine::id() const {
  std::lock_guard lock(m_);
  return id_;
}

void engine::start_presence() {
  std::lock_guard p(presence_m_);
  if (presence_on_) return;
  presence_on_ = true;
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
  } else {
    host_.log(1, "driftwire: could not listen");
  }
  announce_locked();
}

bool engine::quiet_locked() const noexcept {
  return !transfer_running_ && waiting_peer_.empty() && opening_.empty() && !live_;
}

void engine::announce_locked() {
  if (discovery_) discovery_->stop();
  discovery_.reset();
  if (bluetooth_) bluetooth_->stop();
  bluetooth_.reset();
  // #501: a new id each time, unless something is under way (a peer in the
  // middle of a transfer knows this side by the id its ident said). The name
  // goes out only while the window is open: a closed window is a machine that
  // receives in the background, maybe for weeks, and anyone in range would
  // otherwise read the owner's name off it. A sender learns it from ident.
  std::string id;
  bool named = false;
  {
    std::lock_guard lock(m_);
    if (quiet_locked()) {
      old_id_ = std::exchange(id_, random_id());
    }
    id = id_;
    named = screen_open_;
  }
  announced_named_ = named;
  rotate_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(opts_.id_rotate_ms);
  const std::string name = named ? opts_.my_name : std::string();  // "" goes out as "MediaViewer"
  bool wifi = false;
  if (listener_) {
    discovery_ = opts_.make_discovery ? opts_.make_discovery() : wifi_discovery();
    beacon self;
    self.id = id;
    self.name = name;
    self.port = listener_->port();
    self.protocol = kProtocolVersion;  // #472: "p", beside the "v" protocol 1 checks
    wifi = discovery_ && discovery_->start(
                             self, [this](const beacon& b, const std::string& a) { on_beacon(b, a); },
                             [this] { on_tick(); });
    if (!wifi) host_.log(1, "driftwire: discovery could not start");
  }
  std::string bt_note;
  if (opts_.use_bluetooth) {
    std::unique_ptr<bluetooth> bt = opts_.make_bluetooth ? opts_.make_bluetooth() : make_bluetooth();
    if (!bt) {
      bt_note = "Bluetooth is not available here.";
    } else {
      std::string why;
      if (bt->start(
              id, name, [this](const bt_peer& peer) { on_bt_peer(peer); },
              [this](std::unique_ptr<link> l) { on_incoming(std::move(l), false, std::nullopt, "bluetooth"); },
              why)) {
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

void engine::sync_announce() {
  std::chrono::steady_clock::time_point next{};
  {
    std::lock_guard p(presence_m_);
    if (!presence_on_ || opts_.offline || stopping_.load()) return;
    bool named = false, quiet = false;
    {
      std::lock_guard lock(m_);
      named = screen_open_;
      quiet = quiet_locked();
    }
    const auto now = std::chrono::steady_clock::now();
    // With the window open the id stays (a tile that changed under the
    // person's pointer would be another tile); it changes as the window
    // opens or closes, so the named and unnamed ids are not one machine.
    const bool due = named != announced_named_ || (!named && now >= rotate_at_);
    if (due && quiet) {
      announce_locked();
      if (!named) next = rotate_at_;
    } else if (due) {
      next = now + std::chrono::seconds(5);  // after what is under way
    } else if (!named) {
      next = rotate_at_;
    }
  }
  std::lock_guard lock(control_m_);
  announce_due_ = next;
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

bool engine::make_room_locked(const std::string& address) {
  const auto now = std::chrono::steady_clock::now();
  const auto fresh = [&](std::chrono::steady_clock::time_point t) {
    return t.time_since_epoch().count() != 0 && now - t < std::chrono::milliseconds(kPeerForgetMs);
  };
  const auto heard = [](const peer& p) { return std::max(p.wifi_seen, p.bt_seen); };
  const auto wifi_only = [&](const peer& p) { return p.wifi && !(!p.bt_handle.empty() && fresh(p.bt_seen)); };
  // The least recently heard of those `which` takes, never the peer a send
  // is for (or waits for, or is picked).
  const auto evict = [&](auto which) {
    auto oldest = peers_.end();
    for (auto it = peers_.begin(); it != peers_.end(); ++it) {
      const std::string& id = it->first;
      if (id == v_.selected_peer || id == waiting_peer_ || (transfer_running_ && id == v_.out_peer_id)) continue;
      if (!which(it->second)) continue;
      if (oldest == peers_.end() || heard(it->second) < heard(oldest->second)) oldest = it;
    }
    if (oldest == peers_.end()) return false;
    peers_.erase(oldest);
    return true;
  };
  if (!address.empty()) {
    // One address, a few ids (loopback is this computer: its windows).
    const bool loopback = address.rfind("127.", 0) == 0;
    const auto same = [&](const peer& p) { return p.wifi && p.wifi->address == address; };
    if (!loopback && static_cast<std::size_t>(std::count_if(peers_.begin(), peers_.end(), [&](const auto& kv) {
                       return same(kv.second);
                     })) >= kMaxIdsPerAddress &&
        !evict(same)) {
      return false;
    }
    if (static_cast<std::size_t>(std::count_if(peers_.begin(), peers_.end(), [&](const auto& kv) {
          return wifi_only(kv.second);
        })) >= kMaxPeers - kBluetoothRoom &&
        !evict(wifi_only)) {
      return false;
    }
  }
  if (peers_.size() >= kMaxPeers && !evict([](const peer&) { return true; })) return false;
  return true;
}

void engine::on_beacon(const beacon& b, const std::string& address) {
  // #502: a beacon from off this machine's link is not nearby.
  if (const auto ip = net::parse_address(address); !ip || !on_link_source(*ip)) return;
  bool changed = false;
  bool ready = false;
  {
    std::lock_guard lock(m_);
    if (b.id == id_ || b.id == old_id_) return;  // our own, looped back
    const auto now = std::chrono::steady_clock::now();
    auto it = peers_.find(b.id);
    if (it == peers_.end()) {
      if (!make_room_locked(address)) return;
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
    // #497: never lowered. A beacon sent in its name with no "p" would
    // otherwise talk this side down to protocol 1 (no ML-KEM, no rejoin).
    p.wifi_protocol = std::max(p.wifi_protocol, b.protocol);
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
  if (!valid_id(b.id)) return;
  bool changed = false;
  {
    std::lock_guard lock(m_);
    if (b.id == id_ || b.id == old_id_) return;
    const auto now = std::chrono::steady_clock::now();
    auto it = peers_.find(b.id);
    if (it == peers_.end()) {
      if (!make_room_locked({})) return;
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
    p.bt_protocol = std::max(p.bt_protocol, b.protocol);  // never lowered (#497)
    refresh_peers_locked();
    changed = std::exchange(peers_changed_, false);
  }
  if (changed) post_throttled();
}

bool engine::on_link_source(const net::address& a) {
  if (opts_.local_subnets) return net::on_link(a, *opts_.local_subnets);
  if (opts_.loopback_only) return true;  // the tests' made-up addresses
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(subnets_m_);
  const bool stale = now - subnets_at_ > std::chrono::seconds(30);  // a network change
  if (!stale && net::on_link(a, subnets_)) return true;
  if (!stale && now - subnets_at_ < std::chrono::seconds(2)) return false;
  subnets_ = net::local_subnets();
  subnets_at_ = now;
  return net::on_link(a, subnets_);
}

void engine::accept_loop() {
  for (;;) {
    net::listener* l = listener_.get();
    if (!l || stopping_.load()) return;
    net::address from;
    auto c = l->accept_from(500, from);
    if (c) {
      // #502: nearby means on this machine's link, not anything routable.
      if (!on_link_source(from)) {
        (*c)->shutdown();
        continue;
      }
      on_incoming(std::move(*c), false, std::nullopt, net::address_text(from));
      continue;
    }
    if (c.error() == status::cancelled) return;
    if (c.error() != status::timeout) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

// ---- known devices (#472 PR 4) ----------------------------------------------------------

void engine::publish_known_locked() {
  v_.known.clear();
  v_.known_own.clear();
  v_.known_ids.clear();
  for (const auto& d : devices_) {
    v_.known.push_back(d.name);
    v_.known_own.push_back(d.own);
    v_.known_ids.push_back(relay::fingerprint(d.key));
  }
  if (last_pair_) {
    v_.remembered = std::any_of(devices_.begin(), devices_.end(),
                                [&](const known::device& d) { return d.key == last_pair_->key; });
  }
}

std::optional<std::array<std::uint8_t, 32>> engine::identity_to_present() const {
  std::lock_guard lock(m_);
  if (!v_.can_remember) return std::nullopt;
  return public_key_;
}

void engine::offer_remember(const channel& ch, const std::string& name, bool sending,
                            const std::optional<std::array<std::uint8_t, 32>>& presented) {
  if (ch.version() < 2) return;
  // Over the internet the handshake's static came after CPace: it is the
  // peer's identity, proved by the handshake. Nearby it was the link's own
  // key (#501), so the identity is the one said after the code and Accept;
  // a peer from before #501 says none (its static was its identity, but it
  // would remember this side by this link's key and never meet it), so
  // Remember is not offered until it updates.
  const auto key = ch.internet() ? ch.peer_key() : presented;
  if (!key) return;
  std::lock_guard lock(m_);
  if (!v_.can_remember) return;
  known::device d;
  d.name = name;
  d.key = *key;
  d.secret = ch.pairing_secret();
  // #502: already remembered, it keeps the secret both sides hold (and its
  // mark): ticking Remember again must not replace it here alone.
  if (const auto it = std::find_if(devices_.begin(), devices_.end(), [&](const known::device& k) { return k.key == *key; });
      it != devices_.end()) {
    d.secret = it->secret;
    d.own = it->own;
  }
  last_pair_ = std::move(d);
  v_.remember_offer = true;
  v_.remember_peer = name;
  // #497: a nearby code proves the receiver to the sender only. The receiver
  // keeps the sender once its person types the check the sender's window
  // shows; over the internet the receiver made the code itself (CPace), so
  // the sender is proved both ways and nothing more is asked.
  const bool nearby = !ch.internet();
  v_.remember_check = sending && nearby ? ch.remember_check() : std::string();
  v_.remember_needs_check = !sending && nearby;
  v_.remember_check_note.clear();
  remember_expected_ = v_.remember_needs_check ? digits_of(ch.remember_check()) : std::string();
  remember_tries_left_ = kCodeTries;
  publish_known_locked();
}

void engine::set_remembered(bool on) {
  bool full = false;
  {
    std::lock_guard lock(m_);
    if (!last_pair_ || !v_.remember_offer) return;
    if (on && v_.remember_needs_check) return;  // the sender's check comes first (#497)
    const auto it = std::find_if(devices_.begin(), devices_.end(),
                                 [&](const known::device& d) { return d.key == last_pair_->key; });
    if (it != devices_.end()) {
      // #502: unticked and ticked again, it comes back with the secret and
      // mark it had, not this session's alone.
      last_pair_->secret = it->secret;
      last_pair_->own = it->own;
    }
    if (on && it != devices_.end()) {
      it->name = last_pair_->name;  // already remembered: as it was
    } else if (on) {
      if (devices_.size() >= known::kMaxDevices) {
        // The oldest goes, never one marked as this person's own (#502).
        const auto old = std::find_if(devices_.begin(), devices_.end(), [](const known::device& d) { return !d.own; });
        if (old == devices_.end()) {
          full = true;
          v_.out_note += " Not remembered: forget one of your " + std::to_string(known::kMaxDevices) +
                         " remembered computers first.";
        } else {
          devices_.erase(old);
        }
      }
      if (!full) devices_.push_back(*last_pair_);
    } else if (it != devices_.end()) {
      devices_.erase(it);
    }
    publish_known_locked();
  }
  if (full) {
    post();
    return;
  }
  // The store on this worker (never the UI thread), one call at a time.
  {
    std::vector<known::device> copy;
    {
      std::lock_guard lock(m_);
      copy = devices_;
    }
    std::lock_guard s(secrets_m_);
    if (secrets_ && !known::save(*secrets_, copy)) host_.log(1, "driftwire: the remembered devices were not kept");
  }
  request_reconcile();
  post();
}

void engine::forget_known(const std::string& fingerprint) {
  std::vector<known::device> copy;
  {
    std::lock_guard lock(m_);
    const std::size_t index = known_index_locked(fingerprint);
    if (index >= devices_.size()) return;
    devices_.erase(devices_.begin() + static_cast<std::ptrdiff_t>(index));
    publish_known_locked();
    copy = devices_;
  }
  {
    std::lock_guard s(secrets_m_);
    if (secrets_ && !known::save(*secrets_, copy)) host_.log(1, "driftwire: the remembered devices were not kept");
  }
  request_reconcile();
  post();
}

// ---- unfinished receives (#530) -------------------------------------------------------

void engine::load_unfinished() {
  const std::string path = data_file(kResumeFile);
  if (path.empty()) return;
  auto records = load_resume(path);
  const auto now = static_cast<std::int64_t>(std::time(nullptr));
  const std::size_t before = records.size();
  std::erase_if(records, [&](const resume_record& r) {
    if (now - r.saved <= kResumeKeepS) return false;
    discard_partials(r);  // a week old: it goes, with what it kept
    return true;
  });
  const bool expired = records.size() != before;
  {
    std::lock_guard lock(m_);
    unfinished_ = std::move(records);
    publish_unfinished_locked();
  }
  if (expired) save_unfinished();
  post();
}

void engine::publish_unfinished_locked() {
  v_.unfinished.clear();
  for (const auto& r : unfinished_) {
    view::unfinished_row row;
    row.id = r.sender.substr(0, 16);
    row.peer = r.peer.empty() ? std::string("a computer") : r.peer;
    row.count = r.count;
    row.bytes = r.bytes;
    row.bytes_here = r.bytes_here();
    v_.unfinished.push_back(std::move(row));
  }
}

void engine::save_unfinished() {
  const std::string path = data_file(kResumeFile);
  if (path.empty()) return;
  std::lock_guard io(unfinished_io_m_);
  std::vector<resume_record> copy;
  {
    std::lock_guard lock(m_);
    copy = unfinished_;
  }
  if (!save_resume(path, copy)) host_.log(1, "driftwire: could not keep the unfinished transfers");
}

void engine::discard_unfinished(const std::string& id) {
  resume_record gone;
  {
    std::lock_guard lock(m_);
    const auto it = std::find_if(unfinished_.begin(), unfinished_.end(),
                                 [&](const resume_record& r) { return r.sender.rfind(id, 0) == 0 && id.size() == 16; });
    if (it == unfinished_.end()) return;
    gone = std::move(*it);
    unfinished_.erase(it);
    publish_unfinished_locked();
  }
  discard_partials(gone);  // outside m_: file I/O
  save_unfinished();
  post();
}

void engine::keep_unfinished(resume_record record) {
  std::vector<resume_record> gone;
  {
    std::lock_guard lock(m_);
    const auto it = std::find_if(unfinished_.begin(), unfinished_.end(),
                                 [&](const resume_record& r) { return r.sender == record.sender; });
    if (it != unfinished_.end()) {
      // What the old record kept and this one does not use goes now.
      resume_record old = std::move(*it);
      unfinished_.erase(it);
      std::erase_if(old.files, [&](const resume_file& f) {
        return f.whole || std::any_of(record.files.begin(), record.files.end(),
                                      [&](const resume_file& g) { return g.temp_path == f.temp_path; });
      });
      gone.push_back(std::move(old));
    }
    unfinished_.insert(unfinished_.begin(), std::move(record));
    while (unfinished_.size() > kMaxResumeRecords) {
      gone.push_back(std::move(unfinished_.back()));
      unfinished_.pop_back();
    }
    publish_unfinished_locked();
  }
  for (const auto& r : gone) discard_partials(r);  // outside m_: file I/O
  save_unfinished();
  post_throttled();
}

void engine::drop_unfinished(const std::string& sender) {
  resume_record gone;
  {
    std::lock_guard lock(m_);
    const auto it = std::find_if(unfinished_.begin(), unfinished_.end(),
                                 [&](const resume_record& r) { return r.sender == sender; });
    if (it == unfinished_.end()) return;
    gone = std::move(*it);
    unfinished_.erase(it);
    publish_unfinished_locked();
  }
  discard_partials(gone);  // a partial this receive took up is already renamed or dropped
  save_unfinished();
  post();
}

void engine::sync_known() {
  bool want = false;
  {
    std::lock_guard lock(m_);
    want = !stopping_.load() && !opts_.offline && v_.internet && v_.dht && !devices_.empty() && visible_locked();
  }
  if (want == known_thread_.joinable()) return;
  if (!want) {
    known_stop_.store(true);
    known_thread_.join();
    return;
  }
  known_stop_.store(false);
  known_thread_ = std::thread([this] {
    try {
      run_known_wait();
    } catch (...) {
    }
  });
}

void engine::run_known_wait() {
  const auto stopped = [this] { return known_stop_.load() || stopping_.load(); };
  const auto rest = [&](int ms) {
    for (int waited = 0; waited < ms && !stopped(); waited += 100) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  };
  // One socket and one DHT client for every remembered device. A DHT that
  // does not answer (no connection yet) is tried again.
  std::shared_ptr<udp_hub> hub;
  std::unique_ptr<dht::client> client;
  while (!stopped()) {
    auto h = udp_hub::open(0, opts_.internet_loopback, opts_.wrap_udp);
    if (h) {
      hub = std::move(*h);
      client = std::make_unique<dht::client>(hub, dht_options());
      if (client->start(stopped)) break;
      client.reset();
      hub->stop();
      hub.reset();
    }
    rest(30000);
  }
  if (!client) return;
  struct pair_state {
    std::int64_t hour = -1;
    std::unique_ptr<dht::keys> k;
    std::int64_t answered_ms = 0;  // the newest sender's item met
    std::int64_t seq = 0;          // the last withdraw's (a meeting after it publishes above it)
    bool live = false;             // a meeting's item may still be up
    int salt = 0;
  };
  std::map<std::array<std::uint8_t, 32>, pair_state> pairs;
  // A meeting's item says "gone" once it is over, met or not: a send later
  // this hour must not punch toward a meeting that has ended.
  const auto withdraw = [&](pair_state& s, const std::function<bool()>& stop) {
    s.seq = dht::withdraw(*client, *s.k, dht::role::receiver, stop, s.seq);
    s.live = false;
  };
  while (!stopped()) {
    std::vector<known::device> devices;
    bool busy = false;
    std::vector<net::address> outside;
    for (const auto& v : client->external_votes()) {
      const auto ip = net::parse_address(v.address);
      if (ip && !ip->v6 && std::find(outside.begin(), outside.end(), *ip) == outside.end()) outside.push_back(*ip);
    }
    {
      std::lock_guard lock(m_);
      devices = devices_;
      busy = transfer_running_;
      outside_seen_ = std::move(outside);
    }
    std::erase_if(pairs, [&](const auto& p) {
      return std::none_of(devices.begin(), devices.end(), [&](const known::device& d) { return d.key == p.first; });
    });
    const std::int64_t hour = known::hour_of(unix_now_ms());
    for (const auto& d : devices) {
      if (stopped()) break;
      auto& s = pairs[d.key];
      if (s.hour != hour) {
        s.k = std::make_unique<dht::keys>(known::presence_keys(d, hour, public_key_));
        s.hour = hour;
        s.seq = 0;
      }
      if (busy) continue;  // a sender knocking now is met after this transfer
      // A sender knocking: one of its salts per look, in turn. Nothing of
      // this side is published until one does: the meeting's item, with its
      // TCP port, is the first the sender finds, so both punch at once.
      const std::string salt = dht::salt_of(*s.k, dht::role::sender, s.salt++ % dht::kTargets);
      auto got = client->get(s.k->public_key, salt, stopped);
      if (!got) continue;
      auto c = dht::open(*s.k, salt, got->v, unix_now_ms());
      if (!c || c->gone || c->written_ms <= s.answered_ms) continue;
      s.answered_ms = c->written_ms;
      dht::meet_options mo;
      mo.loopback = opts_.internet_loopback;
      mo.wait_ms = 30000;
      mo.punch_ms = known::kPunchMs;
      mo.tcp = true;  // as four words: where UDP cannot be punched, TCP often can
      mo.seq_floor = s.seq + 1;
      // A path the sender agrees on after its handshake over the first failed (#499).
      mo.later = [this, d, hour](std::unique_ptr<link> l) {
        on_incoming(std::make_unique<bearer_as>(std::move(l), transport::internet), true, d, {}, hour);
      };
      s.live = true;
      auto met = dht::meet(hub, *client, *s.k, dht::role::receiver, mo, stopped, {});
      if (met) on_incoming(std::make_unique<bearer_as>(std::move(*met), transport::internet), true, d, {}, hour);
      withdraw(s, stopped);
      if (met) break;  // one at a time: the next look sees the transfer
    }
    rest(opts_.known_poll_ms);
  }
  // Stopped during a meeting: its item says "gone" (briefly; it expires anyway).
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  const auto brief = [&] { return stopping_.load() || std::chrono::steady_clock::now() >= until; };
  for (auto& [key, s] : pairs) {
    if (s.live) withdraw(s, brief);
  }
  hub->stop();
}

std::size_t engine::known_index_locked(const std::string& fingerprint) const {
  for (std::size_t i = 0; i < devices_.size(); ++i) {
    if (relay::fingerprint(devices_[i].key) == fingerprint) return i;
  }
  return devices_.size();
}

void engine::start_send_known(const std::string& fingerprint) {
  std::optional<known::device> to;
  std::vector<outgoing_file> files;
  {
    std::lock_guard lock(m_);
    const std::size_t index = known_index_locked(fingerprint);
    if (index >= devices_.size() || transfer_running_ || out_busy_locked() || v_.reading_folder) return;
    if (!v_.internet || !v_.dht) {
      v_.send_code_note = "Turn on \xE2\x80\x9C" "Find each other over the internet\xE2\x80\x9D to send to a remembered device.";
    } else if (files_.empty()) {
      v_.send_code_note = "Pick the files to send first.";
    } else {
      to = devices_[index];
      v_.send_code_note.clear();
      transfer_running_ = true;
      cancel_.store(false);
      files = files_;
      out_decision_ = 0;
      v_.remember_offer = false;
      v_.remember_needs_check = false;
      v_.remember_check.clear();
      v_.out = view::out_phase::reaching;
      v_.out_internet = true;
      v_.out_known = true;
      v_.out_attempt = 0;
      v_.out_peer = to->name;
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
  if (!to) return;
  launch_transfer([this, files = std::move(files), to = std::move(to)]() mutable {
    try {
      run_send({}, std::move(files), std::nullopt, std::nullopt, std::move(to));
    } catch (...) {
      fail_transfer(true);
    }
  });
}

void engine::on_internet_link(std::unique_ptr<link> l, bool code) {
  if (!l) return;
  on_incoming(std::make_unique<bearer_as>(std::move(l), transport::internet), true, std::nullopt, {}, -1, code);
}

void engine::note_unconfirmed() {
  std::lock_guard lock(m_);
  const auto now = std::chrono::steady_clock::now();
  std::erase_if(unconfirmed_, [&](const auto& t) { return now - t > kUnconfirmedWindow; });
  unconfirmed_.push_back(now);
  if (unconfirmed_.size() >= kUnconfirmedMax) {
    refuse_nearby_until_ = now + kRefuseNearbyFor;
    unconfirmed_.clear();
    host_.log(1, "driftwire: too many nearby links with no offer; refusing new ones for a while");
  }
}

void engine::on_incoming(std::unique_ptr<link> l, bool internet, std::optional<known::device> from_known,
                         std::string source, std::int64_t known_hour, bool code) {
  if (!l || stopping_.load()) return;
  bool busy = false;
  {
    std::lock_guard lock(m_);
    busy = transfer_running_;
    // #502: a source that started kReceivesPerMinute receives this minute
    // starts none (a busy answer or a rejoin is not one). #497: a run of
    // nearby links that never offered: new ones wait (a rejoin of the
    // transfer under way comes while busy, and is not held).
    if (!busy && ((!internet && !from_known && std::chrono::steady_clock::now() < refuse_nearby_until_) ||
                  !receive_limit_.allow(source, std::chrono::steady_clock::now()))) {
      l->shutdown();
      return;
    }
    if (!busy) {
      transfer_running_ = true;
      cancel_.store(false);
      v_.remember_offer = false;
      v_.remember_needs_check = false;
      v_.remember_check.clear();
    }
  }
  if (busy) {
    std::lock_guard threads(threads_m_);
    reap_helpers();
    if (static_cast<int>(helpers_.size()) >= kMaxHelpers) return;  // the link closes as it goes
    auto done = std::make_shared<std::atomic<bool>>(false);
    helpers_.push_back({std::thread([this, done, internet, code, link = std::shared_ptr<driftwire::link>(std::move(l))]() mutable {
                          try {
                            route_busy(std::move(link), internet, code);
                          } catch (...) {
                          }
                          done->store(true);
                        }),
                        done});
    return;
  }
  launch_transfer([this, internet, code, known_hour, link = std::move(l), from_known = std::move(from_known),
                   source = std::move(source)]() mutable {
    try {
      run_receive(std::move(link), internet, std::move(from_known), std::move(source), known_hour, code);
    } catch (...) {
      fail_transfer(false);
    }
  });
  post_throttled();  // #510: the host hears that a transfer runs (post's post_busy)
}

void engine::retire(std::unique_ptr<dht_handover> h) {
  if (!h) return;
  h->request_stop();
  std::lock_guard lock(threads_m_);
  std::erase_if(retired_, [](const std::unique_ptr<dht_handover>& r) { return r->finished(); });
  retired_.push_back(std::move(h));
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

void engine::route_busy(std::shared_ptr<link> l, bool internet, bool code) {
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
      if (auto r = ch->accept_rejoin(l, *h); !r) host_.log(1, "driftwire: a rejoin did not complete");
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
    if (internet && code && in && in->spend()) in->release();
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
  // #501: the identity only after CPace on a code or a pairing secret. A
  // nearby XX handshake sends its static to whoever connected (and the
  // client's to whatever answered) before any code is typed: there it is a
  // key for this link alone, and the identity follows sealed in the offer
  // and the accept, after the code and Accept.
  if (internet_secret) {
    o.static_key = static_key_;
    o.internet_secret = *internet_secret;
  }
  const std::string me = id();
  auto ch = read ? channel::open_server(l, *read, me, opts_.my_name, o)
                 : channel::open(l, client, me, opts_.my_name, o);
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

void engine::settings_opened() { post(); }

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
    // #498: the file itself, never a link (a folder's walk skips them too):
    // what is sent is this file, whatever is put at its path later.
    auto id = io::identify(path);
    if (!id) return;
    std::string rel(leaf_name(path));
    if (!safe_relative(rel) || housekeeping(rel)) return;
    out.push_back({path, std::move(rel), st->size, st->mtime_unix, id.value()});
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
  if (!safe_relative(top)) top = "Folder";  // a hidden folder's own name too (#498)
  (void)io::walk_files(folder, 32, [&](const io::tree_entry& e) {
    if (files_stale(gen)) return false;
    if (e.cloud_only || skipped_relative(e.relative_utf8)) return true;
    std::string rel = top + "/" + e.relative_utf8;
    if (!safe_relative(rel)) return true;
    // #498: the walk's own id where it has one (POSIX); Windows' listing
    // gives none, so the file is looked at once (its attributes only).
    io::file_id id = e.id;
    if (!id.known()) {
      auto got = io::identify(e.path_utf8);
      if (!got) return true;
      id = got.value();
    }
    out.push_back({e.path_utf8, std::move(rel), e.size, e.mtime_unix, id});
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
  if (node == "handover" && kind == kChange && value && value->k == json::kind::boolean) {
    // #472 PR 5: opt-in, off by default; for the next transfer.
    {
      std::lock_guard lock(m_);
      v_.handover = value->b;
    }
    request_save();
    post();
    return;
  }
  if (node == "relay" && kind == kChange && value && value->k == json::kind::boolean) {
    // #472 relays: opt-in, off by default; for the next send, and whether
    // this computer relays for the person's others.
    {
      std::lock_guard lock(m_);
      v_.relay = value->b;
    }
    request_save();
    post();
    return;
  }
  if (node == "relay_server" && (kind == kChange || kind == kSubmit) && text) {
    std::shared_ptr<internet_receiver> in;
    {
      std::lock_guard lock(m_);
      v_.relay_server = *text;
      in = internet_;
    }
    if (in) in->set_relay(*text);
    request_save();
    return;
  }
  if (node.rfind("own", 0) == 0 && node.size() > 3 && kind == kChange && value && value->k == json::kind::boolean) {
    // Keyed by the device's key, not its place in the list: it grants relaying.
    set_own(node.substr(3), value->b);
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
  // #472 PR 4: known devices.
  if (node == "remember" && kind == kChange && value && value->k == json::kind::boolean) {
    set_remembered(value->b);
    return;
  }
  if (node == "known" && (kind == kActivate || kind == kClick) && text && text->rfind("dev", 0) == 0) {
    start_send_known(text->substr(3));  // its fingerprint (#502)
    return;
  }
  if (node.rfind("forget", 0) == 0 && node.size() > 6 && kind == kClick) {
    forget_known(node.substr(6));
    return;
  }
  if (node.rfind("discard", 0) == 0 && node.size() > 7 && kind == kClick) {
    discard_unfinished(node.substr(7));  // #530
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
  } else if (node == "remember_check_entry" && (kind == kChange || kind == kSubmit) && text) {
    // #497: the check the sender's window shows, typed here to remember it.
    const std::string typed = digits_of(*text);
    bool keep = false;
    {
      std::lock_guard lock(m_);
      if (!v_.remember_offer || !v_.remember_needs_check || remember_expected_.empty()) return;
      if (typed.size() < remember_expected_.size()) {
        if (kind == kSubmit && !typed.empty()) v_.remember_check_note = "The code has 6 digits.";
      } else if (typed.size() == remember_expected_.size() &&
                 sodium_memcmp(typed.data(), remember_expected_.data(), typed.size()) == 0) {
        v_.remember_needs_check = false;
        v_.remember_check_note.clear();
        keep = true;
      } else if (--remember_tries_left_ <= 0) {
        v_.remember_offer = false;  // not offered again for this transfer
        v_.remember_needs_check = false;
        remember_expected_.clear();
      } else {
        v_.remember_check_note = "That is not the code on " + v_.remember_peer + ". " +
                                 plural(static_cast<std::uint32_t>(remember_tries_left_), "try", "tries") + " left.";
      }
    }
    if (keep) {
      set_remembered(true);
      return;
    }
    post();
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
  } else if (node == "out_dismiss" && kind == kClick) {
    // An end stays until it is dismissed: an error or a cancel is never
    // gone before the person has read it.
    {
      std::lock_guard lock(m_);
      if (v_.out == view::out_phase::done || v_.out == view::out_phase::ended) v_.out = view::out_phase::idle;
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
    // #511: said reliably (after the chunk being written, at most
    // kCancelWaitMs), and final: no rejoin or mover starts on either side.
    (void)ch->cancel(kCancelWaitMs);
  }
}

hooks engine::hooks_for(bool sending) {
  hooks h;
  h.cancel = &cancel_;
  h.quitting = &stopping_;  // #530
  if (!sending) h.idle_ms = opts_.receive_idle_ms;  // #502
  h.stages = sending ? opts_.send_stages : opts_.receive_stages;  // #540
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
      // #529: the speed and the time left, from the bytes moved over time.
      auto& meter = sending ? out_meter_ : in_meter_;
      meter.add(std::chrono::steady_clock::now(), bytes);
      (sending ? v_.out_rate : v_.in_rate) = meter.rate();
      (sending ? v_.out_left_s : v_.in_left_s) = meter.seconds_left(sending ? v_.out_total : v_.in_total);
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
      v_.remember_offer = false;
      v_.remember_needs_check = false;
      v_.remember_check.clear();
      v_.out_known = false;
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
  launch_transfer([this, peer_id, files = std::move(files)]() mutable {
    try {
      run_send(std::move(peer_id), std::move(files));
    } catch (...) {
      fail_transfer(true);
    }
  });
}

void engine::stay_awake(bool receiving) {
  auto held = power::hold(receiving);  // outside m_: a call into the OS
  if (!held) host_.log(1, "driftwire: the system did not agree to stay awake during the transfer");
  std::lock_guard lock(m_);
  awake_ = std::move(held);
}

void engine::end_transfer() {
  std::unique_ptr<power::stay_awake> awake;  // released outside m_
  {
    std::lock_guard lock(m_);
    awake = std::move(awake_);
    live_.reset();
    transfer_running_ = false;
    out_meter_.restart();  // the next transfer measures from its own start (#529)
    in_meter_.restart();
    v_.out_rate = v_.in_rate = 0;
    v_.out_left_s = v_.in_left_s = -1;
    idle_.notify_all();
  }
  request_reconcile();
  post();
  if (opts_.after_end_transfer) opts_.after_end_transfer();
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
      v_.remember_offer = false;
      v_.remember_needs_check = false;
      v_.remember_check.clear();
      v_.out_known = false;
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
  launch_transfer([this, files = std::move(files), code = std::move(code), words]() mutable {
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
  o.loopback = opts_.internet_loopback;  // the tests' nodes
  o.table_path = data_file("driftwire_dht.dat");
  return o;
}

std::string engine::data_file(const char* name) const {
  if (opts_.prefs_path.empty()) return {};
  const std::string dir(io::parent_of(opts_.prefs_path));
  return dir.empty() ? std::string(name) : io::join_path(dir, name);
}

void engine::run_send(std::string peer_id, std::vector<outgoing_file> files, std::optional<invite::code> code,
                      std::optional<dht::word_code> words, std::optional<known::device> to_known) {
  std::optional<net::endpoint> wifi;
  std::string bt_handle, name;
  std::uint32_t protocol = kProtocol1;
  std::uint64_t total = 0;
  for (const auto& f : files) total += f.size;
  {
    std::lock_guard lock(m_);
    v_.out_relay.clear();
    v_.out_punched = false;
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
      // The highest both speak (#497: 3; 2 to v0.1.34 and v0.1.35; 1 before #472).
      protocol = std::clamp(std::max(it->second.wifi_protocol, it->second.bt_protocol), kProtocol1, kProtocolVersion);
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
  if (to_known) {
    // #472 PR 4: a remembered device. Knock on the DHT under this hour's keys
    // of the pair (known.h) and meet once it answers (it publishes only
    // then), UDP and TCP, as long as it takes, round after round (each
    // round's item lives 10 minutes); then the internet handshake keyed by
    // the pair's secret, and its key must be the one remembered.
    const std::string& who = to_known->name;
    const auto stopped = [this] { return cancel_.load() || stopping_.load(); };
    const std::string waiting = "Waiting for " + who + " to come online\xE2\x80\xA6 It sends as soon as " + who +
                                " has Driftwire open with \xE2\x80\x9C" "Find each other over the internet\xE2\x80\x9D on. " +
                                who + " must remember this computer too.";
    auto say = [this](const std::string& s) {
      {
        std::lock_guard lock(m_);
        v_.out_note = s;
      }
      post_throttled();
    };
    say(waiting);
    auto h = udp_hub::open(0, opts_.internet_loopback, opts_.wrap_udp);
    if (!h) return finish("Could not use the internet on this computer. Nothing was sent.");
    std::shared_ptr<udp_hub> hub = std::move(*h);
    struct hub_guard {
      std::shared_ptr<udp_hub>& h;
      ~hub_guard() { h->stop(); }
    } stop_hub{hub};
    dht::client client(hub, dht_options());
    if (!client.start(stopped)) {
      if (stopped()) return finish("Cancelled. Nothing was sent.");
      return finish("The public DHT did not answer: this network may block UDP, or there is no connection. "
                    "Nothing was sent.");
    }
    std::unique_ptr<dht::keys> k;
    // The handshake over each path agreed on, before it is kept (#499).
    result<std::unique_ptr<channel>> opened = err(status::not_found);
    for (int round = 1; !opened; ++round) {
      if (stopped()) return finish("Cancelled. Nothing was sent.");
      {
        std::lock_guard lock(m_);
        v_.out_attempt = round;
      }
      const std::int64_t now = unix_now_ms();
      const std::int64_t hour = known::hour_of(now);
      k = std::make_unique<dht::keys>(known::presence_keys(*to_known, hour, to_known->key));
      dht::meet_options mo;
      mo.loopback = opts_.internet_loopback;
      // To the hour's end (both sides change keys then), at most 8 minutes.
      mo.wait_ms = static_cast<int>(std::clamp<std::int64_t>((hour + 1) * known::kHourMs - now, 5000, 8 * 60 * 1000));
      mo.punch_ms = known::kPunchMs;
      mo.tcp = true;  // as four words: where UDP cannot be punched, TCP often can
      mo.open = [&](std::unique_ptr<link>& got) {
        std::shared_ptr<link> l(std::move(got));
        {
          std::lock_guard lock(m_);
          if (stopping_.load()) return true;
          opening_.push_back(l);
        }
        channel::options o;
        o.static_key = static_key_;
        std::vector<std::uint8_t> pw(k->password.begin(), k->password.end());
        opened = channel::open_internet(l, pw, id(), opts_.my_name, o);
        sodium_memzero(pw.data(), pw.size());  // #501
        forget_opening(l);
        if (!opened) l->shutdown();
        // It could not prove it is the device remembered: no other path mends that.
        return opened.has_value() || opened.error() == status::permission_denied || stopped();
      };
      (void)dht::meet(hub, client, *k, dht::role::sender, mo, stopped, [&](const std::string& s) {
        say(s.rfind("Connecting", 0) == 0 ? s : waiting);
      });
      if (stopped()) return finish("Cancelled. Nothing was sent.");
      if (!opened && opened.error() == status::permission_denied) {
        return finish("The computer that answered could not prove it is " + who + ". Nothing was sent.");
      }
      if (!opened) say(waiting);
    }
    std::thread gone([&client, &k] { dht::withdraw(client, *k, dht::role::sender, {}); });
    struct join_guard {
      std::thread& t;
      ~join_guard() { t.join(); }
    } join_gone{gone};
    if ((*opened)->peer_key() != to_known->key) {
      (*opened)->shutdown();
      host_.log(1, "driftwire: a remembered device's key changed");
      return finish(changed_key_text(who, true));
    }
    std::shared_ptr<channel> ich(std::move(*opened));
    return send_on(std::move(ich), std::move(files), who, total, {}, std::nullopt, true);
  }
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
    // The code's keys hold the hour it was made in (#499): this hour's, and
    // the last hour's too while a code made then may still be alive.
    const std::int64_t now_ms = unix_now_ms();
    const std::int64_t hour = dht::code_hour(now_ms);
    auto k = dht::derive(*words, opts_.dht_fast_kdf, hour);
    std::optional<result<dht::keys>> k_before;
    if (dht::code_hour(now_ms - opts_.code_lifetime_ms) != hour) k_before = dht::derive(*words, opts_.dht_fast_kdf, hour - 1);
    auto h = udp_hub::open(0, opts_.internet_loopback, opts_.wrap_udp);
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
    // Made last hour: the receiver's item is under that hour's keys.
    const dht::keys* key = &*k;
    if (k_before && *k_before) {
      say("Looking for the other computer\xE2\x80\xA6");
      const auto which = dht::published_under(client, {&*k, &**k_before}, dht::role::receiver,
                                              opts_.code_lifetime_ms, 2000, stopped);
      if (stopped()) return finish("Cancelled. Nothing was sent.");
      if (which && *which == 1) key = &**k_before;
    }
    // Every way out after this side published: its items say "gone" (#499:
    // not left up after a meeting that failed).
    struct withdraw_guard {
      dht::client& c;
      const dht::keys& k;
      withdraw_guard(dht::client& client, const dht::keys& keys) : c(client), k(keys) {}
      withdraw_guard(const withdraw_guard&) = delete;
      withdraw_guard& operator=(const withdraw_guard&) = delete;
      ~withdraw_guard() {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        dht::withdraw(c, k, dht::role::sender, [&] { return std::chrono::steady_clock::now() >= until; });
      }
    };
    std::optional<withdraw_guard> gone_at_end;
    gone_at_end.emplace(client, *key);
    dht::meet_options mo;
    mo.loopback = opts_.internet_loopback;
    mo.wait_ms = opts_.code_lifetime_ms;
    mo.tcp = true;  // a word code's meeting, the window open: TCP simultaneous open too
    // The handshake over each path agreed on, before it is kept (#499): a
    // path that does not carry it gives way to the next one that answered.
    std::vector<std::uint8_t> pw(key->password.begin(), key->password.end());
    struct wipe_pw {
      std::vector<std::uint8_t>& v;
      ~wipe_pw() { sodium_memzero(v.data(), v.size()); }  // #501
    } wipe{pw};
    result<std::unique_ptr<channel>> opened = err(status::not_found);
    mo.open = [&](std::unique_ptr<link>& got) {
      std::shared_ptr<link> l(std::move(got));
      {
        std::lock_guard lock(m_);
        if (stopping_.load()) {
          opened = err(status::cancelled);
          return true;
        }
        opening_.push_back(l);
      }
      channel::options o;
      o.static_key = static_key_;
      opened = channel::open_internet(l, pw, id(), opts_.my_name, o);
      forget_opening(l);
      if (!opened) l->shutdown();
      // Wrong words, or cancelled: no other path mends that.
      return opened.has_value() || opened.error() == status::permission_denied || cancel_.load() ||
             stopping_.load();
    };
    auto met = dht::meet(hub, client, *key, dht::role::sender, mo, stopped, say);
    if (stopped()) return finish("Cancelled. Nothing was sent.");
    if (!met && met.error() != status::permission_denied) {
      return finish(met.error() == status::timeout
                        ? "Could not reach the other computer with these words: they may be mistyped, used or "
                          "expired, or both networks block a direct path (two strict NATs, or no UDP). Check the "
                          "words, or ask for a code made where the router can open a port. Nothing was sent."
                        : "Could not reach the other computer. Nothing was sent.");
    }
    if (!opened) {
      return finish("The words did not work: they were mistyped, used, or have expired. Ask for new ones. "
                    "Nothing was sent.");
    }
    // The link is up: this side's items now say "gone", while the send runs.
    gone_at_end.reset();
    std::thread gone([&client, key] { dht::withdraw(client, *key, dht::role::sender, {}); });
    // Joined however send_on leaves (#502: an exception from it with the
    // thread unjoined would have ended the app).
    struct join_guard {
      std::thread& t;
      ~join_guard() { t.join(); }
    } join_gone{gone};
    std::shared_ptr<channel> ich(std::move(*opened));
    name = ich->peer_name();
    send_on(std::move(ich), std::move(files), name, total, {}, std::nullopt);
    return;
  }
  if (code) {
    const auto connect = [this](const net::endpoint& to, int ms) -> result<std::unique_ptr<link>> {
      auto c = opts_.connect_wifi ? opts_.connect_wifi(to, ms) : net::connect(to, ms);
      if (!c) return err(c.error());
      return std::unique_ptr<link>(std::make_unique<bearer_as>(std::move(*c), transport::internet));
    };
    const auto stopped = [this] { return cancel_.load() || stopping_.load(); };
    // #499: a code's places are tried only where the receiver can be: public
    // addresses that are not this machine's own (never loopback, multicast or
    // 0.0.0.0, but the tests' loopback); its address on its own network only
    // when this network faces the internet from the code's public address
    // (behind the same router; elsewhere that address is a stranger's here).
    std::vector<net::endpoint> lan_places;
    std::vector<net::address> code_public_v4;
    {
      const auto own = net::local_addresses();
      std::vector<net::endpoint> keep;
      for (const auto& p : code->places) {
        const auto ip = net::parse_address(p.address);
        if (!ip || p.port == 0) continue;
        if (relay::may_reach(p, opts_.internet_loopback, own)) {
          keep.push_back(p);
          if (!ip->v6 && net::is_public(*ip)) code_public_v4.push_back(*ip);
        } else if (net::is_private_v4(*ip)) {
          lan_places.push_back(p);
        }
      }
      code->places = std::move(keep);
    }
    std::unique_ptr<link> reached;
    std::string relayed_by;  // #472 relays: "" direct
    std::string relay_why;   // what the person's own computers said, if asked
    for (int attempt = 1; !reached; ++attempt) {
      {
        std::lock_guard lock(m_);
        v_.out_attempt = attempt;
      }
      post();
      auto race = std::make_unique<link_race>();
      start_round(*race, code->places, connect);
      // #472 relays: the receiver's relay server, a moment after the places;
      // and, beside them, the person's own computers (a meeting over the DHT
      // takes a while, so it runs in the race, not after it). Given up as
      // soon as the round is decided.
      if (code->relay) relay::add_relay(*race, *code->relay, connect, stopped);
      auto round_over = std::make_shared<std::atomic<bool>>(false);
      struct own_try {
        std::mutex m;
        std::string via, why;
      };
      auto own = std::make_shared<own_try>();
      const int own_at = static_cast<int>(code->places.size()) + (code->relay ? 1 : 0);
      bool own_tried = false;
      if (!code->places.empty() && own_relay_possible()) {
        own_tried = true;
        race->add(
            [this, places = code->places, round_over, own]() -> result<std::unique_ptr<link>> {
              std::string via, why;
              auto through = reach_through_own(places, via, why, [round_over] { return round_over->load(); });
              std::lock_guard g(own->m);
              own->via = std::move(via);
              own->why = std::move(why);
              if (!through) return err(status::not_found);
              return through;
            },
            relay::kRelayDelayMs);
      }
      // The receiver's address on its network, after the router says this
      // network's public address is the code's (last in the race: the
      // indices before it are the places, the relay and the own computers).
      if (!lan_places.empty() && !code_public_v4.empty() && opts_.internet_mapping) {
        race->add(
            [lan_places, code_public_v4, mapping = *opts_.internet_mapping, connect]() -> result<std::unique_ptr<link>> {
              auto mo = mapping;
              mo.timeout_ms = std::min(mo.timeout_ms, 1500);
              mapping::mapper m(mo);
              const auto outside = m.outside_v4();
              if (!outside ||
                  std::find(code_public_v4.begin(), code_public_v4.end(), *outside) == code_public_v4.end()) {
                return err(status::not_found);
              }
              for (const auto& p : lan_places) {
                if (auto c = connect(p, kReachConnectMs)) return c;
              }
              return err(status::not_found);
            },
            kHappyEyeballsMs);
      }
      reached = race->wait(stopped);
      round_over->store(true);
      const int won = race->winner_index();
      const auto outcomes = race->outcomes();
      races.push_back(std::move(race));
      if (stopped()) {
        if (reached) reached->shutdown();
        return finish("Cancelled. Nothing was sent.");
      }
      if (reached) {
        if (won >= 0 && static_cast<std::size_t>(won) < code->places.size()) internet_place = code->places[won];
        if (won == own_at && own_tried) {
          std::lock_guard g(own->m);
          relayed_by = own->via;
        } else if (reached->through()) {
          relayed_by = "your relay server";
        }
        break;
      }
      if (own_tried) {
        std::lock_guard g(own->m);
        relay_why = own->why;
      }
      const auto mine = net::local_addresses();
      const bool here_v6 =
          std::any_of(mine.begin(), mine.end(), [](const net::address& a) { return a.v6 && net::is_public(a); });
      {
        std::lock_guard lock(m_);
        v_.out_note = why_unreachable(code->places, outcomes, here_v6);
        if (!relay_why.empty()) v_.out_note += " " + relay_why;
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
    auto opened = channel::open_internet(l, code->secret, id(), opts_.my_name, o);
    forget_opening(l);
    if (cancel_.load()) return finish("Cancelled. Nothing was sent.");
    if (!opened) {
      return finish("The code did not work: it was mistyped, has been used, or has expired. Ask for a new one. "
                    "Nothing was sent.");
    }
    std::shared_ptr<channel> ich(std::move(*opened));
    name = ich->peer_name();
    peer_id.clear();
    if (!relayed_by.empty()) {
      // A relayed session does not move: its rejoins would need the relay again.
      internet_place.reset();
      std::lock_guard lock(m_);
      v_.out_relay = relayed_by;
    }
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
  if (!l) return finish("Could not reach " + name + ". Check that its Driftwire window is still open.");
  auto opened = open_tracked(std::shared_ptr<link>(std::move(*l)), true, protocol);
  if (cancel_.load()) return finish("Cancelled. Nothing was sent.");
  if (!opened) {
    return finish(opened.error() == status::unsupported_format
                      ? name + " has a different version of Driftwire. Update MediaViewer on both computers."
                      : "Could not reach " + name + ".");
  }
  std::shared_ptr<channel> ch(std::move(*opened));
  if (ch->peer_id() != peer_id) {
    // Another client now answers at that address: not the one the person picked.
    ch->shutdown();
    return finish("Could not reach " + name + ".");
  }
  // #497: the name it gave inside the sealed channel, not the beacon's (which
  // anyone on the network can send in its place).
  if (!ch->peer_name().empty()) name = ch->peer_name();
  send_on(std::move(ch), std::move(files), name, total, peer_id, std::nullopt);
}

void engine::send_on(std::shared_ptr<channel> ch, std::vector<outgoing_file> files, std::string name,
                     std::uint64_t total, std::string peer_id, std::optional<net::endpoint> internet_place,
                     bool trusted) {
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
    v_.out_punched = ch->punched();
    v_.out_reconnecting = false;
    v_.out_protocol = ch->version();
    v_.out_peer = name;
    if (internet) {
      v_.out_check = ch->code();
      v_.out_note.clear();
    } else {
      out_expected_ = digits_of(ch->code());
    }
    // #472 PR 4: a remembered device proved the pair's secret and its key:
    // there is nothing to confirm.
    if (trusted) out_decision_ = 1;
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
      (void)ch->cancel(kCancelWaitMs);
      return finish("Cancelled. Nothing was sent.");
    }
    if (confirmed == 3) {
      (void)ch->cancel(kCancelWaitMs);
      return finish("The code was wrong " + std::to_string(kCodeTries) +
                    " times, so the link to " + name + " ended. Nothing was sent.");
    }
    if (confirmed == 1) break;
    if (std::chrono::steady_clock::now() - asked > std::chrono::milliseconds(opts_.confirm_ms)) {
      (void)ch->cancel(kCancelWaitMs);
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
  // #472 PR 5: over a local link, with the toggle on, the offer says the
  // receiver's internet places are welcome; they arrive while the files go.
  hooks hk = hooks_for(true);
  hk.identity = identity_to_present();  // #501: sealed, after the code (or the remembered key's check)
  {
    std::lock_guard lock(m_);
    handover_places_.clear();
    handover_dht_ = false;
    v_.out_handover = false;
    v_.out_handover_dht = false;
    hk.handover = ch->resumable() && !internet && v_.internet && v_.handover;
    hk.handover_dht = hk.handover && v_.dht;
  }
  // #472: with the DHT toggle on too, once the receiver says it meets on
  // the DHT (its places, right after Accept): the hub and the DHT up while
  // the files go, for a drop that takes the receiver's places with it. Both
  // sides then meet under the session's keys and the send rejoins there.
  std::mutex meet_m;
  std::unique_ptr<dht_handover> meet_dht;  // under meet_m
  auto start_meet = [&] {
    std::lock_guard lock(meet_m);
    if (meet_dht) return;
    dht_handover::options mo;
    mo.me = dht::role::sender;
    mo.client = dht_options();
    mo.loopback = opts_.internet_loopback;
    meet_dht = std::make_unique<dht_handover>(
        ch, std::move(mo), [] { return true; },
        [this, ch](std::unique_ptr<link> l) {
          // Back some other way since the meeting ended: the link that works stays.
          if (ch->link_up()) {
            l->shutdown();
            return;
          }
          if (opts_.wrap_handover_link) l = opts_.wrap_handover_link(std::move(l));
          if (auto r = ch->rejoin(std::shared_ptr<link>(std::move(l))); !r && r.error() != status::cancelled) {
            host_.log(1, "driftwire: a rejoin over the DHT did not complete");
          }
        },
        [this] {
          {
            std::lock_guard lock(m_);
            v_.out_handover_dht = true;
          }
          post_throttled();
        });
  };
  if (hk.handover) {
    // #499: the receiver's places are taken once (the first list that names
    // any), and only public addresses that are not this machine's own (the
    // relay's rule): never loopback or this network, which a receiver could
    // otherwise make this side connect to on every rescue.
    auto places_taken = std::make_shared<bool>(false);
    hk.on_places = [this, &start_meet, places_taken, ask_dht = hk.handover_dht](std::string_view text) {
      std::vector<net::endpoint> places;
      if (!*places_taken) {
        const auto own = net::local_addresses();
        for (const auto& p : parse_places(text)) {
          if (relay::may_reach(p, opts_.internet_loopback, own)) places.push_back(p);
        }
        *places_taken = !places.empty();
      }
      const bool dht = ask_dht && places_dht(text);  // only if this side asked
      {
        std::lock_guard lock(m_);
        if (!places.empty()) handover_places_ = std::move(places);
        handover_dht_ = handover_dht_ || dht;
        v_.out_handover = !handover_places_.empty() || handover_dht_;
      }
      if (dht) start_meet();
      post_throttled();
    };
  }
  struct places_guard {
    engine& e;
    ~places_guard() {
      std::lock_guard lock(e.m_);
      e.handover_places_.clear();
      e.handover_dht_ = false;
      e.v_.out_handover = false;
      e.v_.out_handover_dht = false;
    }
  } forget_places{*this};
  transfer_result r;
  {
    // #502: retired before end_transfer, as run_receive's is: retire takes
    // threads_m_, which a next transfer may take as soon as this one ends.
    struct meet_guard {
      engine& e;
      std::mutex& m;
      std::unique_ptr<dht_handover>& h;
      ~meet_guard() {
        std::unique_ptr<dht_handover> done;
        {
          std::lock_guard lock(m);
          done = std::move(h);
        }
        e.retire(std::move(done));
      }
    } retire_meet{*this, meet_m, meet_dht};
    r = send_files(*ch, files, hk, [this] {
      stay_awake(false);  // #530: neither machine sleeps from idleness while bytes move
      {
        std::lock_guard lock(m_);
        v_.out = view::out_phase::sending;
      }
      post();
    });
  }
  std::string note;
  const std::string sent = plural(r.done, "file", "files");
  switch (r.how) {
    case outcome::delivered:
      offer_remember(*ch, name, true, r.peer_key);  // #472 PR 4
      note = "Sent " + sent + " to " + name + ".";
      if (r.skipped) note += " " + plural(r.skipped, "file", "files") + " could not be read and stayed here.";
      break;
    case outcome::declined: note = name + " declined."; break;
    case outcome::busy: note = name + " is receiving from someone else. Try again in a moment."; break;
    case outcome::no_room: note = name + " does not have room for " + size_text(total) + "."; break;
    case outcome::cancelled: {
      // #511: who cancelled, and what of it arrived whole there.
      const std::string whole = std::to_string(r.done) + " of " + plural(static_cast<std::uint32_t>(files.size()), "file", "files");
      if (r.by_peer) {
        note = name + " cancelled. " +
               (r.done ? whole + " arrived whole there; nothing partial was kept." : std::string("Nothing was kept there."));
      } else {
        note = r.done ? "Cancelled. " + whole + " arrived whole on " + name + "."
                      : "Cancelled. Nothing was kept on " + name + ".";
      }
      break;
    }
    case outcome::failed:
      note = (r.by_peer ? "Something went wrong on " + name + ", so the transfer stopped. "
                        : "The connection to " + name + " was lost. ") +
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
    // #529: a rate from before the link changed says nothing about the next.
    (sending ? out_meter_ : in_meter_).restart();
    (sending ? v_.out_rate : v_.in_rate) = 0;
    (sending ? v_.out_left_s : v_.in_left_s) = -1;
    if (e == link_event::up) {
      (sending ? v_.out_via : v_.in_via) = transport_name(ch.via());
      (sending ? v_.out_punched : v_.in_punched) = ch.punched();
    }
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
// #502: the peer's answer to a rejoin, once connected. Two short messages:
// a bearer that connects and then says nothing (a machine beaconing the
// peer's id) loses within this, while the others race on.
constexpr int kRejoinAnswerMs = 3000;
using rejoin_race = basic_race<channel::pending_ptr>;

}  // namespace

void engine::run_mover(std::shared_ptr<channel> ch, std::string peer_id, const std::atomic<bool>& stop,
                       std::optional<net::endpoint> internet_place) {
  using clock = std::chrono::steady_clock;
  clock::time_point last_move{};  // none yet: the first move for speed may come at once
  std::vector<std::unique_ptr<rejoin_race>> races;
  auto stopped = [&] { return stop.load() || stopping_.load() || cancel_.load() || ch->closed(); };
  while (!stopped()) {
    std::optional<net::endpoint> wifi;
    std::string bt_handle;
    std::vector<net::endpoint> handover;  // #472 PR 5: the receiver's internet places
    {
      std::unique_lock lock(m_);
      mover_cv_.wait_for(lock, std::chrono::milliseconds(500), [&] { return mover_kick_ || stopped(); });
      mover_kick_ = false;
      // Ended (done, cancelled, or gone too long): nothing to rescue.
      if (stopped()) break;
      handover = handover_places_;
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
    std::erase_if(races, [](const std::unique_ptr<rejoin_race>& r) {
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
    const bool over_handover = rescue && !handover.empty();
    if (!wifi && !bt && !over_internet && !over_handover) continue;  // nowhere to try yet: the next beacon kicks this
    // #502: each bearer connects and begins the rejoin; the first whose
    // answer proves it holds the session's secret wins, and only it goes on
    // to resume_at. A bearer that only connects fast (a machine beaconing the
    // peer's id, a Wi-Fi place first heard mid-session) cannot win, so the
    // others' rescue is not held back, and nothing is adopted on a loser.
    const auto begin = [ch](result<std::unique_ptr<link>> c) -> result<channel::pending_ptr> {
      if (!c) return err(c.error());
      return ch->begin_rejoin(std::shared_ptr<link>(std::move(*c)), kRejoinAnswerMs);
    };
    auto race = std::make_unique<rejoin_race>();
    if (over_internet) {
      // Where the code's sender reached it: the receiver's listener stays up
      // while the transfer runs.
      race->add([this, begin, to = *internet_place] {
        auto c = opts_.connect_wifi ? opts_.connect_wifi(to, kRaceWifiMs) : net::connect(to, kRaceWifiMs);
        if (!c) return begin(err(c.error()));
        return begin(std::unique_ptr<link>(std::make_unique<bearer_as>(std::move(*c), transport::internet)));
      });
    }
    if (wifi) {
      race->add([this, begin, to = *wifi] {
        return begin(opts_.connect_wifi ? opts_.connect_wifi(to, kRaceWifiMs) : net::connect(to, kRaceWifiMs));
      });
    }
    if (bt) race->add([begin, bt, h = bt_handle] { return begin(bt->connect(h, kRaceBluetoothMs)); });
    if (over_handover) {
      // #472 PR 5: Wi-Fi went, the toggle is on: the receiver's internet
      // places (swapped over the local link, no lookup), alongside the rest.
      for (const auto& to : handover) {
        race->add([this, begin, to] {
          auto c = opts_.connect_internet ? opts_.connect_internet(to, kRaceWifiMs) : net::connect(to, kRaceWifiMs);
          if (!c) return begin(err(c.error()));
          return begin(std::unique_ptr<link>(std::make_unique<bearer_as>(std::move(*c), transport::internet)));
        });
      }
    }
    channel::pending_ptr p = race->wait(stopped);
    races.push_back(std::move(race));
    if (!p) continue;
    if (stopped()) break;  // p closes its link
    if (auto r = ch->complete_rejoin(std::move(p)); r) {
      last_move = clock::now();
    } else if (r.error() == status::cancelled || r.error() == status::busy) {
      break;  // the session ended (or flapped and ended)
    } else {
      host_.log(1, "driftwire: a rejoin did not complete");
    }
  }
  for (auto& r : races) r->join();
}

// ---- receiving --------------------------------------------------------------------------

void engine::run_receive(std::unique_ptr<link> incoming, bool internet, std::optional<known::device> from_known,
                         std::string source, std::int64_t known_hour, bool code) {
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
    // A rejoin with nothing running here, or not a code's sender: closed. A
    // code's hello counts only on the code's own ways in (#499: the
    // handover's way in must not burn the code shown).
    if (!h || !h->internet || (!from_known && (!in || !code))) {
      l->shutdown();
      return end_transfer();
    }
    if (from_known) {
      // #472 PR 4: met as a remembered device: the password of the hour it
      // met in (#499: not the hour now, which may have turned since), and the
      // key it had when it was remembered.
      const std::int64_t hour = known_hour >= 0 ? known_hour : known::hour_of(unix_now_ms());
      const auto k = known::presence_keys(*from_known, hour, public_key_);
      std::vector<std::uint8_t> pw(k.password.begin(), k.password.end());
      opened = open_tracked(l, false, kProtocolVersion, &*h, &pw);
      sodium_memzero(pw.data(), pw.size());
      if (opened && (*opened)->peer_key() != from_known->key) {
        (*opened)->shutdown();
        {
          std::lock_guard lock(m_);
          v_.in = view::in_phase::ended;
          v_.in_peer = from_known->name;
          v_.in_via = transport_name(transport::internet);
          v_.in_note = changed_key_text(from_known->name, false);
        }
        host_.log(1, "driftwire: a remembered device's key changed");
        post();
        return end_transfer();
      }
      if (opened) {
        // #502: forgotten while it was meeting: not received from as
        // remembered (or at all: the person said to forget it).
        bool still = false;
        {
          std::lock_guard lock(m_);
          still = std::any_of(devices_.begin(), devices_.end(),
                              [&](const known::device& d) { return d.key == from_known->key; });
        }
        if (!still) {
          (*opened)->shutdown();
          return end_transfer();
        }
      }
    } else {
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
  }
  if (!opened) {
    // Not a MediaViewer, or gone: nothing to show. A nearby handshake
    // dropped after this side answered counts toward the limit (#497).
    if (!internet) note_unconfirmed();
    return end_transfer();
  }
  std::shared_ptr<channel> ch(std::move(*opened));
  // #472 relays: a remembered device's first message may ask this computer
  // to relay (it sends its offer at once otherwise: nothing to confirm).
  std::optional<message> first;
  if (from_known) {
    // It sends its offer (or the request) as soon as the channel is open:
    // nothing waits on a person there. kSilenceMs is a dead link's limit.
    message m;
    if (auto r = ch->receive(m, kSilenceMs); !r) {
      ch->shutdown();
      return end_transfer();
    }
    if (maybe_relay(ch, *from_known, m)) return end_transfer();
    first = std::move(m);
  }
  // A remembered device goes by the name it was remembered as.
  const std::string name = from_known ? from_known->name : ch->peer_name();
  if (ch->resumable()) {
    channel* raw = ch.get();
    ch->set_on_link([this, raw](link_event e) { on_link_event(false, e, *raw); });
  }
  // The code shows now: the person there checks it against this screen
  // before anything about the files is sent.
  bool open_window = false;
  bool may_notify = false;
  {
    std::lock_guard lock(m_);
    // #502: a source past kNoticesPerMinute waits in the window, unannounced.
    may_notify = notice_limit_.allow(source, std::chrono::steady_clock::now());
    ++offer_seq_;  // its notices say which offer they are about
    if (cancel_.load() || stopping_.load()) {
      ch->shutdown();
    } else {
      live_ = ch;
      decision_ = 0;
      v_.in = view::in_phase::connecting;
      v_.in_peer = name;
      v_.in_code = ch->code();
      v_.in_via = transport_name(ch->via());
      v_.in_punched = ch->punched();
      v_.in_reconnecting = false;
      v_.in_protocol = ch->version();
      v_.in_internet = ch->internet();
      v_.in_known = from_known.has_value();
      v_.in_relay = l->through() ? "your relay server" : "";
      v_.in_note.clear();
      // Nothing of an earlier offer stays on screen while this one connects.
      v_.in_names.clear();
      v_.in_folder.clear();
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
  shown_notice.shown = may_notify && notice(name, 0, 0);
  if (open_window && may_notify && !shown_notice.shown) host_.show_screen(true);
  post();
  auto offer = first ? (first->type == msg::offer ? read_offer_after(*ch, first->text(), opts_.confirm_ms)
                                                 : result<incoming_offer>(err(status::corrupt)))
                     : read_offer(*ch, opts_.confirm_ms);
  if (!offer) {
    const bool declined = cancel_.load();
    if (!declined && !from_known && !ch->internet()) note_unconfirmed();  // #497
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
    v_.in_punched = ch->punched();
    v_.in_count = offer->files.size();
    v_.in_total = offer->bytes;
    v_.in_bytes = 0;
    v_.in_files = 0;
    v_.in_note.clear();
    v_.in_names.clear();
    // #498: a folder is shown as the folder (where its files go is the
    // folder's business), files one by one by name.
    v_.in_folder = offer->folder;
    for (const auto& f : offer->files) {
      if (!v_.in_folder.empty() || v_.in_names.size() >= kListedNames) break;
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
  // #530: what a receive from this sender that stopped left here, said in
  // the accept so it is not sent again. Only a sender that says who it is
  // (its identity, #501) and takes "have" and "at".
  const std::string sender = offer->key ? key_hex(*offer->key) : std::string();
  const bool may_resume = offer->resume && !sender.empty() && !data_file(kResumeFile).empty();
  resume_plan plan;
  if (may_resume) {
    std::optional<resume_record> record;
    {
      std::lock_guard lock(m_);
      for (const auto& rec : unfinished_)
        if (rec.sender == sender) record = rec;
    }
    if (record) plan = plan_resume(*record, *offer, dest);  // outside m_: it looks at the files
  }
  std::uint64_t here = plan.partial ? plan.partial->at : 0;
  for (const auto& f : plan.have) here += offer->files[f.index].size;
  if (const auto room = host_.free_bytes(dest); room && *room < offer->bytes - here + kRoomMargin) {
    (void)send_decline(*ch, outcome::no_room);
    return ended("Not enough space for " + size_text(offer->bytes - here) + " in that folder. Nothing was received.");
  }
  if (!send_accept(*ch, identity_to_present(), may_resume ? &plan : nullptr)) {
    return ended("The connection to " + name + " was lost. Nothing was received.");
  }
  stay_awake(true);  // #530: released by end_transfer
  {
    std::lock_guard lock(m_);
    v_.in = view::in_phase::receiving;
    v_.in_handover = false;
  }
  post();
  // #472 PR 5: over a local link, to a sender that takes them, with the
  // toggle on: a way in from the internet while the files arrive (on a
  // worker: the router may take seconds), and its places to the sender. It
  // closes when the files are in, before this transfer ends: its accept
  // thread may be waiting for threads_m_, which a next transfer holds while
  // it joins this thread.
  transfer_result r;
  {
    std::unique_ptr<way_in> way;
    std::thread way_opener;
    struct way_guard {
      engine& e;
      std::thread& t;
      std::unique_ptr<way_in>& w;
      ~way_guard() {
        if (t.joinable()) t.join();
        w.reset();  // the listener closes, the mappings go
        std::lock_guard lock(e.m_);
        e.v_.in_handover = false;
      }
    } close_way{*this, way_opener, way};
    // #472: and, with the DHT toggle on (here and in the offer), the hub
    // and the DHT up while the files arrive, for a drop that takes this
    // side's way in with it. Closed first (declared last).
    std::unique_ptr<dht_handover> meet_dht;
    struct meet_guard {
      engine& e;
      std::unique_ptr<dht_handover>& m;
      ~meet_guard() {
        e.retire(std::move(m));  // stopped, not waited for
        std::lock_guard lock(e.m_);
        e.v_.in_handover_dht = false;
      }
    } close_meet{*this, meet_dht};
    {
      bool want = false, want_dht = false;
      {
        std::lock_guard lock(m_);
        want = v_.internet && v_.handover;
        want_dht = want && v_.dht;
        v_.in_handover_dht = false;
      }
      if (want && offer->handover && ch->resumable() && !ch->internet()) {
        const bool dht = want_dht && offer->dht;
        if (dht) {
          dht_handover::options mo;
          mo.me = dht::role::receiver;
          mo.client = dht_options();
          mo.loopback = opts_.internet_loopback;
          meet_dht = std::make_unique<dht_handover>(
              ch, std::move(mo), [] { return true; },
              [this](std::unique_ptr<link> l) { on_internet_link(std::move(l)); },
              [this] {
                {
                  std::lock_guard lock(m_);
                  v_.in_handover_dht = true;
                }
                post_throttled();
              });
          // The sender learns at once that a drop of both is met on the DHT
          // (the way in may take seconds to open).
          if (send_places(*ch, {}, true)) {
            std::lock_guard lock(m_);
            v_.in_handover = true;
          }
        }
        way_opener = std::thread([this, ch, &way, dht] {
          try {
            auto w = way_in::open(opts_.internet_loopback, opts_.internet_mapping,
                                  [this](std::unique_ptr<link> l) { on_internet_link(std::move(l)); },
                                  data_file("driftwire_way_in.dat"));
            if (!w) return;
            if (send_places(*ch, w->places(), dht)) {
              std::lock_guard lock(m_);
              v_.in_handover = true;
            }
            way = std::move(w);
          } catch (...) {
          }
        });
      }
    }
    hooks h = hooks_for(false);
    if (may_resume) {
      // #530: what this receive holds, kept as it goes, so a power cut or a
      // lost link leaves a record the next send from this sender resumes from.
      h.keep = [&](const kept_state& k) {
        auto rec = record_of(k, *offer, plan, sender, name, dest, static_cast<std::int64_t>(std::time(nullptr)));
        if (rec.files.empty()) {
          drop_unfinished(sender);
        } else {
          keep_unfinished(std::move(rec));
        }
      };
    }
    r = receive_files(*ch, *offer, dest, h, may_resume ? &plan : nullptr);
  }
  // #530: delivered, cancelled or declined, nothing is waiting any more
  // (MediaViewer quitting is not a cancel: what arrived waits to resume).
  if (may_resume && r.how != outcome::failed && !stopping_.load()) drop_unfinished(sender);
  bool kept = false;
  if (may_resume && r.how == outcome::failed) {
    std::lock_guard lock(m_);
    kept = std::any_of(unfinished_.begin(), unfinished_.end(), [&](const resume_record& rec) { return rec.sender == sender; });
  }
  std::string note;
  const std::string got = plural(r.done, "file", "files");
  switch (r.how) {
    case outcome::delivered:
      offer_remember(*ch, name, false, offer->key);  // #472 PR 4
      note = "Received " + got + " from " + name + " into " + dest + ".";
      if (r.resumed_bytes) note += " It picked up where the last transfer stopped.";  // #530
      if (r.skipped) note += " " + plural(r.skipped, "file", "files") + " could not be read on " + name + ".";
      break;
    case outcome::cancelled:
      // #511: who cancelled; what arrived whole stays, nothing partial does.
      note = (r.by_peer ? name + " cancelled. " : std::string("You cancelled. ")) +
             (r.done ? got + " arrived whole; nothing partial was kept." : std::string("Nothing was received."));
      break;
    default:
      if (kept) {
        // #530: what arrived stays, the file that was arriving too (hidden).
        note = "The transfer from " + name + " stopped. " + (r.done ? got + " arrived whole in " + dest + ". " : "") +
               "What arrived is kept: when " + name + " sends these files again, it picks up where it stopped.";
      } else {
        note = "The transfer from " + name + " stopped. " +
               (r.done ? got + " arrived whole in " + dest + "; nothing partial was kept."
                       : std::string("Nothing partial was kept."));
      }
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
  std::uint64_t offer = 0;
  {
    std::lock_guard lock(m_);
    offer = offer_seq_;
  }
  return host_.post_offer_notice(kNoticeTitle, notice_body(peer, count, bytes), offer);
}

void engine::decline_notice(std::uint64_t offer) {
  {
    std::lock_guard lock(m_);
    if (offer != 0 && offer != offer_seq_) return;  // about an offer that has gone
  }
  on_event("decline", 1, "");  // a click
}

std::string notice_body(const std::string& peer, std::size_t count, std::uint64_t bytes) {
  const std::string who = peer.empty() ? std::string("A computer nearby") : peer;
  if (count == 0) return who + " wants to send you files. Open Driftwire to see the code.";
  return who + " wants to send you " + std::to_string(count) + (count == 1 ? " file, " : " files, ") +
         size_text(bytes) + ". Open Driftwire to accept.";
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

}  // namespace mv::driftwire
