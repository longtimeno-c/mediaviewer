// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Tunnel's system proxy (owner, 2026-11-12; docs/plans/driftwire-menu-and-tunnel.md
// slice 3, docs/design/28 "The Tunnel"): while the Tunnel is on and "Use for
// all apps" is on, the computer's automatic proxy setting (a PAC URL) points
// at the script the Tunnel's proxy serves (tunnel.h pac_url), so every app
// that follows the system setting goes through the relay with nothing to
// paste. The script falls back to DIRECT, so apps keep the network if the
// proxy stops answering.
//
// The OS calls stay behind this port (D9), one file per OS, as power.h and
// secrets.h: sysproxy_mac.cpp (SystemConfiguration: every enabled network
// service's ProxyAutoConfigURLString, authorised by an administrator once a
// run), sysproxy_win.cpp (the per-user WinINet setting of the LAN connection,
// INTERNET_PER_CONN_AUTOCONFIG_URL: no administrator), sysproxy_none.cpp
// (Linux and the tests: none).
//
// Never leaves a setting behind: `set` hands back what was there, which the
// engine keeps in a file (sysproxy_record) before anything else, and
// `restore` puts it back on off, on quit and at the next start after a
// crash, but only where the setting is still the URL Driftwire set (one the
// person changed since is theirs). Nothing here logs a URL or a service name.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "core/result.h"

namespace mv::driftwire::sysproxy {

class port {
 public:
  virtual ~port() = default;
  // Whether this OS has a setting here to change ("" when it does), else why
  // not, in the window's words.
  [[nodiscard]] virtual std::string unsupported() const = 0;
  // Points the automatic proxy URL at `pac_url`. On success, what was there
  // before, as text only this port reads back (kept in the record file).
  // status::permission_denied when the person declined (an administrator
  // prompt cancelled); status::io otherwise.
  [[nodiscard]] virtual result<std::string> set(const std::string& pac_url) = 0;
  // Puts `saved` back wherever the setting is still `pac_url`. True when
  // nothing of Driftwire's is left (done, or nothing was ours any more).
  [[nodiscard]] virtual bool restore(const std::string& saved, const std::string& pac_url) = 0;
};

// This OS's port (sysproxy_<os>.cpp).
[[nodiscard]] std::unique_ptr<port> os_port();

// What the engine keeps in its data folder while the setting is Driftwire's
// (driftwire_proxy.json): the URL it set and what was there before. Pure
// text in and out (tested); the engine writes and reads the file.
struct record {
  std::string pac_url;
  std::string saved;
};
[[nodiscard]] std::string record_json(const record& r);
[[nodiscard]] std::optional<record> parse_record(std::string_view json);
inline constexpr const char* kRecordFile = "driftwire_proxy.json";
// The record at `path` (nullopt: none, or not one); written whole and
// flushed before the call returns (false on any I/O error).
[[nodiscard]] std::optional<record> load_record(const std::string& path);
[[nodiscard]] bool save_record(const std::string& path, const record& r);

}  // namespace mv::driftwire::sysproxy
