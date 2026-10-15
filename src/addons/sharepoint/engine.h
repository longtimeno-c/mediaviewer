// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Sharepoint's engine (docs/design/28): the state behind the window, the
// presence (listen, announce, hear), and one transfer at a time.
//
// Threads. The host calls in on its worker for this add-on (screen open and
// close, the person's clicks, the files a command hands over). The engine
// keeps its own threads for what can wait on the network: the listener's
// accept loop, the discovery bearer, Bluetooth's callbacks, and the one
// transfer. None of them is a UI or render thread, and none of them holds the
// engine's lock across I/O. Every change posts the whole view tree through
// the host (a tree is small; progress posts are held to ~10 a second).
//
// When it is on the network. Presence runs while the window is open or
// receiving is on, and while a transfer runs; otherwise there is no socket
// and no thread (docs/design/28 "What stays put"). Nothing here runs before
// the host loads the add-on, after first pixel.
//
// Who can take what. Bytes move only from a sender to the peer its person
// picked, after the person at that peer accepted (msg::accept); the sender
// never answers a request for files. A machine that only hears the beacon
// learns a name and a port.
//
// Portable (D9): the bearers are injected.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "addons/sharepoint/channel.h"
#include "addons/sharepoint/presence.h"
#include "addons/sharepoint/transfer.h"

namespace mv::sharepoint {

// A set over this is refused on Bluetooth (a budget, docs/design/12): it
// would take many minutes. Wi-Fi carries anything.
inline constexpr std::uint64_t kBluetoothMaxBytes = 32ull * 1024 * 1024;
// A peer not heard for this long leaves the list.
inline constexpr int kPeerForgetMs = 4500;
// Peers listed at most (a flood of made-up beacons costs nothing past it).
inline constexpr std::size_t kMaxPeers = 64;
// Names listed under "Files to send" / in an offer, at most.
inline constexpr std::size_t kListedNames = 200;

// What the window shows, as data (build_tree turns it into the view tree).
struct view {
  std::string my_name;
  bool receiving = false;       // the toggle: stay visible after the window closes
  bool wifi_up = false;         // the listener and the beacon are running
  std::string bluetooth_note;   // "" when Bluetooth runs, else why not

  // ---- send
  std::vector<std::string> file_names;  // the first kListedNames
  std::size_t file_count = 0;
  std::uint64_t file_bytes = 0;
  bool reading_folder = false;
  struct peer_row {
    std::string id, name, via;  // via: "Wi-Fi", "Bluetooth", "Wi-Fi and Bluetooth"
    bool wifi = false, bt = false;  // heard on each in the last kPeerForgetMs
  };
  std::vector<peer_row> peers;
  std::string selected_peer;
  // waiting_wifi: the set is too big for Bluetooth and the peer is only in
  // Bluetooth range; the send starts by itself once the peer is heard on Wi-Fi.
  // confirming: the link is up and both screens show the code; nothing about
  // the files has left until this person says the codes match.
  enum class out_phase : std::uint8_t {
    idle, waiting_wifi, connecting, confirming, waiting, sending, done, ended
  } out = out_phase::idle;
  std::string out_peer, out_code, out_note;
  std::uint64_t out_bytes = 0, out_total = 0;
  std::uint32_t out_files = 0, out_count = 0;

  // ---- receive
  // connecting: a sender's link is up and the code shows; its offer comes
  // once the person there has checked the code.
  enum class in_phase : std::uint8_t { none, connecting, asking, receiving, done, ended } in = in_phase::none;
  std::string in_peer, in_code, in_note, in_via;
  std::vector<std::string> in_names;
  std::size_t in_count = 0;
  std::uint64_t in_total = 0, in_bytes = 0;
  std::uint32_t in_files = 0;
  std::string destination;
};

// The selected peer is heard only on Bluetooth and the set is over
// kBluetoothMaxBytes: Send waits for it on Wi-Fi instead (the window says so).
[[nodiscard]] bool needs_wifi(const view& v) noexcept;

// The view tree (docs/design/25 §10 vocabulary) for `v`. Pure; tested.
[[nodiscard]] std::string build_tree(const view& v);

// What the engine needs from the host.
class engine_host {
 public:
  virtual ~engine_host() = default;
  // The whole tree for the open window; a no-op when it is closed.
  virtual void post_tree(const std::string& tree_json) = 0;
  // Opens (an offer arrived while receiving with the window closed) or
  // closes the window.
  virtual void show_screen(bool show) = 0;
  // Where received files go unless the person picks another folder:
  // <the default photo folder>/Received. "" when there is none.
  [[nodiscard]] virtual std::string default_destination() = 0;
  // Free bytes on the volume of `dir`, if known.
  [[nodiscard]] virtual std::optional<std::uint64_t> free_bytes(const std::string& dir) = 0;
  // Non-zero while the viewer presents (a transfer waits between chunks).
  [[nodiscard]] virtual bool should_yield() = 0;
  // For each name of one folder, the index of its pair partner (RAW + JPEG,
  // Live Photo) or UINT32_MAX. Empty: no pairing.
  [[nodiscard]] virtual std::vector<std::uint32_t> pair_partners(const std::vector<std::string>& names) = 0;
  // Never a path, a name or an address (rule 6).
  virtual void log(int level, const char* ascii) = 0;
};

class engine {
 public:
  struct options {
    std::string my_name;  // net::machine_name() when empty
    std::function<std::unique_ptr<discovery>()> make_discovery;  // wifi_discovery() when empty
    std::function<std::unique_ptr<bluetooth>()> make_bluetooth;  // make_bluetooth() when empty
    bool use_bluetooth = true;
    // Tests: listen on loopback only.
    bool loopback_only = false;
    // No presence at all: no socket, no beacon, no Bluetooth (the loader
    // test, which opens the window; MV_SHAREPOINT_OFFLINE=1).
    bool offline = false;
  };

  engine(engine_host& host, options opts);
  ~engine();  // stops everything; on return no thread of the engine runs
  engine(const engine&) = delete;
  engine& operator=(const engine&) = delete;

  // ---- from the host's worker ----------------------------------------------------
  // The window opened (`path` is the file on screen, offered when nothing
  // else is) or closed.
  void screen_opened(const std::string& path);
  void screen_closed();
  // Files a command, a context item or the gallery's share icon handed over:
  // these and their pair halves and sidecars become the set to send.
  void set_files(const std::vector<std::string>& paths);
  // A control in the window (screen::event_kind, the value as JSON).
  void on_event(const std::string& node, std::uint32_t kind, const std::string& value_json);

  // ---- for tests ------------------------------------------------------------------
  [[nodiscard]] view snapshot() const;
  [[nodiscard]] const std::string& id() const noexcept { return id_; }
  // Blocks until the current transfer (if any) has ended.
  void wait_idle();

 private:
  struct peer {
    std::string id, name;
    std::optional<net::endpoint> wifi;
    std::string bt_handle;
    std::chrono::steady_clock::time_point wifi_seen{}, bt_seen{};
  };
  struct helper {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };

  // The control thread: starts and stops presence (the only thread that
  // does, so no thread ever joins one that may be joining it) and sends the
  // throttled posts.
  void control_loop();
  void request_reconcile();
  void reconcile();
  void post();            // the tree now (any thread)
  void post_throttled();  // within ~100 ms, coalesced
  void start_presence();
  void stop_presence();
  void refresh_peers_locked();
  void on_beacon(const beacon& b, const std::string& address);
  void on_tick();
  void on_bt_peer(const bt_peer& p);
  void on_incoming(std::unique_ptr<link> l);
  void accept_loop();
  void start_send();
  // The peer the send waited for is on Wi-Fi now: go.
  void start_waiting_send();
  void run_send(std::string peer_id, std::vector<outgoing_file> files);
  void run_receive(std::unique_ptr<link> l);
  void end_transfer();
  void refuse_busy(std::shared_ptr<link> l);
  // channel::open with `l` listed in opening_ meanwhile.
  [[nodiscard]] result<std::unique_ptr<channel>> open_tracked(std::shared_ptr<link> l, bool client);
  void forget_opening(const std::shared_ptr<link>& l);
  void fail_transfer(bool sending) noexcept;  // after an exception on a transfer thread
  // The files to send are read on one thread of the engine's (the folder
  // reader), never on the caller's: a set asked for replaces one waiting, and
  // the one being read stops at its next file (its generation is stale).
  // Nobody waits for it (issue #442: the share icon stalled the UI on Windows).
  struct files_job {
    std::vector<std::string> paths;  // a set handed over...
    std::string folder;              // ...or a folder, recursively
    std::uint64_t gen = 0;
  };
  void queue_files(files_job job);
  void files_loop();
  [[nodiscard]] bool files_stale(std::uint64_t gen) const noexcept;
  void build_files(std::vector<std::string> paths, std::uint64_t gen);
  void walk_folder(const std::string& folder, std::uint64_t gen);
  void publish_files(std::vector<outgoing_file> files, std::uint64_t gen);
  void read_folder(std::string folder);
  void cancel_transfer();
  void reap_helpers();  // under threads_m_
  [[nodiscard]] bool visible_locked() const noexcept;
  [[nodiscard]] bool out_busy_locked() const noexcept;
  [[nodiscard]] hooks hooks_for(bool sending);

  engine_host& host_;
  options opts_;
  std::string id_;

  mutable std::mutex m_;
  view v_;                                // under m_
  std::map<std::string, peer> peers_;     // under m_
  bool peers_changed_ = false;            // under m_
  std::vector<outgoing_file> files_;      // under m_
  bool screen_open_ = false;              // under m_
  bool transfer_running_ = false;         // under m_
  std::atomic<std::uint64_t> files_generation_{0};  // changed under m_; read anywhere
  int decision_ = 0;                      // under m_: 0 none, 1 accept, 2 decline
  int out_decision_ = 0;                  // under m_: 0 none, 1 codes match, 2 cancel
  // The peer a too-big set waits to see on Wi-Fi (out_phase::waiting_wifi).
  std::string waiting_peer_;              // under m_
  // Links in their handshake (no channel yet), so a cancel or the destructor
  // can end one a peer stalls.
  std::vector<std::shared_ptr<link>> opening_;  // under m_
  std::condition_variable decided_;
  std::condition_variable idle_;
  std::shared_ptr<channel> live_;         // under m_: the transfer's channel, to cancel it
  std::atomic<bool> cancel_{false};
  std::atomic<bool> stopping_{false};

  std::mutex post_m_;  // one post at a time, so trees arrive in order
  std::atomic<std::chrono::steady_clock::time_point> last_post_{};
  std::mutex control_m_;
  std::condition_variable control_cv_;
  bool reconcile_wanted_ = false;                     // under control_m_
  bool post_pending_ = false;                         // under control_m_
  std::chrono::steady_clock::time_point post_due_{};  // under control_m_
  std::thread control_thread_;

  // Presence: changed only by the control thread (and the destructor after it).
  std::mutex presence_m_;
  bool presence_on_ = false;
  std::unique_ptr<net::listener> listener_;
  std::unique_ptr<discovery> discovery_;
  std::shared_ptr<bluetooth> bluetooth_;  // a sender holds a reference while it connects
  std::thread accept_thread_;

  std::mutex threads_m_;
  std::thread transfer_thread_;
  std::vector<helper> helpers_;  // busy refusals

  // The folder reader (queue_files).
  std::mutex files_m_;
  std::condition_variable files_cv_;
  std::optional<files_job> files_next_;  // under files_m_
  std::thread files_thread_;             // under files_m_; made at the first set
};

// "Received" under the default photo folder.
inline constexpr const char* kReceivedFolder = "Received";

}  // namespace mv::sharepoint
