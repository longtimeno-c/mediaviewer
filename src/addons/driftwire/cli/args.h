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
// router, no beacon), --test-data DIR (a data folder of its own) and
// --test-events (machine-readable lines on stderr for the end-to-end test).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mv::driftwire::cli {

enum class command : std::uint8_t { session, send, receive, code, devices, update, help, version };

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
  // The test build's.
  bool test_loopback = false;
  std::string test_data;
  bool test_events = false;
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
