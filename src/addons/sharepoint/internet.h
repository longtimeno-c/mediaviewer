// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Sharepoint over the internet with a direct code (issue #472 PR 2;
// docs/design/28 "Over the internet"). No server of anyone's: the receiver's
// window shows a code that carries its own public addresses; a sender
// anywhere connects to one of them over TCP and proves the code with CPace.
//
// The receiver (internet_receiver): one code at a time. Making one opens a
// TCP listener (IPv6 and IPv4) and asks the router for a port (mapping.h),
// then gathers the places a sender can try: the
// mapped port (and external 443 when the router allows, for a sender whose
// network lets only 443 out), this machine's public IPv6 address (with a
// router pinhole when IGDv2 offers one), and its address on this network (a
// sender on the same network or behind the same router). A code dies at its
// first attempt, right or wrong, and after kCodeLifetimeMs; then the
// listener closes and every mapping is removed, unless a transfer over the
// internet still runs on it (its rejoins come back the same way).
//
// The sender (reach): every place at once, IPv6 first and the rest 250 ms
// later ("happy eyeballs"); the first TCP connection wins and the others are
// closed before a byte is sent, so only one attempt reaches the receiver
// and spends the code. A round with no connection is retried after 1, 2, 5,
// 10 and then every 30 seconds until Cancel; why_unreachable() says what the
// round's answers point to.
//
// Opt-in ("Send and receive over the internet", off by default): with it off
// nothing here runs. Nothing is logged with an address or a code (rule 6).
// Portable (D9).
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "addons/sharepoint/dht.h"
#include "addons/sharepoint/invite.h"
#include "addons/sharepoint/mapping.h"
#include "addons/sharepoint/net.h"
#include "addons/sharepoint/race.h"

namespace mv::sharepoint {

inline constexpr int kCodeLifetimeMs = 10 * 60 * 1000;
inline constexpr int kMapLifetimeS = 3600;
inline constexpr int kReachConnectMs = 5000;  // each TCP connect of a round
inline constexpr int kHappyEyeballsMs = 250;

class internet_receiver {
 public:
  enum class phase : std::uint8_t {
    none,       // no code
    preparing,  // listening, asking the router, gathering places
    ready,      // a code waits
    used,       // someone tried it (right: a transfer; wrong: it died)
    expired,
    failed,     // no place a sender anywhere could reach
  };
  struct snapshot {
    phase p = phase::none;
    std::string text, link;  // ready: the code
    std::string note;        // what the window says under it
    std::vector<std::string> how;  // "IPv6", "UPnP", ... (the window's line)
  };
  struct options {
    bool loopback_only = false;  // the tests: places are 127.0.0.1 and ::1
    // The router's port mapping; nullopt: none asked (the tests).
    std::optional<mapping::options> mapping = mapping::options{};
    int lifetime_ms = kCodeLifetimeMs;
    // #472 PR 3: with no public place, a four-word code on the public DHT
    // (opt-in, "Find each other over the internet"). `dht_fast_kdf` and
    // `force_words` are the tests'.
    bool dht = false;
    dht::client::options dht_client;
    bool dht_fast_kdf = false;
    bool force_words = false;
    // #472 relays: the advanced "Relay (your own server)", "<address or
    // name>:<port>#<key>" (tools/sharepoint-relay prints it). Set: each code also waits
    // there under a fresh token the code carries, so a sender whose direct
    // tries fail meets this computer through it.
    std::string relay;
  };
  using link_fn = std::function<void(std::unique_ptr<link>)>;

  internet_receiver(options o, link_fn on_link, std::function<void()> changed);
  ~internet_receiver();  // the code, the listener, every mapping: gone
  internet_receiver(const internet_receiver&) = delete;
  internet_receiver& operator=(const internet_receiver&) = delete;

  // Any thread: a new code (the old one, if any, dies); the work is on this
  // object's own thread.
  void make_code();
  void drop_code();
  // The DHT toggle, for the next code.
  void set_dht(bool on);
  // The relay server, for the next code.
  void set_relay(std::string relay);
  [[nodiscard]] snapshot state() const;
  // An internet hello arrived: the code's secret, spent now (right or wrong,
  // it never works again); nullopt when no code waits.
  [[nodiscard]] std::optional<std::vector<std::uint8_t>> spend();
  // The attempt was a wrong code: the window says so.
  void wrong_attempt();
  // spend() holds the listener and mappings for the attempt it starts (and
  // the transfer over it, whose rejoins come back the same way); release()
  // when that attempt, or its transfer, ends.
  void release();

 private:
  void run();
  void prepare(std::unique_lock<std::mutex>& lock);
  void release_locked(std::unique_lock<std::mutex>& lock);
  void accept_loop(net::listener* l);
  void notify();
  // #472 relays: waits at the relay server while the code does, handing on
  // each sender it is paired with (once that sender has spoken).
  void relay_loop(net::endpoint at, std::array<std::uint8_t, 16> token, std::string key, std::unique_ptr<link> first);

  options opts_;
  link_fn on_link_;
  std::function<void()> changed_;

  mutable std::mutex m_;
  std::condition_variable cv_;
  snapshot snap_;                                                       // under m_
  std::optional<std::vector<std::uint8_t>> secret_;  // under m_: while ready (the code's CPace password)
  std::chrono::steady_clock::time_point expires_{};                     // under m_
  bool want_code_ = false;                                              // under m_
  bool want_drop_ = false;                                              // under m_
  int holding_ = 0;                                                     // under m_: attempts and transfers running
  bool stop_ = false;                                                   // under m_
  std::unique_ptr<net::listener> listener_;                             // under m_
  std::thread accept_thread_;                                           // this object's thread only
  std::unique_ptr<mapping::mapper> mapper_;                             // the run thread only
  std::vector<mapping::mapped> mapped_;                                 // the run thread only
  // A word code (#472 PR 3): its socket, DHT client and keys, and the thread
  // that waits for the sender on the DHT and punches to it.
  std::shared_ptr<udp_hub> hub_;                                        // the run thread only
  std::unique_ptr<dht::client> dht_;                                    // the run thread only
  std::unique_ptr<dht::keys> dht_keys_;                                 // the run thread only
  std::thread meet_thread_;                                             // the run thread only
  std::atomic<bool> meet_stop_{false};
  std::thread relay_thread_;                                            // the run thread only
  std::thread relay_setup_thread_;                                      // the run thread only
  std::atomic<bool> relay_stop_{false};
  std::thread thread_;
};

// #472 PR 5: a way in from the internet while a transfer runs over a local
// link, for "Continue over the internet if Wi-Fi drops": a listener, the
// router asked for a port (and an IPv6 pinhole), and the places a sender
// elsewhere can reach (the mapped port, the public IPv6;
// no LAN address: it is the local link that went). Links that arrive go to
// `on_link` (the engine's rejoin path). Destroying it closes the listener and
// removes every mapping.
class way_in {
 public:
  using link_fn = std::function<void(std::unique_ptr<link>)>;
  ~way_in();
  way_in(const way_in&) = delete;
  way_in& operator=(const way_in&) = delete;
  // Blocks while the router answers (a worker's call); nullptr when there is
  // no place a sender elsewhere could reach.
  [[nodiscard]] static std::unique_ptr<way_in> open(bool loopback_only, const std::optional<mapping::options>& mapping,
                                                    link_fn on_link);
  [[nodiscard]] const std::vector<net::endpoint>& places() const noexcept { return places_; }

 private:
  way_in() = default;
  std::unique_ptr<net::listener> listener_;
  std::vector<net::endpoint> places_;
  std::unique_ptr<mapping::mapper> mapper_;
  std::vector<mapping::mapped> mapped_;
  std::thread accept_thread_;
};

// "<address>:<port>", or "<name>:<port>" resolved (the relay server only:
// the person typed it). nullopt: neither.
[[nodiscard]] std::optional<net::endpoint> relay_endpoint(const std::string& text);
// The relay field, "<address or name>:<port>#<key>", split (not resolved);
// nullopt with `why` (the window's words) when it is not one.
struct relay_server_field {
  std::string at;   // "<address or name>:<port>"
  std::string key;  // what only a waiter there proves (splice.h)
};
[[nodiscard]] std::optional<relay_server_field> relay_server_of(const std::string& text, std::string& why);
// How long a code waits for its relay server to answer before it is shown without it.
inline constexpr int kRelaySetupMs = 3000;

// The retry ladder: 1, 2, 5, 10 s, then every 30 s.
[[nodiscard]] int backoff_ms(int attempt) noexcept;
// What a round of connects to a code's places points to, for the window.
// `here_has_ipv6`: this machine has a public IPv6 address.
[[nodiscard]] std::string why_unreachable(const std::vector<net::endpoint>& places,
                                          const std::vector<link_race::outcome>& outcomes, bool here_has_ipv6);
// One round: a race over `places` (IPv6 first). The caller keeps `race` and
// joins it when it is done with the winner.
using connect_fn = std::function<result<std::unique_ptr<link>>(const net::endpoint&, int timeout_ms)>;
void start_round(link_race& race, const std::vector<net::endpoint>& places, const connect_fn& connect);

}  // namespace mv::sharepoint
