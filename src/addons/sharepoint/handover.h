// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Wi-Fi -> internet handover when both sides move (issue #472; docs/
// design/28 "Wi-Fi -> internet"). PR 5's handover goes to the places the
// receiver swapped over the local link; when the receiver's own network went
// too (or both did), those places are gone. Then both sides meet on the DHT
// as two four-word sides do (meet.h), under keys from the session itself:
//
//   - The keys: channel::handover_secret() (the resume secret under its own
//     context, so neither the pairing secret nor the next rejoin's psk can be
//     told from them), through crypto_kdf into the four parts of a word
//     code's set (dht_item.h). Only the two ends of the session hold it; it
//     changes at every rejoin, so each drop meets under fresh targets.
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
//     is up again some other way (Wi-Fi back, the swapped places).
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

#include "addons/sharepoint/channel.h"
#include "addons/sharepoint/dht.h"
#include "addons/sharepoint/meet.h"
#include "addons/sharepoint/wire.h"

namespace mv::sharepoint {

// The DHT keys for a session secret (handover_secret()).
[[nodiscard]] dht::keys handover_keys(std::span<const std::uint8_t, 32> secret);

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
  std::thread thread_;
};

}  // namespace mv::sharepoint
