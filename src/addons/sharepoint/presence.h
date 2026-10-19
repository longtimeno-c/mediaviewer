// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// How two MediaViewers find each other (docs/design/28 "Finding each other").
//
// Two bearers, one shape: each announces this client (its random session id,
// its machine name) and reports the others it hears, and each can open a link
// to one of them. The engine runs them only while the window is open or
// receiving is on: the idle viewer announces nothing and polls nothing.
//
//   wifi_discovery  a UDP beacon a second on the local network (net.h), the
//                   peer reached over TCP at the beacon's address.
//   bluetooth       BLE: an advertised service, a characteristic each way as
//                   a byte pipe (bt.h). Discovers and carries small sends when
//                   the two are not on one network.
//
// Both are interfaces so the tests run the engine over an in-memory bus.
// Portable (D9).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "addons/sharepoint/link.h"
#include "addons/sharepoint/net.h"
#include "addons/sharepoint/wire.h"

namespace mv::sharepoint {

class discovery {
 public:
  using seen_fn = std::function<void(const beacon& who, const std::string& address)>;
  using tick_fn = std::function<void()>;
  virtual ~discovery() = default;
  // Announces `self` and reports every other client heard (its own beacons
  // are the caller's to ignore by id), on the discovery's own thread; `tick`
  // about once a second there too, so the engine can forget the silent.
  // False when the bearer cannot start (no network).
  [[nodiscard]] virtual bool start(const beacon& self, seen_fn seen, tick_fn tick) = 0;
  // Stops announcing and joins the thread. Idempotent.
  virtual void stop() noexcept = 0;
};

// The real one: UDP on `port` (kBeaconPort), a beacon a second.
[[nodiscard]] std::unique_ptr<discovery> wifi_discovery(std::uint16_t port = kBeaconPort);

// A peer heard on Bluetooth: an opaque handle the bearer understands.
struct bt_peer {
  std::string handle;
  std::string id;    // its session id (from the advertisement / info read)
  std::string name;
  std::uint32_t protocol = kProtocol1;  // from its INFO's "p" (#472)
};

class bluetooth {
 public:
  using peer_fn = std::function<void(const bt_peer&)>;
  using link_fn = std::function<void(std::unique_ptr<link>)>;
  virtual ~bluetooth() = default;
  // Advertises (id, name) and scans. Peers and incoming links are reported on
  // the bearer's own threads. False when Bluetooth is off, not permitted, or
  // absent; `why` then says which, for the screen.
  [[nodiscard]] virtual bool start(const std::string& id, const std::string& name, peer_fn on_peer,
                                   link_fn on_link, std::string& why) = 0;
  virtual void stop() noexcept = 0;
  // A link to a peer it reported.
  [[nodiscard]] virtual result<std::unique_ptr<link>> connect(const std::string& handle, int timeout_ms) = 0;
};

// The platform's bearer (bt_mac.mm, bt_win.cpp), or nullptr where there is
// none (bt_none.cpp: the portable build).
[[nodiscard]] std::unique_ptr<bluetooth> make_bluetooth();

}  // namespace mv::sharepoint
