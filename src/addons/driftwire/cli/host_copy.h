// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Keep a copy (docs/plans/self-host-drives-keep-a-copy.md): which drives are
// copied where (<host>/copies.json), how each copy stands
// (<host>/copies-state.json, written by the keeper), the words for both, and
// the copy keeper: a host keeper (host_keepers.h) whose thread runs
// drive_copier.h's passes, one copy after another.
//
//   driftwire host drives copy FROM TO     start keeping a copy of FROM's volume on TO
//   driftwire host drives copy-stop FROM   stop (the copied files stay on TO)
//
// (host_drives.cpp runs both.) The keeper passes again every `every_ms`, sooner
// while something is held back or failed, and at once when copies.json
// changes. A copy's drives are kept by path and, on Windows, by volume GUID
// (volume.h): a drive whose letter changed is found again.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "addons/driftwire/cli/host_keepers.h"

namespace mv::driftwire::cli {

struct copy_entry {
  std::string label;        // the source's name (the copy's folder is named after it)
  std::string to_name;      // the copy drive's name, for the words
  std::string from, from_volume;  // the source volume's root, and its GUID path on Windows
  std::string to, to_volume;      // the copy drive's root
};
[[nodiscard]] std::string copies_json(std::span<const copy_entry> copies);
// nullopt: not a copies.json (the caller keeps none: nothing is copied by mistake).
[[nodiscard]] std::optional<std::vector<copy_entry>> copies_from_json(std::string_view text);
[[nodiscard]] std::vector<copy_entry> read_copies(const std::string& host_folder);
[[nodiscard]] bool write_copies(const std::string& host_folder, std::span<const copy_entry> copies);

// How one copy stands, as the keeper last saw it.
enum class copy_state : std::uint8_t { waiting, copying, up_to_date, behind, source_missing, copy_missing, copy_full };
[[nodiscard]] const char* copy_state_word(copy_state s) noexcept;  // "copying", "up_to_date", ...
[[nodiscard]] std::optional<copy_state> parse_copy_state(std::string_view w) noexcept;

struct copy_status {
  std::string label, to_name;
  copy_state state = copy_state::waiting;
  std::uint64_t files_left = 0, bytes_left = 0, failed = 0, held = 0;
  std::uint64_t needed = 0, free = 0;   // copy_full
  std::int64_t complete_at = 0;         // the copy had everything then (0: not yet)
  std::uint64_t bin_items = 0, bin_bytes = 0;
};
[[nodiscard]] std::string copy_statuses_json(std::span<const copy_status> s);
[[nodiscard]] std::vector<copy_status> copy_statuses_from_json(std::string_view text);
[[nodiscard]] std::vector<copy_status> read_copy_statuses(const std::string& host_folder);

// "Copy on “Family Media Copy”: up to date", "… copying, 1,204 files (420 GB) left",
// "… the copy drive is full: needs 1.2 TB, has 500 GB free". `mac`: Trash, not Recycle Bin.
[[nodiscard]] std::string copy_line(const copy_status& s, bool mac);
// state.json's keeper line: "copy        Family Media: up to date" (one per copy).
[[nodiscard]] std::string copies_status_line(std::span<const copy_status> s);

struct copy_keeper_config {
  int every_ms = 15 * 60 * 1000;   // a full compare (the plan's backstop; changes are found by it too)
  int soon_ms = 2 * 60 * 1000;     // again this soon while files are held back or failed
  int config_ms = 20 * 1000;       // copies.json read this often between passes
  bool on_loopback = false;        // the tests: runs with loopback_only too
  bool plain_folders = false;      // the tests: the copy's places are folders, not a volume's root
  std::int64_t settle_seconds = 10;
};
[[nodiscard]] std::unique_ptr<host_keeper> make_copy_keeper(copy_keeper_config c = {});

}  // namespace mv::driftwire::cli
