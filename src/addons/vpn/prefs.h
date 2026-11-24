// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The VPN add-on's settings (docs/plans/vpn-addon.md): vpn.json in its data
// folder, a few hundred bytes, written whole and renamed in.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "wire/secrets.h"

namespace mv::driftwire::vpn {

struct prefs {
  // "Your VPN server", "<address or name>:<port>#<key>" ("" = none). With a
  // secret store the service keeps the key there and this only the address.
  std::string server;
  // The Tunnel: "Tunnel through <server>" (off by default), and the loopback
  // port its proxy had, tried first next time (0: none yet).
  bool tunnel = false;
  std::uint16_t tunnel_port = 0;
  bool all_apps = true;  // "Use for all apps": the system proxy points at it
  // "Look like a web site": the server's TLS port (off by default), and which
  // port that is (443 unless the file says another).
  bool tls = false;
  std::uint16_t tls_port = 443;
  // "All traffic (VPN)" and "Block when the tunnel is down", both off by default.
  bool all_traffic = false;
  bool block = false;
  bool operator==(const prefs&) const = default;
};

inline constexpr const char* kPrefsFile = "vpn.json";
// The secret store's service and the key's name there: the VPN's own, never
// Driftwire's (its transfers keep a relay server of their own).
inline constexpr const char* kSecretService = "MediaViewer VPN";
inline constexpr const char* kServerKeyName = "server_key";
// The system proxy's record (sysproxy.h) in the data folder.
inline constexpr const char* kProxyRecordFile = "vpn_proxy.json";

[[nodiscard]] std::optional<prefs> load_prefs(const std::string& path);
[[nodiscard]] bool save_prefs(const std::string& path, const prefs& p);

// The Tunnel was Driftwire's until 2026-11-19. The first time the VPN starts
// with no vpn.json at `prefs_path`, it takes the Tunnel's settings from
// driftwire.json in `driftwire_dir`, if the Tunnel was ever used there, with
// the relay server's key from Driftwire's secret item into `mine` (into the
// file without a store), and the system proxy's record into `data_dir`, so a
// setting a crash left is still put back. Driftwire's own file and items are
// left as they are. The add-on's `driftwire_dir` is the sibling data folder
// (driftwire_data_dir); the driftwire program's is its own folder. True when
// something was taken.
bool adopt_driftwire(const std::string& driftwire_dir, const std::string& data_dir, const std::string& prefs_path,
                     secrets::store* driftwire, secrets::store* mine);
// <root>/VPN/data -> <root>/Driftwire/data on the Mac, <root>/vpn/data ->
// <root>/driftwire/data elsewhere (the host keeps each add-on's data in
// <root>/<name or id>/data); "" when `data_dir` has no such parent.
[[nodiscard]] std::string driftwire_data_dir(const std::string& data_dir);

}  // namespace mv::driftwire::vpn
