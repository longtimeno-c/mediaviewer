// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/settings_values.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "addon/open_manifest.h"
#include "addon/open_store.h"
#include "io/file_port.h"
#include "io/paths.h"
#include "io/replace.h"

namespace mv::addon {
namespace {

constexpr std::uint64_t kValuesMaxBytes = 256u << 10;

// One settings file: whose add-on the values were chosen for, and the values.
struct values_file {
  // The publisher's key: an open add-on's Ed25519 key (64 hex), "" for a
  // first-party add-on (the app's own).
  std::string publisher;
  json::value values;  // an object
};

// The files, as last read or written: one entry per file path. A settings
// file is small and written only from Settings, so it is cheap to keep, and
// an add-on reading on every request never touches disk.
struct values_cache {
  std::mutex m;
  std::map<std::string, values_file> by_path;
};
values_cache& cache() {
  static values_cache* const c = new values_cache;  // never freed: workers may outlive statics
  return *c;
}

values_file read_file(const std::string& path) {
  values_file out;
  out.values.k = json::kind::object;
  auto st = io::stat_path(path);
  if (!st || st->is_directory || st->size == 0 || st->size > kValuesMaxBytes) return out;
  io::file_reader in;
  if (!in.open(path, io::read_mode::sequential)) return out;
  std::string text(static_cast<std::size_t>(st->size), '\0');
  std::size_t have = 0;
  while (have < text.size()) {
    auto got = in.read(std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(text.data()) + have,
                                               text.size() - have));
    if (!got || *got == 0) break;
    have += *got;
  }
  text.resize(have);
  const auto doc = json::parse(text, 4);
  if (!doc || doc->k != json::kind::object) return out;
  const json::value* values = doc->find("values");
  if (!values || values->k != json::kind::object) return out;
  if (const std::string* publisher = doc->str("publisher")) out.publisher = *publisher;
  out.values = *values;
  return out;
}

// Under cache().m.
values_file& file_of(const std::string& path) {
  auto& by = cache().by_path;
  auto it = by.find(path);
  if (it == by.end()) it = by.emplace(path, read_file(path)).first;
  return it->second;
}

// Under cache().m. What the file holds now, into the cache: another window is
// another process with its own cache, and may have written since (Settings
// opening, and every write, start from the disk so neither undoes the other).
values_file& fresh_file_of(const std::string& path) {
  values_file& f = cache().by_path[path];
  f = read_file(path);
  return f;
}

// The values `publisher`'s add-on may see: none when they were chosen for
// another publisher's add-on under the same id (one removed keeping its
// settings, then a stranger's package installed under that id). What a
// person chose for one publisher is never handed to another.
// nullptr: nothing to see.
const json::value* values_for(const values_file& f, std::string_view publisher) {
  return f.publisher == publisher ? &f.values : nullptr;
}

const json::value* stored(const json::value* values, std::string_view key) {
  return values ? values->find(key) : nullptr;
}

// The settings listeners (add_settings_listener). Heap-held and never freed:
// a registry may unregister from a static destructor.
struct listener_list {
  std::mutex m;
  std::uint64_t next = 0;
  std::vector<std::pair<std::uint64_t, settings_listener>> fns;
};
listener_list& listeners() {
  static listener_list* const l = new listener_list;
  return *l;
}

// After a write, outside cache().m: everyone that serves `id` hears of it.
void changed(std::string_view id) noexcept {
  listener_list& l = listeners();
  std::lock_guard lock(l.m);
  for (const auto& [token, fn] : l.fns) {
    (void)token;
    try {
      fn(id);
    } catch (...) {
      // A listener that failed to queue its refresh leaves the old answer on
      // screen until the next request; the write itself stands.
    }
  }
}

void write_values_file(json::writer& w, std::string_view publisher, const json::value& values) {
  w.begin_object();
  w.key("schema").integer(1);
  w.key("publisher").string(publisher);
  w.key("values").begin_object();
  for (const auto& [name, v] : values.o) w.key(name).raw(setting_value_json(v));
  w.end_object();
  w.end_object();
}

void write_item(json::writer& w, const setting_item& it, const json::value* values) {
  w.begin_object();
  w.key("type").string(setting_kind_name(it.kind));
  w.key("key").string(it.key);
  w.key("label").string(it.label);
  w.key("help").string(it.help);
  w.key("enabled_by").string(it.enabled_by);
  w.key("default").raw(setting_default_json(it));
  const json::value* v = stored(values, it.key);
  w.key("value").raw(v && setting_value_fits(it, *v) ? setting_value_json(*v)
                                                     : setting_default_json(it));
  switch (it.kind) {
    case setting_kind::number: {
      json::value n;
      n.k = json::kind::number;
      n.d = it.min;
      w.key("min").raw(setting_value_json(n));
      n.d = it.max;
      w.key("max").raw(setting_value_json(n));
      n.d = it.step;
      w.key("step").raw(setting_value_json(n));
      w.key("unit").string(it.unit);
      break;
    }
    case setting_kind::choice:
      w.key("options").begin_array();
      for (const setting_option& o : it.options) {
        w.begin_object().key("value").string(o.value).key("label").string(o.label).end_object();
      }
      w.end_array();
      break;
    case setting_kind::text:
      w.key("max_length").integer(it.max_length);
      w.key("placeholder").string(it.placeholder);
      break;
    case setting_kind::toggle:
      break;
  }
  w.end_object();
}

}  // namespace

bool valid_settings_owner(std::string_view id) noexcept {
  if (valid_open_id(id)) return true;
  if (id.empty() || id.size() > 40 || id.front() == '-' || id.back() == '-') return false;
  if (id == "con" || id == "prn" || id == "aux" || id == "nul") return false;
  if (id.size() == 4 && (id.substr(0, 3) == "com" || id.substr(0, 3) == "lpt") && id[3] >= '0' &&
      id[3] <= '9') {
    return false;
  }
  return std::all_of(id.begin(), id.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
  });
}

settings_values::settings_values(std::string root) : root_(std::move(root)) {}

std::string settings_values::path_of(std::string_view id) const {
  return io::join_path(root_, std::string(id) + ".json");
}

std::string settings_values::page_json(std::string_view id, std::string_view publisher,
                                       const settings_page& page,
                                       std::string_view fallback_title) const {
  json::value values;
  bool have = false;
  if (valid_settings_owner(id)) {
    std::lock_guard lock(cache().m);
    if (const json::value* v = values_for(fresh_file_of(path_of(id)), publisher)) {
      values = *v;
      have = true;
    }
  }
  const json::value* shown = have ? &values : nullptr;
  json::writer w;
  w.begin_object();
  w.key("id").string(id);
  w.key("title").string(page.title.empty() ? fallback_title : std::string_view(page.title));
  w.key("sections").begin_array();
  for (const setting_section& s : page.sections) {
    w.begin_object();
    w.key("title").string(s.title);
    w.key("help").string(s.help);
    w.key("items").begin_array();
    for (const setting_item& it : s.items) write_item(w, it, shown);
    w.end_array();
    w.end_object();
  }
  w.end_array();
  w.end_object();
  return w.take();
}

result<std::string> settings_values::value_json(std::string_view id, std::string_view publisher,
                                                const settings_page& page,
                                                std::string_view key) const {
  const setting_item* it = page.find(key);
  if (!it || !valid_settings_owner(id)) return err(status::invalid_arg);
  std::lock_guard lock(cache().m);
  const json::value* v = stored(values_for(file_of(path_of(id)), publisher), key);
  return v && setting_value_fits(*it, *v) ? setting_value_json(*v) : setting_default_json(*it);
}

expected settings_values::set(std::string_view id, std::string_view publisher,
                              const settings_page& page, std::string_view key,
                              std::string_view value_text) const {
  const setting_item* it = page.find(key);
  if (!it || !valid_settings_owner(id) || value_text.size() > 4 * kSettingsMaxText) {
    return err(status::invalid_arg);
  }
  const auto value = json::parse(value_text, 1);
  if (!value || !setting_value_fits(*it, *value)) return err(status::invalid_arg);

  const std::string path = path_of(id);
  bool same = false;  // the file already held exactly this value for this publisher
  {
    std::lock_guard lock(cache().m);
    values_file next = fresh_file_of(path);
    if (next.publisher != publisher) {
      // Chosen for another publisher's add-on under this id: not merged into,
      // replaced.
      next.publisher = std::string(publisher);
      next.values = json::value{};
      next.values.k = json::kind::object;
    }
    // Keys the page no longer has stay: an older version of the add-on, put
    // back, finds what was chosen for it.
    bool replaced = false;
    for (auto& [name, v] : next.values.o) {
      if (name == key) {
        same = setting_value_json(v) == setting_value_json(*value);
        v = *value;
        replaced = true;
      }
    }
    if (!replaced) next.values.o.emplace_back(std::string(key), *value);

    json::writer w;
    write_values_file(w, next.publisher, next.values);
    const std::string& text = w.str();
    const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(text.data()),
                                              text.size());
    MV_TRY_VOID(io::make_directories(root_));
    if (io::stat_path(path)) {
      MV_TRY_VOID(io::replace_atomic(path, bytes));
    } else {
      MV_TRY_VOID(io::write_new_atomic(path, bytes));
    }
    cache().by_path[path] = std::move(next);
  }
  if (!same) changed(id);
  return {};
}

expected settings_values::forget(std::string_view id) const {
  if (!valid_settings_owner(id)) return err(status::invalid_arg);
  const std::string path = path_of(id);
  {
    std::lock_guard lock(cache().m);
    cache().by_path.erase(path);
    if (!io::stat_path(path)) return {};
    MV_TRY_VOID(io::remove_file(path));
  }
  changed(id);  // back to the page's defaults
  return {};
}

bool settings_values::has_values(std::string_view id) const {
  if (!valid_settings_owner(id)) return false;
  std::lock_guard lock(cache().m);
  return !file_of(path_of(id)).values.o.empty();
}

expected settings_values::claim(std::string_view id, std::string_view publisher) const {
  if (!valid_settings_owner(id)) return err(status::invalid_arg);
  const std::string path = path_of(id);
  {
    std::lock_guard lock(cache().m);
    const values_file& f = fresh_file_of(path);
    if (f.values.o.empty() || f.publisher == publisher) return {};
    cache().by_path.erase(path);
    if (!io::stat_path(path)) return {};
    MV_TRY_VOID(io::remove_file(path));
  }
  changed(id);
  return {};
}

result<settings_values> default_settings_values() {
  MV_TRY(std::string root, io::addon_settings_dir());
  return settings_values(std::move(root));
}

result<std::string> open_setting_value_json(const open_store& store, std::string_view addon_id,
                                            std::string_view key) {
  auto installed = store.find(addon_id);
  if (!installed || installed->state != install_state::ok) return err(status::invalid_arg);
  MV_TRY(settings_values values, default_settings_values());
  return values.value_json(addon_id, installed->m.publisher_key, installed->m.settings, key);
}

std::uint64_t add_settings_listener(settings_listener fn) {
  listener_list& l = listeners();
  std::lock_guard lock(l.m);
  const std::uint64_t token = ++l.next;
  l.fns.emplace_back(token, std::move(fn));
  return token;
}

void remove_settings_listener(std::uint64_t token) {
  listener_list& l = listeners();
  std::lock_guard lock(l.m);
  std::erase_if(l.fns, [token](const auto& e) { return e.first == token; });
}

void reset_settings_cache_for_tests() {
  std::lock_guard lock(cache().m);
  cache().by_path.clear();
}

}  // namespace mv::addon
