// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The script runtime for add-ons from other makers (docs/design/25 §9 A, PR 57):
// Lua 5.4 (third_party/lua), one sandboxed state per add-on, on a worker of
// its own.
//
//  - **Its own worker.** Every Lua call runs on the add-on's thread, never on
//    the UI, render or decode threads. Anything that wants the script posts a
//    task (post(), any thread, never blocks); host calls that answer later post
//    their answer the same way.
//  - **Limits.** A memory ceiling through the allocator, an instruction budget
//    per task (a count hook) and a wall-clock bound per task (the same hook,
//    plus one watchdog thread for the few library loops that run no VM
//    instruction). Over a limit the add-on is STOPPED: its state is closed on
//    its worker, nothing of it runs again until the next start, and the host
//    is told which limit and why. The viewer carries on.
//  - **Nothing it was not given.** The environment holds the safe parts of
//    base, string, table, math, utf8 and coroutine. No io, os, package, debug,
//    require, dofile, loadfile, string.dump or binary chunks; `load` takes text
//    only, into the sandbox. The `mv` table holds what the add-on's
//    permissions grant: an API whose permission was not granted is absent.
//    There is no network call and no file call to grant.
//
// Bindings (the `mv.*` tables) are registered once per process
// (add_script_binding) and installed into each state whose API and
// permissions allow them, so later contribution points (screens, slots,
// search providers: PRs 58-59) add theirs without touching this file.
//
// Portable (D9). Rule 6: nothing here sends anything anywhere.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.h"

struct lua_State;

namespace mv::addon {

// docs/design/12 2026-10-11: the values PR 57 ships.
struct script_limits {
  std::size_t memory_bytes = 64u << 20;            // the state's every allocation, together
  std::uint64_t instructions = 100'000'000;        // VM instructions per task
  std::chrono::milliseconds wall{1000};            // per task
  std::uint32_t hook_every = 10'000;               // instructions between checks
  std::size_t stack_bytes = 4u << 20;              // the worker's C stack
  std::size_t queue = 256;                         // tasks waiting; a full queue drops
};

enum class script_stop : std::uint8_t {
  none = 0,
  memory,        // asked for more than the ceiling
  instructions,  // one task ran past its instruction budget
  time,          // one task ran past its wall-clock bound
  load_error,    // a script did not compile, or its top level raised
  stopped,       // the host stopped it (removed, quit, reloaded)
};
[[nodiscard]] const char* script_stop_name(script_stop s) noexcept;

// What the host lends a script. Every function may be called on the add-on's
// worker and must return at once (a snapshot read, never a wait on the UI
// thread). Empty functions read as "nothing open".
struct script_services {
  // folder.read: the open folder as the viewer lists it.
  std::function<std::uint32_t()> folder_count;
  std::function<std::string()> folder_path;
  std::function<std::string(std::uint32_t index)> folder_item_name;  // 0-based; "" past the end
  // selection: the file on screen ("" when none).
  std::function<std::string()> current_path;
  // A short line to show the person, from mv.notify. Any thread; the host
  // marshals it to its UI.
  std::function<void(const std::string& addon_id, const std::string& text)> notify;
  // mv.log / print, and the runtime's own lines about the add-on. Stays on
  // this machine (rule 6).
  std::function<void(const std::string& addon_id, int level, const std::string& text)> log;
  // mv.settings.get: this add-on's value for a key on its own settings page,
  // as JSON text (PR 56's store, settings_values::value_json); nullopt for a
  // key its page does not have. Empty: the add-on has no page.
  std::function<std::optional<std::string>(const std::string& key)> setting;
};

struct script_source {
  std::string name;  // "main.lua": the chunk name errors carry
  std::string text;
};

struct script_config {
  std::string addon_id;
  std::uint32_t api = 3;          // the contribution API the state is built for
  std::uint32_t permissions = 0;  // open_manifest.h permission bits
  std::vector<script_source> sources;
  script_limits limits;
};

class script_addon;

// A task's view of the state, on the worker. Every call is protected: a Lua
// error is caught and reported, never thrown at the caller.
class script_call {
 public:
  [[nodiscard]] lua_State* state() const noexcept { return L_; }
  [[nodiscard]] script_addon& addon() const noexcept { return *addon_; }
  // Calls the script's global function `name` with `args` (JSON values,
  // converted to Lua). False, with `error()` set, when it is missing or
  // raised. A stop (a limit) also returns false; the runtime then closes the
  // state after the task.
  bool call_global(std::string_view name, const std::vector<json::value>& args = {});
  // Calls every function the script registered with mv.on(event, fn).
  bool call_handlers(std::string_view event, const std::vector<json::value>& args = {});
  // Runs `fn` with the state in protected mode: the raw Lua API is safe
  // inside it (an error is caught and lands in error()). For a task that reads
  // what the script left in its globals (a slot's table, a screen's tree).
  bool with_state(const std::function<void(lua_State*)>& fn);
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  friend class script_addon;
  script_call(lua_State* L, script_addon* a) : L_(L), addon_(a) {}
  lua_State* L_;
  script_addon* addon_;
  std::string error_;
};

struct script_task {
  std::string what;              // names the work in a stop message: "command count-files"
  std::uint64_t generation = 0;  // the view intent it answers
  // True when `generation` no longer matters: the task is dropped unrun.
  std::function<bool(std::uint64_t)> stale;
  std::function<void(script_call&)> run;
};

struct script_status {
  bool running = false;  // loaded and taking tasks
  script_stop stop = script_stop::none;
  std::string message;     // why it stopped, in words
  std::string last_error;  // the last script error (it did not stop)
  std::uint64_t tasks = 0;
  std::size_t memory_peak = 0;
};

class script_addon {
 public:
  // Builds the state and runs the sources' top level on a new worker; returns
  // at once. A load error stops the add-on (script_stop::load_error).
  static std::unique_ptr<script_addon> start(script_config config, script_services services);
  ~script_addon();  // stops it (stopped) and joins the worker
  script_addon(const script_addon&) = delete;
  script_addon& operator=(const script_addon&) = delete;

  // Any thread, never blocks. False when stopped or the queue is full.
  bool post(script_task task);
  // Posts a task that calls the global `function_name` with `payload`.
  bool run_function(std::string what, std::string function_name, json::value payload);

  // Called once, on the worker, when the add-on stops for any reason; at once,
  // on the caller's thread, if it has stopped already.
  void set_on_stopped(std::function<void(script_stop, const std::string& message)> cb);

  [[nodiscard]] const std::string& id() const noexcept { return config_.addon_id; }
  [[nodiscard]] std::uint32_t permissions() const noexcept { return config_.permissions; }
  [[nodiscard]] const script_services& services() const noexcept { return services_; }
  [[nodiscard]] script_status status() const;
  // Tests and the verify rig: waits until the queue is empty and no task runs,
  // or the add-on stopped. False on timeout.
  bool wait_idle(std::chrono::milliseconds timeout) const;

  struct impl;

 private:
  script_addon() = default;
  script_config config_;
  script_services services_;
  std::unique_ptr<impl> impl_;
};

// ---- bindings ---------------------------------------------------------------

// A part of the `mv` table. `install` runs on the worker while the state is
// built, with the `mv` table on top of the stack, and sets fields on it.
// Installed only when the state's API is at least `api` and every bit of
// `permission` was granted (0: always).
struct script_binding {
  const char* name;
  std::uint32_t api;
  std::uint32_t permission;
  void (*install)(lua_State* L, script_addon& addon);
};
// Registers a binding for every state built after it. Call at start-up,
// before any add-on starts; the runtime's own (mv.log, mv.notify, mv.on,
// mv.folder, mv.selection) are registered already.
void add_script_binding(const script_binding& binding);

// Lua <-> JSON for bindings. to_json refuses cycles, functions, userdata,
// depth over 32 and output over `max_bytes`: false, with the reason in `why`.
bool lua_to_json(lua_State* L, int index, std::string& out, std::size_t max_bytes,
                 std::string& why);
void push_json(lua_State* L, const json::value& v);

// The add-on that owns a state (from the state's extra space).
[[nodiscard]] script_addon* script_owner(lua_State* L) noexcept;

}  // namespace mv::addon
