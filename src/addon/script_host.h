// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The script add-ons a host runs (docs/design/25 §9 A, PR 57): every installed,
// verified open add-on with code, each a script_addon on its own worker. One
// class for both hosts, so the Windows ABI (abi/script_abi.cpp) and the Mac
// app (shell/addons_mac.mm) read the same rows and say the same words (D9).
//
// Nothing here runs before first pixel: the host calls reload() from a worker
// after its chrome is up. With no script add-on installed it starts no thread
// and reads nothing but the store's listing (docs/design/25 "Absent means absent").
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <string_view>
#include <vector>

#include "addon/open_store.h"
#include "addon/script_runtime.h"

namespace mv::addon {

// A command a running script contributes, as the host's command table takes
// it (shell addon_command_row): `addon` is the dotted open id.
struct script_command_row {
  std::string addon;
  std::string id;
  std::string name;
  std::string key;      // the manifest's label ("Ctrl+Shift+K"); "" none
  std::string modes;    // "viewing" | "video" | "browse" | "all"
  std::string payload;  // "none" | "marks" | "marked_or_current" | "screen"
};

class script_host {
 public:
  explicit script_host(script_services services, script_limits limits = {});
  ~script_host();  // stops every add-on and joins its worker
  script_host(const script_host&) = delete;
  script_host& operator=(const script_host&) = delete;

  // Stops what runs, then starts every installed add-on whose manifest has
  // scripts and which verifies. Reads files: a worker thread.
  void reload(const open_store& store);
  // Starts one add-on again (Settings "Start again"), from the store.
  void restart(const open_store& store, std::string_view addon_id);
  void stop_all();

  // The rows for the command table, from every add-on that is running or
  // starting, in install-list order. Any thread.
  [[nodiscard]] std::vector<script_command_row> commands() const;
  // Runs a command: posts its function to the add-on's worker with the
  // payload (a JSON value, "null" for none). Any thread, never blocks. False
  // when the add-on or command is unknown, stopped, or its queue is full.
  bool run_command(std::string_view addon_id, std::string_view command_id,
                   std::string_view payload_json);

  // What the host's command table did with each row's key, for Settings:
  // [{"addon","id","note"}], "note" saying why a key was dropped ("used by
  // MediaViewer for Rotate left"). Replaces the previous notes.
  void set_key_notes(std::string_view notes_json);

  // For Settings, both hosts:
  //   [{"id","state":"running|stopped|starting","stop":"none|memory|
  //     instructions|time|load_error|stopped","message","last_error",
  //     "memory_peak","tasks",
  //     "commands":[{"id","name","key","note"}]}]
  [[nodiscard]] std::string status_json() const;

  // Called on any thread when an add-on starts or stops, so the host can
  // rebuild its command table and refresh Settings. Set before reload().
  void set_on_change(std::function<void()> cb);

  // The verify rig (MV_SCRIPT_SPIN, docs/design/25 PR 57 "both gates hold while a
  // script spins"): keeps `command` of `addon` running back to back, posting
  // it again each time the add-on's queue empties, until stop_all, reload or
  // restart. Each run
  // stays inside the limits, so the add-on is busy, not stopped. Never used by
  // the app unless the variable is set.
  void start_spin(std::string addon_id, std::string command_id);

  // A running add-on, for later contribution points to post to (PRs 58-59).
  // Valid until the next reload / restart / stop_all.
  [[nodiscard]] script_addon* find(std::string_view addon_id) const;

 private:
  struct entry {
    std::string id;
    std::vector<script_command> commands;
    std::unique_ptr<script_addon> addon;
    std::string failed;  // why it never started (a file did not verify)
  };
  entry make_entry(const open_store& store, const open_installed& installed);
  void changed() const;

  void stop_spin();

  script_services services_;
  script_limits limits_;
  std::thread spin_;
  std::atomic<bool> spin_stop_{false};
  mutable std::mutex m_;
  std::vector<entry> entries_;
  std::vector<std::pair<std::string, std::string>> key_notes_;  // "addon/id" -> note
  std::function<void()> on_change_;
};

}  // namespace mv::addon
