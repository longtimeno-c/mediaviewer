// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// `driftwire host` (docs/plans/self-hosted.md §3, slice 2): this computer as
// the phones' server and the laptop's relay, the job a NAS would have done.
//
//   driftwire host                  the supervisor: runs `host child` and starts
//                                   it again 30 s after it ends unasked (a crash,
//                                   a service that could not run); ends 75 when
//                                   the program was replaced by an update, so
//                                   the start-at-login entry starts the new one
//   driftwire host child            the services, in one process: serve::server
//                                   for the shared folders and the relay
//                                   (splice.h), the power request, the keepers
//                                   (host_keepers.h); stops when its stdin ends
//   driftwire host share PATH [--label L] [--backup] | unshare LABEL
//   driftwire host status           name, ports, folders (by label), phones,
//                                   VPN, staying awake, start at login: never a
//                                   path or an address (rule 6)
//   driftwire host start-at-login on|off | stay-awake on|off
//
// Its folder is <data dir>/host (0700): host.json (its settings), serve/ (the
// service's serve.json and secrets, as `driftwire serve --data` keeps them),
// relay/ (the relay's key, in a secret store of its own: made here, never
// printed; slice 7 shows the relay line), secrets/ (the keepers'), state.json
// (what the running host has, for `host status`), host.lock.
//
// What it changes while it runs comes from those files: a `host share` from
// another terminal, the desktop's owner edits (written by the service itself),
// a drive's new letter. serve.json is read again every 2 s (one small file),
// host.json too; every minute each folder's drive is looked at (volume.h) and
// one whose letter changed is moved to it. A service or the relay that could
// not start (its port taken) is tried again every 30 s without stopping the
// other.
//
// It refuses to run as root or elevated, like serve: its keys would be the
// administrator's, and it faces the internet.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "addons/driftwire/cli/args.h"
#include "addons/driftwire/cli/host_keepers.h"
#include "addons/driftwire/cli/host_proc.h"
#include "addons/driftwire/cli/volume.h"
#include "wire/serve_wire.h"

namespace mv::driftwire::cli {

inline constexpr std::uint16_t kHostServicePort = 7878;  // serve.json's port when the host makes one
inline constexpr int kHostRestartMs = 30000;              // after the child ended unasked (the SPK's rule)
inline constexpr int kHostRetryMs = 30000;                // a service that could not start, again
inline constexpr int kHostConfigEveryMs = 2000;           // serve.json and host.json read again
inline constexpr int kHostRootsEveryMs = 60000;           // each folder's drive looked at
inline constexpr int kHostUpdateEveryMs = 60000;          // the program looked at, for an update
inline constexpr int kHostChildCannotRun = 3;             // the child's exit: no use trying again
inline constexpr const char* kHostReason = "Driftwire is hosting your server";

// host.json.
struct host_settings {
  std::uint16_t relay_port = 47614;  // relay::kDefaultPort
  bool vpn = false;                  // forwarding for the Tunnel/VPN (slice 7's `host vpn on`)
  bool stay_awake = true;            // the "system required" request while hosting
};
[[nodiscard]] std::optional<host_settings> parse_host_settings(std::string_view json);
[[nodiscard]] std::string host_settings_json(const host_settings& s);

struct host_layout {
  std::string folder;        // <data>/host
  std::string serve;         // <data>/host/serve
  std::string serve_config;  // <data>/host/serve/serve.json
  std::string relay;         // <data>/host/relay
  std::string settings;      // <data>/host/host.json
  std::string state;         // <data>/host/state.json
  std::string lock;          // <data>/host/host.lock
};
[[nodiscard]] host_layout host_layout_of(const std::string& data_dir);
// Makes the folders, each this user's alone (secrets::owner_only_folder).
[[nodiscard]] bool make_host_folders(const host_layout& l);

// ---- what is shared (pure, on a config) ---------------------------------------------------

// The label a folder gets without --label: its last name ("Photos", "E" for E:\).
[[nodiscard]] std::string default_label(const std::string& abs_path);
// Adds the folder; "" when done, else why not (in words for the person).
// `id`: 16 lowercase hex digits. `host_folder`: never shared, nor anything holding it.
[[nodiscard]] std::string share_folder(serve::config& c, const std::string& abs_path, const std::string& label,
                                       bool backup, const volume_facts& facts, const std::string& id,
                                       const std::string& host_folder);
[[nodiscard]] std::string unshare_folder(serve::config& c, std::string_view label);

// ---- the supervisor's rule ------------------------------------------------------------------

enum class after_child : std::uint8_t { stop, restart, fail };
// What to do once the child ended: `stopping` when the supervisor asked it to.
[[nodiscard]] after_child decide_after_child(const child_process::ended& e, bool stopping);

// ---- the service process (in-process for the tests) ---------------------------------------

struct host_service_options {
  host_layout where;
  bool loopback_only = false;  // the tests: serve and the relay on 127.0.0.1, ports the OS picks
  bool any_port = false;       // serve.json's port 0 accepted (the test build)
  int config_every_ms = kHostConfigEveryMs;
  int roots_every_ms = kHostRootsEveryMs;
  int retry_ms = kHostRetryMs;
  bool power = true;  // the tests: never ask the OS to stay awake
  // volume.h's, unless the tests give their own.
  std::function<bool(const serve::root_config&)> present;
  std::function<std::optional<std::string>(const serve::root_config&)> locate;
  keeper_factories keepers;
};

struct host_snapshot {
  std::uint16_t service_port = 0;  // 0: not serving (no folder, or its port taken)
  std::uint16_t relay_port = 0;    // 0: the relay is not running
  bool vpn = false;
  bool awake = false;  // the request to stay awake is held
  std::size_t folders = 0, offline = 0;
};

class host_service {
 public:
  // Starts what it can and returns; nullptr (with `why`) when it cannot run at
  // all (its folders are not this user's, its keys cannot be read or made).
  [[nodiscard]] static std::unique_ptr<host_service> start(host_service_options o, std::string& why);
  ~host_service();
  host_service(const host_service&) = delete;
  host_service& operator=(const host_service&) = delete;

  [[nodiscard]] host_snapshot snapshot() const;
  // One pass of the loop now (the files, the drives, the retries): the tests.
  void recheck_now();
  void stop() noexcept;

 private:
  host_service() = default;
  struct state;
  std::unique_ptr<state> st_;
};

// Runs a.cmd == command::host. `test_hooks`: --test-loopback, --test-quick and
// port 0 honoured, the privilege check skipped, start-at-login refused.
[[nodiscard]] int run_host(const args& a, bool test_hooks, const char* version);

}  // namespace mv::driftwire::cli
