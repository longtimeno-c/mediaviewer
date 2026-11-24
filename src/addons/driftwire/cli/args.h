// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The driftwire command line's arguments (docs/design/28 "From a terminal").
// Pure and tested: argv in (UTF-8, without the program's own name), what to
// do out, or the usage error to print.
//
//   driftwire                      the session
//   driftwire send PATH... [--to NAME]
//   driftwire receive [--once]
//   driftwire code
//   driftwire devices
//   driftwire update
//   driftwire serve [init | status | devices | add NAME KEY | revoke NAME-OR-KEY | pair-code [SECONDS] | disown
//                    | passphrase set|reset] [--data DIR] [--passphrase-stdin | --no-passphrase]
//   driftwire host [setup | share PATH [--label L] [--backup [--limit GB|none]] | unshare LABEL | status | pair
//                   | name duckdns|none | name domain NAME|none | vpn on [--tls-port N|off] | vpn off
//                   | start-at-login on|off | stay-awake on|off
//                   | users [add NAME | role NAME ROLE | password NAME | remove NAME | apply]
//                     [--role R] [--folders L1,L2|all] [--input-stdin]] [--data DIR]
//                   | drives [list | choices DRIVE... | combine CHOICE DRIVE... --label L
//                             | replace SET DRIVE] [--json] [--confirm-stdin]] [--data DIR]
//   driftwire --help | --version
//   common: --save-to DIR, --name NAME (this run only), --plain, --no-color, --verbose
//
// Nothing secret is ever an argument: no code, no check digits, no relay
// key (they would sit in the shell's history and in `ps`). A code someone
// gave you is pasted into /send-to. So there is no option that takes one,
// and a command that takes no arguments refuses any (`driftwire code 123456`
// is an error, not a code). There is no --yes either: nothing is accepted or
// sent without the person at the terminal answering.
//
// The test build (MV_DRIFTWIRE_CLI_TEST_HOOKS) also takes its own options,
// refused by the product: --test-loopback (loopback networking only, no
// router, no beacon), --test-data DIR (a data folder of its own),
// --test-events (machine-readable lines on stderr for the end-to-end test) and
// --test-quick (`host`: its waits in a second or less instead of 30 s and a minute).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mv::driftwire::cli {

enum class command : std::uint8_t { session, send, receive, code, devices, update, serve, host, help, version };

// `driftwire serve`: the service itself (no terminal, docs/design/28 "The NAS
// service"), or one of its local chores.
enum class serve_action : std::uint8_t { run, init, devices, add, revoke, pair_code, disown, status, passphrase_set, passphrase_reset, grant, self_test };

// `driftwire host` (docs/plans/self-hosted.md §3): run the service and the
// relay together (run; `child` is the supervisor's own process, never typed),
// or change what it shares and how it starts.
enum class host_action : std::uint8_t { run, child, share, unshare, status, start_at_login, stay_awake, setup, name, pair, vpn,
                                        convert,  // host convert on [--helper PATH] | off (phone-clip-convert.md)
                                        // The LAN share's users (docs/plans/self-host-users-smb.md): `host users …`, and
                                        // the elevated step it starts (`host smb-apply`, never typed).
                                        users, smb_apply,
                                        drives,
                                        pause, resume, quit };  // the last three: host_control.h (the tray)

struct args {
  command cmd = command::session;
  std::vector<std::string> paths;  // send: as given (the shell has expanded them)
  std::string to;                  // send --to NAME
  bool once = false;               // receive --once
  std::string save_to;             // --save-to DIR, as given
  std::string name;                // --name NAME
  bool plain = false;
  bool no_color = false;
  bool verbose = false;  // the engine's log lines (never a path, a name or an address) in the transcript
  serve_action serve = serve_action::run;
  std::string serve_key;  // add: the device's key; revoke: a name or key
  std::string data;       // serve --data DIR: its folder, as given
  // serve init / passphrase: one line of stdin instead of the terminal, or none at all (init).
  bool passphrase_stdin = false;
  bool no_passphrase = false;
  // serve add / pair-code / grant: --for DURATION ("24h", "7d", "90m", seconds), or grant --forever.
  std::string for_duration;
  bool forever = false;
  host_action host = host_action::run;
  std::string host_path;  // host share: the folder, as given
  std::string label;      // host share --label; host unshare: the label
  bool backup = false;    // host share --backup
  // host share --backup --limit: "none", or GB (a decimal number); "" when not given.
  // On a folder shared already, it changes that folder's limit.
  std::string limit;
  bool watch = false;     // host status --watch (the tray's numbers, live)
  bool on = false;        // host start-at-login / stay-awake / vpn: on or off; host name: duckdns (true) or none
  // host vpn on --tls-port N|off: the relay's web-site look on port N (it is on by default), or off.
  std::uint16_t tls_port = 0;
  bool tls_off = false;
  // host convert on --helper PATH; serve --convert-helper PATH: the app's clip helper.
  std::string convert_helper;
  // host users / smb-apply (host_users.h checks them): the words after the action,
  // --role, --folders, --input-stdin (the app's passwords as lines on stdin).
  std::vector<std::string> host_words;
  std::string role, folders;
  bool input_stdin = false;
  // host drives (host_drives.h): its words are in `paths`; --label is the combined drive's name.
  bool json = false;           // host drives list|choices --json
  bool confirm_stdin = false;  // host drives combine|replace --confirm-stdin: the typed names on stdin
  // The test build's.
  bool test_loopback = false;
  std::string test_data;
  bool test_events = false;
  bool test_quick = false;
};

struct parsed_args {
  args a;
  std::string error;  // non-empty: a usage error ("driftwire: ..." is put before it)
};

[[nodiscard]] parsed_args parse_args(std::span<const std::string> argv, bool test_hooks);

// --help, and the line under a usage error.
[[nodiscard]] std::string help_text();
[[nodiscard]] std::string usage_line();

}  // namespace mv::driftwire::cli
