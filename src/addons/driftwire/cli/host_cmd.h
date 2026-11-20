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
//   driftwire host setup            the guided run (host_setup.h, slice 5)
//   driftwire host name duckdns|none   the DuckDNS name; the token typed or pasted
//   driftwire host pair             a phone's pairing QR in the terminal
//   driftwire host vpn on [--tls-port N] | off   the Tunnel/VPN exit (slice 7);
//                                   `on` prints the relay line for the laptop
//   driftwire host pause | resume | quit   the tray's control (host_control.h):
//                                   written to host.json, taken within 2 s
//
// Its folder is <data dir>/host (0700): host.json (its settings), serve/ (the
// service's serve.json and secrets, as `driftwire serve --data` keeps them),
// relay/ (the relay's key, in a secret store of its own: made here, printed
// only in the relay line `host vpn on` shows), secrets/ (the keepers': the
// DuckDNS token; the host library's publisher key), state.json (what the
// running host has, for `host status`), host.lock, router.ledger. Beside it,
// <data dir>/library/.mediaviewer holds the host library's library.json, the
// "Catalogue" folder a phone paired from `host pair` opens in file mode.
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
#include "core/json.h"
#include "wire/net.h"
#include "wire/secrets.h"
#include "wire/serve_wire.h"
#include "wire/splice.h"

namespace mv::driftwire::cli {

inline constexpr std::uint16_t kHostServicePort = 7878;  // serve.json's port when the host makes one
inline constexpr int kHostRestartMs = 30000;              // after the child ended unasked (the SPK's rule)
inline constexpr int kHostRetryMs = 30000;                // a service that could not start, again
inline constexpr int kHostConfigEveryMs = 2000;           // serve.json and host.json read again
inline constexpr int kHostRootsEveryMs = 60000;           // each folder's drive looked at
inline constexpr int kHostUpdateEveryMs = 60000;          // the program looked at, for an update
inline constexpr int kHostChildCannotRun = 3;             // the child's exit: no use trying again
inline constexpr const char* kHostSinceEnv = "DRIFTWIRE_HOST_SINCE_MS";  // the supervisor's start, for its child
inline constexpr int kHostChildQuit = 4;                  // the child's exit: `host quit` asked; the supervisor stops
inline constexpr const char* kHostReason = "Driftwire is hosting your server";

// host.json.
struct host_settings {
  std::uint16_t relay_port = 47614;  // relay::kDefaultPort
  bool vpn = false;                  // forwarding for the Tunnel/VPN (slice 7's `host vpn on`)
  bool stay_awake = true;            // the "system required" request while hosting
  std::string ddns;                  // the DuckDNS subdomain ("home"; "": none), as ddns::parse_subdomain gives it
  std::int64_t ddns_stamp = 0;       // when the name or its token was last set: a running host takes them again
  // The relay's web-site look with the VPN on (docs/plans/vpn-stealth.md): on unless `--tls-port off`
  // (`look`), on `tls_port` (0: the relay's port + 1). The router maps outside 443 to it.
  bool look = true;
  std::uint16_t tls_port = 0;
  // The person's own domain for the VPN (`host name domain NAME`; "": none): the VPN code, the
  // look's SNI and its certificate carry it instead of the DuckDNS name. Its address is the
  // person's to keep (a CNAME to the DuckDNS name, which DuckDNS keeps).
  std::string domain;
  std::string library_id;            // the host library's id (`host pair`; "": none made yet)
  // The tray's control (docs/plans/self-hosted.md "Tray"; `host pause|resume|quit`):
  bool paused = false;               // nothing served, no relay, no stay-awake; kept until resumed
  std::int64_t quit_stamp = 0;       // unix ms of the last `host quit`: a host started before it stops
  // Clips the phone can't play (docs/plans/phone-clip-convert.md; `host convert on|off`): on
  // unless turned off, with the app's clip helper that the card names ("": none known).
  bool convert = true;
  std::string convert_helper;
};
[[nodiscard]] std::optional<host_settings> parse_host_settings(std::string_view json);
// The port the relay's web-site look listens on: 0 without the VPN or with the look off.
[[nodiscard]] std::uint16_t look_port(const host_settings& s) noexcept;
// The name the VPN is reached by from outside: the own domain, else the DuckDNS name ("": none).
[[nodiscard]] std::string vpn_name(const host_settings& s);
// A domain as typed ("VPN.Example.com.", "https://vpn.example.com/"): lowercased, at least two
// labels of [a-z0-9-] (not starting or ending with '-'), at most 253 long; nullopt otherwise.
[[nodiscard]] std::optional<std::string> parse_domain(std::string_view typed);
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
// <data dir>/library/.mediaviewer: the host library's Catalogue folder.
[[nodiscard]] std::string host_catalogue_of(const host_layout& l);
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
// The host library's Catalogue goes with the last folder of the person's.
// `host_catalogue`: host_catalogue_of(); another Catalogue (a desktop's claim) stays.
[[nodiscard]] std::string unshare_folder(serve::config& c, std::string_view label, std::string_view host_catalogue = {});

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
  // A `host quit` stamped at or after this (unix ms) stops the service; 0: when
  // it started. The supervisor gives its own start (kHostSinceEnv), so a quit
  // between its restart check and the child's start is not missed.
  std::int64_t quit_since_ms = 0;
  // The tests: the relay's options last (forward to a loopback echo server).
  std::function<void(relay::server::options&)> relay_tweak;
};

struct host_snapshot {
  std::uint16_t service_port = 0;  // 0: not serving (no folder, or its port taken)
  std::uint16_t relay_port = 0;    // 0: the relay is not running
  std::uint16_t relay_tls_port = 0;  // the web-site look's port (0: none)
  bool tls_failed = false;           // the look was asked for and could not listen: the relay runs without it
  bool vpn = false;
  bool awake = false;  // the request to stay awake is held
  std::size_t folders = 0, offline = 0;
  bool paused = false;
  // "Tray": the phones connected now, by the paired list's names ("A phone"
  // for one not on it), how (home network, over the internet, through the
  // relay), since when, and today's bytes and the rate now. Never an address.
  struct client {
    std::string name, via;  // via: "home", "internet", "relay"
    std::int64_t since = 0; // unix seconds
    std::uint64_t read = 0, written = 0, read_rate = 0, write_rate = 0;
  };
  std::vector<client> clients;
  // Today's (the computer's date) bytes in all, the rate now, and per device by name.
  struct day_device {
    std::string name;
    std::uint64_t read = 0, written = 0;
  };
  std::string day;
  std::uint64_t read_today = 0, written_today = 0, read_rate = 0, write_rate = 0;
  std::vector<day_device> devices_today;
  // Converting: turned on, a hardware encoder found ("VideoToolbox"), and a job running now.
  bool convert_on = false, convert_available = false, convert_busy = false;
  std::string convert_encoder;
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
  // A `host quit` written after this service started: the child ends with kHostChildQuit.
  [[nodiscard]] bool quit_asked() const noexcept;
  // One pass of the loop now (the files, the drives, the retries): the tests.
  void recheck_now();
  void stop() noexcept;

 private:
  host_service() = default;
  struct state;
  std::unique_ptr<state> st_;
};

// The host's files, for host_setup.cpp (each as `host share`, `host status`
// and the service use them: whole-file writes renamed in, owner-only, the
// serve.json lock).
namespace host_files {
[[nodiscard]] std::optional<std::string> read(const std::string& path, std::size_t max = 256 * 1024);
[[nodiscard]] bool write(const std::string& path, const std::string& text);
[[nodiscard]] std::unique_ptr<net::instance_lock> lock_config(const host_layout& l, int wait_ms = 3000);
[[nodiscard]] host_settings settings(const host_layout& l);
[[nodiscard]] bool save_settings(const host_layout& l, const host_settings& s);
[[nodiscard]] std::optional<serve::config> config(const host_layout& l, bool any_port);
[[nodiscard]] std::unique_ptr<secrets::store> store(const std::string& folder);
// The relay's key, made if there is none (never printed but in the relay line).
[[nodiscard]] std::optional<std::string> relay_secret(const host_layout& l);
// `host share`: "" when shared (as `shared_as`), else why not. `limit`: a backup folder's most (0: none).
[[nodiscard]] std::string share(const host_layout& l, const std::string& typed, const std::string& label, bool backup,
                                std::uint64_t limit, bool any_port, std::string& shared_as);
[[nodiscard]] bool running(const host_layout& l);
// The running host's state.json (none when it is not running).
[[nodiscard]] std::optional<json::value> state(const host_layout& l);
[[nodiscard]] std::string new_id();
int status(const host_layout& l, bool test_hooks);
int start_at_login(const args& a, const host_layout& l, bool test_hooks);
}  // namespace host_files

// Runs a.cmd == command::host. `test_hooks`: --test-loopback, --test-quick and
// port 0 honoured, the privilege check skipped, start-at-login refused.
[[nodiscard]] int run_host(const args& a, bool test_hooks, const char* version);

}  // namespace mv::driftwire::cli
