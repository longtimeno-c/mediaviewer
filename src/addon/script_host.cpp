// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/script_host.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "addon/settings_values.h"
#include "core/json.h"

namespace mv::addon {

namespace {
constexpr std::uint64_t kMaxScriptBytes = 1u << 20;  // open_manifest.cpp's cap
}  // namespace

script_host::script_host(script_services services, script_limits limits)
    : services_(std::move(services)), limits_(limits) {}

script_host::~script_host() { stop_all(); }

void script_host::set_on_change(std::function<void()> cb) { on_change_ = std::move(cb); }

void script_host::changed() const {
  if (on_change_) on_change_();
}

script_host::entry script_host::make_entry(const open_store& store, const open_installed& installed) {
  entry e;
  e.id = installed.id;
  e.commands = installed.m.commands;
  script_config config;
  config.addon_id = installed.id;
  config.api = kContributionApi;
  config.permissions = installed.m.permissions;
  config.limits = limits_;
  for (const std::string& path : installed.m.scripts) {
    auto text = store.verified_file(installed, path, kMaxScriptBytes);
    if (!text) {
      e.failed = "Its files are not the ones its maker signed.";
      return e;
    }
    config.sources.push_back(script_source{path, std::move(*text)});
  }
  // A line an add-on shows names it: the person sees who is talking (docs/design/25
  // §5, the publisher's words are never passed off as MediaViewer's).
  script_services services = services_;
  if (services_.notify) {
    services.notify = [notify = services_.notify, name = installed.m.name](
                          const std::string& id, const std::string& text) {
      notify(id, name + ": " + text);
    };
  }
  if (!installed.m.settings.empty()) {
    // Its own page only, for its own publisher (settings_values.h).
    services.setting = [id = installed.id, publisher = installed.m.publisher_key,
                        page = installed.m.settings](const std::string& key) -> std::optional<std::string> {
      auto values = default_settings_values();
      if (!values) return std::nullopt;
      auto v = values->value_json(id, publisher, page, key);
      if (!v) return std::nullopt;
      return std::move(*v);
    };
  }
  e.addon = script_addon::start(std::move(config), std::move(services));
  // A stop the host asked for (reload, remove, quit) changes nothing the host
  // does not already know; a limit or a load error does.
  e.addon->set_on_stopped([this](script_stop why, const std::string&) {
    if (why != script_stop::stopped) changed();
  });
  return e;
}

void script_host::reload(const open_store& store) {
  stop_all();
  std::vector<entry> fresh;
  for (const open_installed& i : store.list()) {
    if (i.state != install_state::ok || !i.m.runs_code()) continue;
    fresh.push_back(make_entry(store, i));
  }
  {
    std::lock_guard<std::mutex> lock(m_);
    entries_ = std::move(fresh);
  }
  changed();
}

void script_host::restart(const open_store& store, std::string_view addon_id) {
  stop_spin();  // the rig holds an add-on pointer; a restart ends it
  std::unique_ptr<script_addon> old;
  {
    std::lock_guard<std::mutex> lock(m_);
    for (entry& e : entries_) {
      if (e.id == addon_id) old = std::move(e.addon);
    }
  }
  old.reset();  // joins its worker, outside the lock
  auto installed = store.find(addon_id);
  entry fresh;
  if (installed && installed->state == install_state::ok && installed->m.runs_code()) {
    fresh = make_entry(store, *installed);
  }
  {
    std::lock_guard<std::mutex> lock(m_);
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [&](const entry& e) { return e.id == addon_id; });
    if (fresh.id.empty()) {
      if (it != entries_.end()) entries_.erase(it);
    } else if (it != entries_.end()) {
      *it = std::move(fresh);
    } else {
      entries_.push_back(std::move(fresh));
    }
  }
  changed();
}

void script_host::start_spin(std::string addon_id, std::string command_id) {
  stop_spin();
  spin_stop_.store(false);
  spin_ = std::thread([this, addon = std::move(addon_id), command = std::move(command_id)] {
    while (!spin_stop_.load()) {
      script_addon* a = find(addon);
      if (a && a->wait_idle(std::chrono::milliseconds(200)) && !spin_stop_.load() &&
          a->status().running) {
        (void)run_command(addon, command, "null");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(a ? 1 : 100));
    }
  });
}

void script_host::stop_spin() {
  spin_stop_.store(true);
  if (spin_.joinable()) spin_.join();
}

void script_host::stop_all() {
  stop_spin();
  std::vector<entry> old;
  {
    std::lock_guard<std::mutex> lock(m_);
    old.swap(entries_);
  }
  // Every add-on is told first, so their workers stop together; then each
  // destructor joins its worker, outside the lock: a worker's last act may be
  // to call changed(), which reads the table.
  for (entry& e : old) {
    if (e.addon) e.addon->stop();
  }
  old.clear();
}

std::vector<script_command_row> script_host::commands() const {
  std::vector<script_command_row> rows;
  std::lock_guard<std::mutex> lock(m_);
  for (const entry& e : entries_) {
    // Starting counts: a command run before the top level has finished
    // waits in the queue behind it.
    if (!e.addon || e.addon->status().stop != script_stop::none) continue;
    for (const script_command& c : e.commands) {
      rows.push_back(script_command_row{e.id, c.id, c.name, c.key, c.modes, c.payload});
    }
  }
  return rows;
}

bool script_host::run_command(std::string_view addon_id, std::string_view command_id,
                              std::string_view payload_json) {
  std::lock_guard<std::mutex> lock(m_);
  for (entry& e : entries_) {
    if (e.id != addon_id || !e.addon) continue;
    for (const script_command& c : e.commands) {
      if (c.id != command_id) continue;
      json::value payload;
      if (!payload_json.empty()) {
        auto parsed = json::parse(payload_json);
        if (!parsed) return false;
        payload = std::move(*parsed);
      }
      return e.addon->run_function("command " + c.id, c.run, std::move(payload));
    }
  }
  return false;
}

void script_host::set_key_notes(std::string_view notes_json) {
  std::vector<std::pair<std::string, std::string>> notes;
  if (auto doc = json::parse(notes_json); doc && doc->k == json::kind::array) {
    for (const json::value& n : doc->a) {
      const std::string* addon = n.str("addon");
      const std::string* id = n.str("id");
      const std::string* note = n.str("note");
      if (addon && id && note) notes.emplace_back(*addon + "/" + *id, *note);
    }
  }
  std::lock_guard<std::mutex> lock(m_);
  key_notes_ = std::move(notes);
}

std::string script_host::status_json() const {
  json::writer w;
  w.begin_array();
  std::lock_guard<std::mutex> lock(m_);
  for (const entry& e : entries_) {
    w.begin_object();
    w.key("id").string(e.id);
    if (!e.addon) {
      w.key("state").string("stopped");
      w.key("stop").string("load_error");
      w.key("message").string(e.failed);
      w.key("last_error").string("");
      w.key("memory_peak").integer(0);
      w.key("tasks").integer(0);
    } else {
      const script_status s = e.addon->status();
      w.key("state").string(s.running ? "running"
                                      : s.stop == script_stop::none ? "starting" : "stopped");
      w.key("stop").string(script_stop_name(s.stop));
      w.key("message").string(s.message);
      w.key("last_error").string(s.last_error);
      w.key("memory_peak").integer(static_cast<std::int64_t>(s.memory_peak));
      w.key("tasks").integer(static_cast<std::int64_t>(s.tasks));
    }
    w.key("commands").begin_array();
    for (const script_command& c : e.commands) {
      std::string note;
      const std::string k = e.id + "/" + c.id;
      for (const auto& [name, text] : key_notes_) {
        if (name == k) note = text;
      }
      w.begin_object().key("id").string(c.id).key("name").string(c.name).key("key").string(c.key)
          .key("note").string(note).end_object();
    }
    w.end_array();
    w.end_object();
  }
  w.end_array();
  return w.take();
}

script_addon* script_host::find(std::string_view addon_id) const {
  std::lock_guard<std::mutex> lock(m_);
  for (const entry& e : entries_) {
    if (e.id == addon_id) return e.addon.get();
  }
  return nullptr;
}

}  // namespace mv::addon
