// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Wi-Fi -> internet handover when both sides move (issue #472; docs/
// design/28 "Wi-Fi -> internet"). PR 5's handover goes to the places the
// receiver swapped over the local link; when the receiver's own network went
// too (or both did), those places are gone. Then both sides meet on the DHT
// as two four-word sides do (meet.h), under keys from the session itself:
//
//   - The keys: channel::handover_secret() (the resume secret under its own
//     context, so neither the pairing secret nor the rejoins' psk can be
//     told from them; fixed for the session), and the session's rejoins so
//     far (both ends count every rejoin they adopt), through crypto_kdf into
//     the four parts of a word code's set (dht_item.h). Only the two ends of
//     the session hold the secret; each move gives the next drop fresh
//     targets and a fresh stream id. Should the two counts ever differ (a
//     rejoin that completed on one end only), the meeting finds nobody and
//     the session ends after kRejoinMs, as with the toggle off.
//   - Roles: the sender publishes as the sender and looks up the receiver's
//     items, the receiver the other way round, so a side never meets its own
//     item.
//   - Pre-started: while a protocol 2 transfer runs over a local link with
//     the handover and DHT toggles on (both sides: the offer and the places
//     say so), each side opens its UDP hub and joins the DHT at once, so a
//     drop costs only a vote refresh, a publish, a lookup and a punch, inside
//     kRejoinMs.
//   - On a drop (the link suspect or down): the sender rejoins over the met
//     stream (channel::rejoin), the receiver hands it to the engine's rejoin
//     path (route_busy, accept_rejoin). A meeting stops as soon as the link
//     is up again some other way (Wi-Fi back, the swapped places). Every
//     meeting ends with this side's items saying "gone", met or not.
// Nothing is logged with an address. Portable (D9).
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>

#include "addons/driftwire/channel.h"
#include "addons/driftwire/dht.h"
#include "addons/driftwire/meet.h"
#include "addons/driftwire/wire.h"

namespace mv::driftwire {

// The DHT keys for a session secret (handover_secret()) after `rejoins` moves.
[[nodiscard]] dht::keys handover_keys(std::span<const std::uint8_t, 32> secret, std::uint32_t rejoins);

class dht_handover {
 public:
  struct options {
    dht::role me = dht::role::sender;
    dht::client::options client;
    bool loopback = false;      // the tests: loopback candidates and hub
    int wait_ms = kRejoinMs;    // how long one meeting looks for the other side
    int poll_ms = 200;          // how often the link is looked at while it is up
  };
  using link_fn = std::function<void(std::unique_ptr<link>)>;
  // `armed`: whether the other side meets too (the sender: the receiver's
  // places said so); a drop with it false meets nobody. `on_ready`: the hub
  // is open and the DHT answered (the window, the tests). Both are called on
  // this object's thread, briefly.
  dht_handover(std::shared_ptr<channel> ch, options o, std::function<bool()> armed, link_fn on_link,
               std::function<void()> on_ready = {});
  ~dht_handover();  // stops a meeting under way, joins, closes the hub
  // Any thread: stop without waiting (the engine retires it and joins it
  // once finished(), off the transfer's way).
  void request_stop();
  [[nodiscard]] bool finished() const noexcept { return finished_.load(); }
  dht_handover(const dht_handover&) = delete;
  dht_handover& operator=(const dht_handover&) = delete;

  [[nodiscard]] bool ready() const noexcept { return ready_.load(); }
  // Streams met and handed on (the tests).
  [[nodiscard]] std::uint32_t meetings() const noexcept { return meetings_.load(); }
  // How long the last meeting took, from the drop seen to the stream (ms).
  [[nodiscard]] std::int64_t last_meet_ms() const noexcept { return last_meet_ms_.load(); }

 private:
  void run();
  [[nodiscard]] bool stopped() const;
  // Waits up to `ms`; false once stopped.
  bool pause(int ms);

  std::shared_ptr<channel> ch_;
  options opts_;
  std::function<bool()> armed_;
  link_fn on_link_;
  std::function<void()> on_ready_;
  mutable std::mutex m_;
  std::condition_variable cv_;
  bool stop_ = false;  // under m_
  std::atomic<bool> ready_{false};
  std::atomic<std::uint32_t> meetings_{0};
  std::atomic<std::int64_t> last_meet_ms_{-1};
  std::atomic<bool> finished_{false};
  std::int64_t seq_floor_ = 0;  // this object's thread only: above the last withdraw
  std::thread thread_;
};

}  // namespace mv::driftwire
