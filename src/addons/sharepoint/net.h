// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Sharepoint's network port (D9: a portable header, net_posix.cpp for macOS
// and the portable build, net_win.cpp over Winsock).
//
// Only Sharepoint opens these, only while its window is open or receiving is
// on (docs/design/28 "What stays put"): the idle viewer has no socket. IPv4 on
// the local network; nothing here resolves a name or reaches past the subnet
// (the beacon goes out with a TTL of 1). Nothing is logged with an address.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "addons/sharepoint/link.h"
#include "core/result.h"

namespace mv::sharepoint::net {

struct endpoint {
  std::string address;  // dotted IPv4, as a beacon's sender
  std::uint16_t port = 0;
};

// A TCP listener on a port the OS picks: on every interface, or on loopback
// only (the tests).
class listener {
 public:
  virtual ~listener() = default;
  [[nodiscard]] virtual std::uint16_t port() const noexcept = 0;
  // The next connection; status::timeout when none came within `timeout_ms`;
  // status::cancelled after shutdown().
  [[nodiscard]] virtual result<std::unique_ptr<link>> accept(int timeout_ms) = 0;
  virtual void shutdown() noexcept = 0;
};
[[nodiscard]] result<std::unique_ptr<listener>> listen(bool loopback_only = false);

// Connects within `timeout_ms`.
[[nodiscard]] result<std::unique_ptr<link>> connect(const endpoint& to, int timeout_ms);

// The discovery socket: UDP on `port`, joined to kMulticastGroup, sharing the
// port with other MediaViewers on this machine.
class beacon_socket {
 public:
  virtual ~beacon_socket() = default;
  // To the multicast group and to the broadcast address, TTL 1.
  [[nodiscard]] virtual expected send(std::span<const std::uint8_t> datagram) = 0;
  // One datagram and its sender's address; status::timeout when none.
  [[nodiscard]] virtual result<std::size_t> receive(std::span<std::uint8_t> into, int timeout_ms,
                                                    std::string& from_address) = 0;
  virtual void shutdown() noexcept = 0;
};
[[nodiscard]] result<std::unique_ptr<beacon_socket>> open_beacon_socket(std::uint16_t port);

// The name people know this machine by ("Tristan's MacBook Air", the
// computer name on Windows). Never a user name.
[[nodiscard]] std::string machine_name();

// The person's Downloads folder, where received files land unless they pick
// another (#442, as AirDrop): FOLDERID_Downloads on Windows, ~/Downloads on
// the Mac. "" when there is none. A local read; never blocks on the network.
[[nodiscard]] std::string downloads_folder();

// One process at a time: each MediaViewer window is its own process
// (docs/design/18 "One host process"), and only one of them keeps receiving
// at launch when Keep receiving was on (#442). Held until destroyed; the OS
// lets go of it if the process dies. nullptr when another process holds it.
class instance_lock {
 public:
  virtual ~instance_lock() = default;
};
[[nodiscard]] std::unique_ptr<instance_lock> try_lock_instance(const std::string& path);

}  // namespace mv::sharepoint::net
