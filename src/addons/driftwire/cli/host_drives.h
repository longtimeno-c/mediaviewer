// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// `driftwire host drives …` and the host's drives keeper
// (docs/plans/self-host-drives.md §4–§5). The choices, the guards and the
// OS tools are drives.h's; this is the command line around them and the
// keeper that watches a combined set while the host runs.
//
//   driftwire host drives [list] [--json]           the drives, what is on each, the sets and their health
//   driftwire host drives choices DRIVE... [--json] the five choices for these drives, side by side
//   driftwire host drives combine CHOICE DRIVE... --label NAME [--confirm-stdin]
//   driftwire host drives replace SET DRIVE [--confirm-stdin]
//
// combine and replace show each drive's facts, refuse what the guards refuse,
// then ask for each drive's name to be typed (at the terminal, or one line
// each on stdin with --confirm-stdin: the card). Only the OS tool is
// elevated, through the OS's own prompt; this program never is.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "addons/driftwire/cli/args.h"
#include "addons/driftwire/cli/drives.h"
#include "addons/driftwire/cli/host_keepers.h"

namespace mv::driftwire::cli {

struct host_layout;

// What `host drives WORDS…` asks (pure; the tests).
enum class drives_action : std::uint8_t { list, choices, combine, replace };
struct drives_words {
  drives_action act = drives_action::list;
  drives::choice c = drives::choice::separate;
  std::vector<std::string> ids;  // the drives (choices, combine); replace: the new drive
  std::string set;               // replace: the set's name or id
  std::string error;             // a usage error
};
[[nodiscard]] drives_words parse_drives_words(std::span<const std::string> words);

// The guard for this computer: driftwire's data folder and the host's library,
// MediaViewer's own data folder, the host's shared folders. `images_under`:
// the test build's folder (never empty there: nothing but its images).
[[nodiscard]] drives::guard host_guard(const host_layout& l, const std::string& data_dir, bool test_hooks,
                                       const std::string& images_under);

// `list --json` and `choices --json` (the card reads them). No mount path.
[[nodiscard]] std::string listing_json(const drives::listing& l, const drives::guard& g);
[[nodiscard]] std::string choices_json(std::span<const drives::option> options);
// The rows for a terminal, side by side as far as it is wide (one under another below 100 columns).
[[nodiscard]] std::string choices_text(std::span<const drives::option> options, int columns);
// state.json's keeper line ("drives      Photos (mirror): healthy"); "" with no set.
[[nodiscard]] std::string drives_status_line(std::span<const drives::set_health> sets);

// Runs `host drives …`. The port and the input are the tests' to give.
struct drives_run {
  std::function<std::unique_ptr<drives::port>()> make_port;  // empty: drives::os_port
  std::function<bool(std::string&)> read_line;               // empty: stdin
  bool elevate = true;                                       // the test build: false (its own images only)
};
[[nodiscard]] int run_host_drives(const args& a, const host_layout& l, const std::string& data_dir, bool test_hooks,
                                  drives_run run = {});

// The keeper (host_keepers.h): every `every_ms` the sets' health, a status
// line, state.json's numbers ("drives_sets", "drives_health": the worst, as
// drives::health), a notification on a change for the worse, and the word for
// the phone (keeper_context::storage_changed).
struct drives_keeper_config {
  std::function<std::unique_ptr<drives::port>()> make_port;  // empty: drives::os_port
  int every_ms = 5 * 60 * 1000;
  bool on_loopback = false;  // the tests: runs with loopback_only too
};
[[nodiscard]] std::unique_ptr<host_keeper> make_drives_keeper(drives_keeper_config c = {});

}  // namespace mv::driftwire::cli
