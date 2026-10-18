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

#include <array>
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
#include "addons/sharepoint/internet.h"
#include "addons/sharepoint/known.h"
#include "addons/sharepoint/prefs.h"
#include "addons/sharepoint/presence.h"
#include "addons/sharepoint/transfer.h"

namespace mv::sharepoint {

// A set over this is refused on Bluetooth to a protocol 1 peer (a budget,
// docs/design/12): it would take many minutes. Wi-Fi carries anything. With
// protocol 2 (#472) a send starts on Bluetooth and moves to Wi-Fi by itself.
inline constexpr std::uint64_t kBluetoothMaxBytes = 32ull * 1024 * 1024;
// A peer not heard for this long leaves the list.
inline constexpr int kPeerForgetMs = 4500;
// Peers listed at most (a flood of made-up beacons costs nothing past it).
inline constexpr std::size_t kMaxPeers = 64;
// Names listed under "Files to send" / in an offer, at most.
inline constexpr std::size_t kListedNames = 200;
// Thumbnails shown of the files to send (the window's strip).
inline constexpr std::size_t kShownThumbs = 6;
// Wrong codes typed on the sender before the link ends (#442).
inline constexpr int kCodeTries = 3;

// What the window shows, as data (build_tree turns it into the view tree).
struct view {
  std::string my_name;
  bool receiving = false;       // the toggle: stay visible after the window closes
  bool background = false;      // #452: and after MediaViewer closes (implies receiving)
  bool wifi_up = false;         // the listener and the beacon are running
  std::string bluetooth_note;   // "" when Bluetooth runs, else why not

  // ---- send
  std::vector<std::string> file_names;  // the first kListedNames
  std::vector<std::string> file_paths;  // the first kShownThumbs, on this machine (thumbnails; never sent)
  std::size_t file_count = 0;
  std::uint64_t file_bytes = 0;
  bool reading_folder = false;
  struct peer_row {
    std::string id, name, via;  // via: "Wi-Fi", "Bluetooth", "Wi-Fi and Bluetooth"
    bool wifi = false, bt = false;  // heard on each in the last kPeerForgetMs
    bool resumable = false;         // speaks protocol 2: a send moves between bearers (#472)
  };
  std::vector<peer_row> peers;
  std::string selected_peer;
  // waiting_wifi: the set is too big for Bluetooth and the peer is only in
  // Bluetooth range; the send starts by itself once the peer is heard on Wi-Fi.
  // confirming: the link is up and the receiver's screen shows the code; this
  // person types it here (#442). Nothing about the files has left until the
  // code typed is the one this link derived; kCodeTries wrong ones end it.
  // This screen never shows the code.
  // reaching (#472 PR 2): trying the places in an internet code, round after
  // round. naming: connected over the internet; this person confirms the
  // other's name (and may compare the check) before the offer leaves.
  enum class out_phase : std::uint8_t {
    idle, waiting_wifi, connecting, confirming, waiting, sending, done, ended, reaching, naming
  } out = out_phase::idle;
  std::string out_peer, out_peer_id, out_note;
  std::string out_entry_note;  // confirming: "That is not the code ..." after a wrong one
  int out_tries_left = kCodeTries;
  std::uint64_t out_bytes = 0, out_total = 0;
  std::uint32_t out_files = 0, out_count = 0;
  // #472: the bearer the send is on, and whether it is between links
  // (suspect or dropped, a rejoin under way).
  std::string out_via;
  bool out_reconnecting = false;
  std::uint32_t out_protocol = 0;  // the send's protocol, once its link is open (tests, the log)
  bool out_internet = false;       // #472 PR 2: to a code, over the internet
  int out_attempt = 0;             // reaching: the round
  std::string out_check;           // naming: the six digits both screens show

  // ---- receive
  // connecting: a sender's link is up and the code shows; its offer comes
  // once the person there has checked the code.
  enum class in_phase : std::uint8_t { none, connecting, asking, receiving, done, ended } in = in_phase::none;
  std::string in_peer, in_code, in_note, in_via;
  std::vector<std::string> in_names;
  std::size_t in_count = 0;
  std::uint64_t in_total = 0, in_bytes = 0;
  std::uint32_t in_files = 0;
  bool in_reconnecting = false;  // #472, as out_reconnecting
  std::uint32_t in_protocol = 0;  // as out_protocol
  bool in_internet = false;       // #472 PR 2: from a code; in_code is the check, not a code to type
  bool in_known = false;          // #472 PR 4: from a remembered device (its key checked)
  std::string destination;

  // ---- over the internet (#472 PR 2; the toggle off: none of it shows or runs)
  bool internet = false;
  bool dht = false;          // #472 PR 3: word codes on the public DHT (opt-in)
  std::string forwarded;     // the advanced "Forwarded port"
  std::uint8_t code_phase = 0;  // internet_receiver::phase
  std::string code_text, code_link, code_note, code_how;
  std::string send_code_note;   // a pasted code that is not one

  // ---- known devices (#472 PR 4)
  bool can_remember = false;      // this machine keeps its identity (a secret store)
  std::vector<std::string> known;  // the remembered devices' names, in order
  // A protocol 2 transfer just went through: "Remember <peer>" shows under
  // its result, with whether it is remembered.
  bool remember_offer = false;
  bool remembered = false;
  std::string remember_peer;
  bool out_known = false;  // the send is to a remembered device (reaching: waiting for it)

  // ---- Wi-Fi -> internet (#472 PR 5; needs the internet toggle)
  bool handover = false;      // "Continue over the internet if Wi-Fi drops", off by default
  bool out_handover = false;  // the receiver sent where it can be reached: a drop continues there
  bool in_handover = false;   // this side sent its places to the sender
  // #472: both sides also meet on the DHT if both move (needs the DHT
  // toggle on both): this side's hub is open and the DHT answered.
  bool out_handover_dht = false;
  bool in_handover_dht = false;
};

// The selected peer is heard only on Bluetooth, speaks only protocol 1, and
// the set is over kBluetoothMaxBytes: Send waits for it on Wi-Fi instead (the
// window says so).
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
  // Where received files go unless the person picks another folder: the
  // Downloads folder (#442; net::downloads_folder). "" when there is none.
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
  // The command bar's status text ("" hides it): shows that this machine is
  // discoverable while Keep receiving is on (#442). False when the host did
  // not take it (the add-on not attached yet): the engine tries again.
  virtual bool post_status(const std::string& text) {
    (void)text;
    return true;
  }
  // #452: a system notification for the offer waiting here. `body` names the
  // sender and, once known, the count and size: never a file name or the code
  // (rule 6; a lock screen shows it). Clicking it opens the window; its one
  // button is Decline (an on_event "decline" click). An empty title takes it
  // back. False when the host shows no notifications: the engine then opens
  // the window itself, as before #452.
  virtual bool post_notice(const std::string& title, const std::string& body) {
    (void)title;
    (void)body;
    return false;
  }
  // #452: ask the person to allow notifications (a toggle turned on; the OS
  // asks once). Never at launch.
  virtual void ask_to_notify() {}
  // #452: keep this process, with a menu-bar item / tray icon, after every
  // window is closed (Receive when MediaViewer is closed). False when the host
  // did not take it (the add-on not attached yet): the engine tries again.
  virtual bool keep_alive(bool on) {
    (void)on;
    return true;
  }
};

// The status text while Keep receiving is on.
inline constexpr const char* kReceivingStatus = "Sharepoint: receiving";
// The toggle that keeps MediaViewer running with every window closed (#452),
// and the menu-bar / tray item's first line while it is on.
inline constexpr const char* kBackgroundLabel = "Receive when MediaViewer is closed";
inline constexpr const char* kNoticeTitle = "Sharepoint";
// The notification's text for an offer (#452): the sender's name, and the
// count and size once the offer has arrived (count 0: not yet). Never a file
// name or the code. Pure; tested.
[[nodiscard]] std::string notice_body(const std::string& peer, std::size_t count, std::uint64_t bytes);

class engine {
 public:
  struct options {
    std::string my_name;  // net::machine_name() when empty
    std::function<std::unique_ptr<discovery>()> make_discovery;  // wifi_discovery() when empty
    std::function<std::unique_ptr<bluetooth>()> make_bluetooth;  // make_bluetooth() when empty
    // A TCP connection to a peer's beacon address (net::connect when empty;
    // the tests wrap it to cut a link mid-transfer).
    std::function<result<std::unique_ptr<link>>(const net::endpoint&, int timeout_ms)> connect_wifi;
    bool use_bluetooth = true;
    // Tests: listen on loopback only.
    bool loopback_only = false;
    // No presence at all: no socket, no beacon, no Bluetooth (the loader
    // test, which opens the window; MV_SHAREPOINT_OFFLINE=1).
    bool offline = false;
    // Where the prefs live (prefs.h; the add-on's data folder). "" keeps
    // nothing across launches. `initial`: already read by the caller; when
    // empty the engine reads `prefs_path` itself.
    std::string prefs_path;
    std::optional<prefs> initial;
    // #472 PR 2, the tests: a code's places are this machine's loopback
    // addresses, the router is asked nothing (or a stand-in), and codes live
    // this long.
    bool internet_loopback = false;
    std::optional<mapping::options> internet_mapping = mapping::options{};
    int code_lifetime_ms = kCodeLifetimeMs;
    // #472 PR 3, the tests: the DHT starts from these nodes (no routers), the
    // code's key derivation is the cheapest, and every code is a word code.
    std::optional<std::vector<net::endpoint>> dht_nodes;
    bool dht_fast_kdf = false;
    bool internet_force_words = false;
    // #472 PR 4: where this machine's identity and the devices it remembers
    // are kept (secrets::os_store() in the app; a folder in the tests).
    // Empty, or nullptr from it: a key per run, and nothing is remembered.
    std::function<std::unique_ptr<secrets::store>()> make_secrets;
    // How often a remembered device's wait looks for a sender on the DHT.
    int known_poll_ms = 15000;
    // #472 PR 5: a TCP connection to a receiver's internet place, for the
    // Wi-Fi -> internet handover (net::connect when empty; the tests').
    std::function<result<std::unique_ptr<link>>(const net::endpoint&, int timeout_ms)> connect_internet;
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
    std::uint32_t wifi_protocol = kProtocol1, bt_protocol = kProtocol1;  // what each announced
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
  // The prefs change: written on the control thread, never the caller's.
  void request_save();
  void save_now();
  // The control thread: the status text for the toggle, posted (or retried).
  void sync_status();
  void post();            // the tree now (any thread)
  void post_throttled();  // within ~100 ms, coalesced
  void start_presence();
  void stop_presence();
  void refresh_peers_locked();
  void on_beacon(const beacon& b, const std::string& address);
  void on_tick();
  void on_bt_peer(const bt_peer& p);
  // `internet`: from the internet listener (#472 PR 2).
  // `from_known` (#472 PR 4): met on the DHT as a remembered device.
  void on_incoming(std::unique_ptr<link> l, bool internet = false,
                   std::optional<known::device> from_known = std::nullopt);
  void accept_loop();
  void start_send();
  // The peer the send waited for is on Wi-Fi now: go.
  void start_waiting_send();
  // `code` (#472 PR 2): to the receiver of an internet code instead of a
  // peer nearby; peer_id is then empty.
  // `to_known` (#472 PR 4): to a remembered device, met on the DHT.
  void run_send(std::string peer_id, std::vector<outgoing_file> files, std::optional<invite::code> code = std::nullopt,
                std::optional<dht::word_code> words = std::nullopt,
                std::optional<known::device> to_known = std::nullopt);
  // #472 PR 4: known devices. Sending to one; the toggle under a transfer's
  // result; Forget.
  void start_send_known(std::size_t index);
  void set_remembered(bool on);
  void forget_known(std::size_t index);
  void publish_known_locked();
  // The control thread: the wait for remembered devices, started and
  // stopped with the window or Keep receiving, the internet and DHT
  // toggles, and whether any device is remembered.
  void sync_known();
  void run_known_wait();
  // A transfer ended well over protocol 2: what Remember would keep.
  void offer_remember(const channel& ch, const std::string& name);
  // #472 PR 3: the DHT client's options (the routers, or the tests' nodes;
  // the table kept beside the prefs).
  [[nodiscard]] dht::client::options dht_options() const;
  void start_send_code();
  // After the link is open and the peer is known: the code typed (nearby) or
  // the name confirmed (internet), then the offer, Accept and the files.
  // `trusted` (#472 PR 4): a remembered device, its key checked; nothing to confirm.
  void send_on(std::shared_ptr<channel> ch, std::vector<outgoing_file> files, std::string name, std::uint64_t total,
               std::string peer_id, std::optional<net::endpoint> internet_place, bool trusted = false);
  // The control thread: the internet receiver made or dropped with the toggle.
  void sync_internet();
  void on_internet_link(std::unique_ptr<link> l);
  // #472, the sender's side of a protocol 2 send, on its own thread while
  // the send runs: when the link is suspect or drops, races every bearer the
  // peer is heard on and rejoins over the first that connects; when the send
  // is on Bluetooth and the peer is heard on Wi-Fi, moves there (make before
  // break), at most once per kSwitchHoldMs.
  void run_mover(std::shared_ptr<channel> ch, std::string peer_id, const std::atomic<bool>& stop,
                 std::optional<net::endpoint> internet_place = std::nullopt);
  // A channel's link event, for the window (and the mover, on the sender).
  void on_link_event(bool sending, link_event e, const channel& ch);
  // `internet`: the link came to the internet listener (a code's sender, or
  // a rejoin of a transfer over one).
  void run_receive(std::unique_ptr<link> l, bool internet = false,
                   std::optional<known::device> from_known = std::nullopt);
  // The offer's notification (#452), from the receive thread only. False:
  // the host has none (open the window instead).
  bool notice(const std::string& peer, std::size_t count, std::uint64_t bytes);
  void end_transfer();
  // A link while a transfer runs: a rejoin of it goes to it (#472); any
  // other is answered busy.
  void route_busy(std::shared_ptr<link> l, bool internet = false);
  // channel::open with `l` listed in opening_ meanwhile.
  // A handshake the engine can end (cancel, the destructor). The client
  // speaks `version`; the server answers the hello it read.
  [[nodiscard]] result<std::unique_ptr<channel>> open_tracked(
      std::shared_ptr<link> l, bool client, std::uint32_t version = kProtocolVersion, const hello* read = nullptr,
      const std::vector<std::uint8_t>* internet_secret = nullptr);
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
  int out_decision_ = 0;                  // under m_: 0 none, 1 code right, 2 cancel, 3 wrong too often
  std::string out_expected_;              // under m_: the link's code, digits only; never in the view
  // The peer a too-big set waits to see on Wi-Fi (out_phase::waiting_wifi).
  std::string waiting_peer_;              // under m_
  // Links in their handshake (no channel yet), so a cancel or the destructor
  // can end one a peer stalls.
  std::vector<std::shared_ptr<link>> opening_;  // under m_
  std::condition_variable decided_;
  std::condition_variable idle_;
  std::shared_ptr<channel> live_;         // under m_: the transfer's channel, to cancel it (and rejoin it)
  bool mover_kick_ = false;               // under m_: the mover looks again now
  std::condition_variable mover_cv_;
  // This machine's X25519 key in protocol 2's XX handshake: kept in the
  // secret store (#472 PR 4), else this run's. Set in the constructor, read
  // anywhere after. Wiped at the end.
  std::array<std::uint8_t, 32> static_key_{};
  std::array<std::uint8_t, 32> public_key_{};  // static_key_'s public half
  // #472 PR 4: the secret store (calls under secrets_m_, never on the UI
  // thread), the remembered devices, and what Remember would keep.
  std::mutex secrets_m_;
  std::unique_ptr<secrets::store> secrets_;  // under secrets_m_
  std::vector<known::device> devices_;       // under m_
  std::optional<known::device> last_pair_;   // under m_
  // The wait for remembered devices (the control thread starts and stops it).
  std::thread known_thread_;
  std::atomic<bool> known_stop_{false};
  // #472 PR 5: the receiver's internet places, as it sent them, for the
  // send running now (the mover races them when the local link goes).
  std::vector<net::endpoint> handover_places_;  // under m_
  // #472: the receiver said it meets on the DHT too, if both sides move.
  bool handover_dht_ = false;  // under m_
  // #472 PR 2: the receiver of codes while the toggle is on (made and dropped
  // on the control thread; a receive in flight keeps its own reference).
  std::shared_ptr<internet_receiver> internet_;  // under m_
  std::string code_entry_;                       // under m_: what the "send to a code" field holds
  std::atomic<bool> cancel_{false};
  std::atomic<bool> stopping_{false};

  std::mutex post_m_;  // one post at a time, so trees arrive in order
  std::atomic<std::chrono::steady_clock::time_point> last_post_{};
  std::mutex control_m_;
  std::condition_variable control_cv_;
  bool reconcile_wanted_ = false;                     // under control_m_
  bool post_pending_ = false;                         // under control_m_
  bool save_wanted_ = false;                          // under control_m_
  // The status text the host shows (post_status), and a retry while the
  // host is not ready for it.
  std::string status_shown_;                          // control thread
  bool keep_shown_ = false;                           // control thread: keep_alive taken
  int status_tries_ = 0;                              // control thread
  std::chrono::steady_clock::time_point status_due_{};  // under control_m_; {} = none
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

}  // namespace mv::sharepoint
