// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The host's section of the menu-bar / notification-area menu while
// `driftwire host` runs on this computer (docs/plans/self-hosted.md "Tray",
// issue #686 §4, owner 2026-11-14). Not a second app or process: the app's own
// background presence (#452, docs/design/28 "In the background") draws it, as
// one more kept entry of the add-on registry (slots.h set_shell_keep).
//
// The app hosts nothing: it reads the files the running host keeps for
// `host status` (host.lock held, state.json, host.json's DuckDNS name), as the
// This computer card's status row does (self_host.h), and acts through the
// program's own verbs (`host pause`, `host resume`, `host quit`).
//
// Two parts: the pure model (state -> rows, every OS's tests), and a watcher
// with one thread of its own that looks at the files (rule 1: never the UI
// thread) and runs the verbs. It looks no more often than the host rewrites
// state.json (every 2 s at most, only on a change): a stat of state.json and
// the lock every kPollMs, a read only when the stat changed. It presents
// nothing and wakes no UI on an unchanged model (idle cost). Nothing here logs
// or shows a path, an address or a port (rule 6); phones by the names they
// were paired under.
#pragma once

#include <cstdint>
#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "addons/phone/self_host.h"

namespace mv::phone::host_tray {

inline constexpr int kPollMs = 5000;          // the files looked at (the host writes at most every 2 s)
inline constexpr int kLoginEveryMs = 60000;   // the start-at-login entry looked at
inline constexpr std::size_t kMaxRows = 16;   // slots.h keep_menu's limit
inline constexpr std::size_t kMaxNames = 4;   // phones named, then "and N more"
inline constexpr const char* kKeepId = "mediaviewer.host";  // never an add-on's id (they have no dot)
inline constexpr const char* kScreen = "phone_access";      // Settings → Add-ons → Phone access

// What the files said.
struct input {
  bool running = false;    // host.lock is held
  bool at_login = false;   // the start-at-login entry is there
  std::optional<self_host::running_state> state;  // state.json, while running
  std::string duckdns;     // host.json's subdomain, "" for none
  // A connection's "since" as the person reads it; empty: the local "14:02". The tests' own.
  std::function<std::string(std::int64_t unix_s)> clock;
};

enum class status : std::uint8_t { serving, paused, problem, nothing_shared };

struct row {
  std::string kind;  // "text", "toggle", "button", "separator" (slots.h menu_row)
  std::string id;    // the toggle's "hosting", the button's "quit"
  std::string label;
  bool value = false;  // a toggle's: on
  bool operator==(const row&) const = default;
};

struct model {
  bool shown = false;  // the host runs: the entry is drawn and keeps the process
  bool login = false;  // the app's login item is wanted (the host starts at login)
  status what = status::nothing_shared;
  std::string label;   // the icon's tooltip and the menu's first line
  std::vector<row> rows;
  // Not compared: the clock is the input's.
  bool operator==(const model& o) const {
    return shown == o.shown && login == o.login && what == o.what && label == o.label && rows == o.rows;
  }
};

// "1.2 GB", "340 MB", "12 KB", "0 B" (decimal, as the OS's file sizes).
[[nodiscard]] std::string bytes_words(std::uint64_t n);
// The usage line ("Today: 3.4 GB read, 120 MB written · reading 12 MB/s") and
// a line per phone connected ("Ann's iPhone · home network since 14:02 ·
// 1.2 GB read · reading"), as the menu, the This computer card and
// `host status` say them. `clock` empty: the local "14:02".
[[nodiscard]] std::vector<std::string> usage_lines(const self_host::running_state& s,
                                                   const std::function<std::string(std::int64_t)>& clock = {});

// Pure: the menu for what the files said.
[[nodiscard]] model build(const input& in);
// {"items":[..]} (slots.h keep_menu's shape), for the tests and a host that wants it.
[[nodiscard]] std::string menu_json(const model& m);

// ---- the watcher --------------------------------------------------------------------------

struct options {
  // Where the host's files are. Empty: self_host::find() on the watcher's thread.
  std::function<self_host::where()> where;
  int poll_ms = kPollMs;
  int login_every_ms = kLoginEveryMs;
  int idle_poll_ms = 30000;   // with no host folder at all
  int gone_grace_ms = 90000;  // the entry kept after the host lets go of its lock (not after Quit hosting)
  // Runs a verb ("pause", "resume", "quit"); empty: self_host::run_host. The tests' fake.
  std::function<bool(const self_host::where&, const std::string& verb)> run;
  // A new model (on the watcher's thread; it must not block). Called once at
  // the first look, then only when the model changed.
  std::function<void(const model&)> changed;
};

class watcher {
 public:
  [[nodiscard]] static std::unique_ptr<watcher> start(options o);
  ~watcher();
  watcher(const watcher&) = delete;
  watcher& operator=(const watcher&) = delete;

  // A row was picked: the Hosting toggle with its new value ("true" resumes,
  // "false" pauses), "quit", or a verb by name ("pause", "resume"). Run on the
  // watcher's thread, then the files are looked at again. Any thread, never blocks.
  void pick(const std::string& id, const std::string& value_json = "null");
  // Look now (the tests; a menu opened). Any thread, never blocks.
  void look_now();
  [[nodiscard]] model current() const;
  // Ends the thread and waits for it (a verb it is running: up to that verb's
  // 30 s). `wait` false at the app's quit: it is told and not waited for, and
  // the caller keeps the watcher alive (never destroyed) for its thread.
  void stop(bool wait = true) noexcept;

 private:
  watcher() = default;
  struct state;
  std::unique_ptr<state> st_;
};

}  // namespace mv::phone::host_tray
