// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Settings → Add-ons → Phone access → Set up the service… → This computer
// (docs/plans/self-hosted.md §8, slice 6): this computer as the phones'
// server, set up from the card with no terminal. The app does not host
// anything itself and does not reimplement the host: it runs the installed
// `driftwire` program's own commands (`host share`, `host start-at-login on`,
// which also starts it now), writes the DuckDNS name where `host name duckdns`
// writes it (that command asks at a terminal only), and reads the files the
// running host keeps for `host status` (docs/design/28 "On your own computer").
//
// Worker threads only (the phone link's): finding the program stats files,
// running it waits for it, reading the host's files is I/O. Nothing here
// logs a path, a name, an address or the token (rule 6). Portable (D9): the
// places differ per OS in pure functions every OS's tests run; io/child_process
// is the only process port.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace mv::phone::self_host {

// Not `linux`: GCC predefines it.
enum class os : std::uint8_t { mac, windows, linux_os };
[[nodiscard]] os this_os() noexcept;

// What the program and its data folder are found from (the environment).
struct env {
  std::string home;            // $HOME, %USERPROFILE%
  std::string local_app_data;  // %LOCALAPPDATA% (Windows)
  std::string xdg_state_home;  // $XDG_STATE_HOME (Linux), "" for the default
  std::string xdg_config_home; // $XDG_CONFIG_HOME (Linux), "" for the default
  std::string path;            // $PATH / %Path%, as the OS separates it
};
[[nodiscard]] env read_env();

// driftwire's data folder as the command line picks it (cli/data_dir.h):
// macOS ~/Library/Application Support/Driftwire, Windows %LOCALAPPDATA%\Driftwire,
// Linux ${XDG_STATE_HOME:-~/.local/state}/driftwire. "" when there is no home.
[[nodiscard]] std::string data_dir_for(os o, const env& e);
// Where an installed driftwire may be, in the order they are tried
// (docs/design/28 "Installing"): the installers' own places first (an app
// started from the Finder or the Start menu has no shell's PATH), then PATH.
[[nodiscard]] std::vector<std::string> program_candidates(os o, const env& e);
// The start-at-login entry `host start-at-login on` writes (cli/autostart_*.cpp):
// the LaunchAgent, the task's XML kept in the host's folder, the systemd unit.
[[nodiscard]] std::string login_entry_for(os o, const env& e, const std::string& host_folder);

struct where {
  std::string program;   // the driftwire program; "" when none is installed
  std::string data_dir;  // its data folder; the host's is data_dir/host
  // Given after every `host` command: none for the person's own host, or the
  // tests' {"--test-data", DIR} for a test build of the program.
  std::vector<std::string> data_args;
  std::string login_entry;  // "" for none to look at (the tests)
};
// The add-on's own copy of the program (bundled.h), set at its load: find()
// takes it before every other place. "" for none (a build without it).
void set_bundled(std::string program);
[[nodiscard]] std::string bundled();
// The app's clip helper (module_self::app_clip_helper), set at the add-on's load: what the host is
// told to convert clips for phones with (docs/plans/phone-clip-convert.md). "" for none.
void set_clip_helper(std::string program);
[[nodiscard]] std::string clip_helper();
// The person's: the add-on's copy, else the first candidate that is a file, and
// the default data folder.
[[nodiscard]] where find();

// Whether the start-at-login entry (w.login_entry) starts `program`: its text
// names that path (as written, or XML-escaped as the LaunchAgent and the task
// write it). False with no entry.
[[nodiscard]] bool login_entry_runs(const where& w, const std::string& program);

// What `host status` would say, read from the host's files: never a path or
// an address. A running host holds host.lock; its state.json has its ports.
struct host_status {
  bool installed = false;
  bool set_up = false;    // serve.json has a shared folder
  bool running = false;
  bool at_login = false;  // the start-at-login entry is there
  std::uint16_t service_port = 0, relay_port = 0;  // 0: not listening (or not running)
  std::uint16_t configured_port = 0;               // serve.json's port
  std::uint16_t outside_port = 0;                  // the router's outside port for the service, once mapped
  std::size_t folders = 0, offline = 0;
  std::string name;     // the service's name (serve.json)
  std::string duckdns;  // the DuckDNS subdomain (host.json's "ddns"), "" for none
  std::string domain;   // the VPN's own domain (host.json's "domain"), "" for none
  // The keepers' lines (the router, DuckDNS), as the running host wrote them.
  std::vector<std::string> keepers;
  // "Tray": hosting off (`host pause`), and today's usage and the phones
  // connected, in the menu's words (host_tray::usage_lines).
  bool paused = false;
  std::vector<std::string> stats;
  // Clips the phone can't play (docs/plans/phone-clip-convert.md): host.json's switch and the
  // helper it was given, and what the running host found ("VideoToolbox"; "" for none).
  bool convert_on = true, convert_available = false, convert_busy = false;
  std::string convert_helper, convert_encoder;
  // The VPN (`host vpn on|off`, docs/design/28 "Self-hosting"): `vpn` is what
  // host.json asks for, `vpn_on` what the running host's relay does now (it
  // takes host.json within 2 s); the web-site look's port (0: none) and whether
  // it could listen; the relay's port as the router maps it from outside.
  bool vpn = false, vpn_on = false, tls_failed = false;
  std::uint16_t tls_port = 0, relay_outside_port = 0, tls_outside_port = 0;
  std::uint16_t relay_tls_port = 0;  // the running relay's look port (tls_port is what host.json asks)
};
[[nodiscard]] host_status read_status(const where& w);

// state.json's fields (cli/host_cmd.cpp state_json). nullopt: not that shape.
struct running_state {
  std::int64_t pid = 0;
  std::uint16_t service_port = 0, relay_port = 0;
  std::uint16_t service_outside_port = 0;  // the router keeper's, once mapped (0: not mapped)
  bool vpn = false, awake = false;
  std::size_t folders = 0, offline = 0;
  std::vector<std::string> keepers;
  // The tray (docs/plans/self-hosted.md "Tray"):
  bool paused = false;             // `host pause`
  bool tls_failed = false;         // the web-site look could not listen
  std::uint16_t relay_outside_port = 0;  // the router keeper's for the relay, once mapped
  std::uint16_t tls_outside_port = 0;    // and for the web-site look
  std::uint16_t relay_tls_port = 0;      // the look's port as the running relay listens (0: none)
  // The phones connected now (state.json "clients"): the paired name, how
  // ("home", "internet", "relay"), since when (unix s), today's bytes, the rate.
  struct client {
    std::string name, via;
    std::int64_t since = 0;
    std::uint64_t read = 0, written = 0, read_rate = 0, write_rate = 0;
  };
  std::vector<client> clients;
  // Today's bytes in all (state.json "usage"), and per device by name.
  struct day_device {
    std::string name;
    std::uint64_t read = 0, written = 0;
  };
  std::string day;
  std::uint64_t read_today = 0, written_today = 0, read_rate = 0, write_rate = 0;
  std::vector<day_device> devices_today;
  // Combined drives (issue #686 §3, PR #692's drives keeper): its state.json
  // keeper line ("drives      Photos (mirror): healthy", a line per set) and
  // its "drives_health" number, the worst set (drives::health: 0 healthy,
  // 1 rebuilding, 2 degraded, 3 offline, 4 failing; -1: no set).
  std::vector<std::string> drive_lines;  // "Photos (mirror): healthy", the advice after ';' left out
  int drives_health = -1;
  // Converting (state.json "convert"): on, a hardware encoder found (its name), a job running now.
  bool convert_on = false, convert_available = false, convert_busy = false;
  std::string convert_encoder;
};
// The keepers' lines are tidied for a card: "DuckDNS     home…" becomes
// "DuckDNS: home…", each line of a line trimmed.
[[nodiscard]] std::optional<running_state> parse_state(std::string_view json);
[[nodiscard]] std::string tidy_keeper_line(std::string_view line);
// host.json's DuckDNS subdomain ("ddns"), "" when none or not that shape.
[[nodiscard]] std::string parse_duckdns(std::string_view host_json);
// host.json's VPN ("vpn") and the web-site look's port: on with the VPN unless
// "look" is false, on "tls_port" (0 there: the relay's port + 1), as
// cli::look_port reads it; 0 here: none.
struct vpn_wanted {
  bool on = false;
  std::uint16_t tls_port = 0;
};
[[nodiscard]] vpn_wanted parse_vpn(std::string_view host_json);
// The relay line `host vpn on` prints for the laptop ("name.duckdns.org:PORT#KEY",
// indented on a line of its own), or the web-site look's whole code
// ("mediaviewer://relay-tls#name:443#KEY"), "" when the output has neither. It
// holds the relay's key: for the person's own screen, never logged.
[[nodiscard]] std::string relay_line_in(std::string_view output);

// ---- running the program ------------------------------------------------------------------

struct ran {
  int code = -1;        // its exit code; -1 when it was killed or crashed
  std::string output;   // what it printed, standard output and error (a line or two)
};
// `driftwire host ARGS... DATA_ARGS`, `input` written to its standard input one
// line each (then closed), killed after `timeout_ms`. status::not_found with no
// program; status::io when it cannot be started.
[[nodiscard]] result<ran> run_host(const where& w, std::vector<std::string> args,
                                   std::span<const std::string> input = {}, int timeout_ms = 30000);

// A folder the card asks to share.
struct folder_choice {
  std::string path, label;  // label "" for the command's own (the folder's name)
  bool backup = false;
};
// The folders serve.json shares now, for a setup run again.
struct shared_folder {
  std::string path, label;
};
[[nodiscard]] std::vector<shared_folder> shared_folders(const where& w);
// A path as the OS would match it: canonical where it exists, no trailing
// separator, and on Windows and the Mac in one case.
[[nodiscard]] std::string same_path_key(const std::string& path);
// The command line's label for a folder with none given (default_label): its
// last name, a drive root's letter.
[[nodiscard]] std::string folder_label(const std::string& path);
// A label for each folder, none the same as another or as one in `taken`
// (the program refuses a label in use): "Photos", "Photos 2".
[[nodiscard]] std::vector<std::string> unique_labels(std::span<const folder_choice> f, std::span<const std::string> taken);
// Whether something answers on this computer's loopback at `port`.
[[nodiscard]] bool answers(std::uint16_t port, int timeout_ms = 1000);
// Names the host on DuckDNS as `host name duckdns` does (that command takes
// them from a terminal only): the token as `ddns_token` in the host's own
// secret store (<data>/host/secrets, ddns::save_token's item), the subdomain as
// host.json's "ddns" with "ddns_stamp" (now) so a running host takes the new
// name; host.json's other keys are kept. Never an argument, never logged.
inline constexpr const char* kTokenSecret = "ddns_token";  // ddns::kTokenSecret
[[nodiscard]] expected set_duckdns(const where& w, const std::string& subdomain, const std::string& token);
// host.json's text with "ddns" and "ddns_stamp" set, every other key as it was.
[[nodiscard]] std::string with_duckdns(std::string_view host_json, const std::string& subdomain, std::int64_t stamp);
// This computer's address on its own network: on the route to the default
// gateway, else its first private IPv4. "" when it is on none.
[[nodiscard]] std::string lan_address();

// The LAN share's addresses as the host's last elevated step left them
// (`share_state` in <data>/host/serve/secrets, docs/plans/self-host-users-smb.md §7):
// "\\PC\Photos" and "smb://pc.local/Photos" for each share. Empty: none made.
// For the person's own screen only, never logged.
[[nodiscard]] std::vector<std::string> share_addresses(const where& w);
// `host users apply --input-stdin`'s lines: "NAME<TAB>PASSWORD" for each.
struct user_password {
  std::string name, password;
};
[[nodiscard]] std::vector<std::string> apply_lines(std::span<const user_password> p);

// The `host share` arguments for one folder.
[[nodiscard]] std::vector<std::string> share_args(const std::string& path, const std::string& label, bool backup);

// ---- the setup's inputs (pure) ------------------------------------------------------------

// The card's folders, checked: at least one folder phones read (not every one a
// backup folder), absolute paths, no two the same. "" when good, else why
// ("no_folders", "no_library", "bad_path", "twice").
[[nodiscard]] std::string check_folders(std::span<const folder_choice> f);
// The one the library goes in (the link's folder): the first that is not a backup folder.
[[nodiscard]] const folder_choice* library_folder(std::span<const folder_choice> f);
// A DuckDNS subdomain as typed ("home", "home.duckdns.org", "HOME.DuckDNS.org.")
// to "home"; nullopt for anything else (ddns::parse_subdomain's rule).
[[nodiscard]] std::optional<std::string> parse_subdomain(std::string_view typed);
// A DuckDNS token as pasted: a UUID, lower-cased; nullopt for anything else
// (ddns::parse_token's rule: only hex and '-' ever reach the program).
[[nodiscard]] std::optional<std::string> parse_token(std::string_view typed);
// "home.duckdns.org:7878", the phone's route from outside.
[[nodiscard]] std::string public_route(const std::string& subdomain, std::uint16_t port);
// "a.b.c.d:port" (or "[v6]:port") for an address, as the phone dials it on the network.
[[nodiscard]] std::string host_port(const std::string& address, std::uint16_t port);

}  // namespace mv::phone::self_host
