// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/settings_values.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

#include "addon/open_manifest.h"
#include "addon/open_store.h"
#include "io/file_port.h"
#include "io/paths.h"
#include "io/replace.h"

namespace mv::addon {
namespace {

constexpr std::uint64_t kValuesMaxBytes = 256u << 10;

// The values each file holds, as last read or written: one entry per file
// path. A settings file is small and written only from Settings, so it is
// cheap to keep, and an add-on reading on every request never touches disk.
struct values_cache {
  std::mutex m;
  std::map<std::string, json::value> by_path;  // each an object
};
values_cache& cache() {
  static values_cache* const c = new values_cache;  // never freed: workers may outlive statics
  return *c;
}

json::value read_values(const std::string& path) {
  json::value out;
  out.k = json::kind::object;
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
  return *values;
}

// Under cache().m.
json::value& values_of(const std::string& path) {
  auto& by = cache().by_path;
  auto it = by.find(path);
  if (it == by.end()) it = by.emplace(path, read_values(path)).first;
  return it->second;
}

const json::value* stored(const json::value& values, std::string_view key) {
  return values.find(key);
}

void write_item(json::writer& w, const setting_item& it, const json::value& values) {
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

std::string settings_values::page_json(std::string_view id, const settings_page& page,
                                       std::string_view fallback_title) const {
  json::value values;
  if (valid_settings_owner(id)) {
    std::lock_guard lock(cache().m);
    values = values_of(path_of(id));
  }
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
    for (const setting_item& it : s.items) write_item(w, it, values);
    w.end_array();
    w.end_object();
  }
  w.end_array();
  w.end_object();
  return w.take();
}

result<std::string> settings_values::value_json(std::string_view id, const settings_page& page,
                                                std::string_view key) const {
  const setting_item* it = page.find(key);
  if (!it || !valid_settings_owner(id)) return err(status::invalid_arg);
  std::lock_guard lock(cache().m);
  const json::value* v = stored(values_of(path_of(id)), key);
  return v && setting_value_fits(*it, *v) ? setting_value_json(*v) : setting_default_json(*it);
}

expected settings_values::set(std::string_view id, const settings_page& page, std::string_view key,
                              std::string_view value_text) const {
  const setting_item* it = page.find(key);
  if (!it || !valid_settings_owner(id) || value_text.size() > 4 * kSettingsMaxText) {
    return err(status::invalid_arg);
  }
  const auto value = json::parse(value_text, 1);
  if (!value || !setting_value_fits(*it, *value)) return err(status::invalid_arg);

  const std::string path = path_of(id);
  std::lock_guard lock(cache().m);
  json::value next = values_of(path);
  // Keys the page no longer has stay: an older version of the add-on, put
  // back, finds what was chosen for it.
  bool replaced = false;
  for (auto& [name, v] : next.o) {
    if (name == key) {
      v = *value;
      replaced = true;
    }
  }
  if (!replaced) next.o.emplace_back(std::string(key), *value);

  json::writer w;
  w.begin_object();
  w.key("schema").integer(1);
  w.key("values").begin_object();
  for (const auto& [name, v] : next.o) w.key(name).raw(setting_value_json(v));
  w.end_object();
  w.end_object();
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
  return {};
}

expected settings_values::forget(std::string_view id) const {
  if (!valid_settings_owner(id)) return err(status::invalid_arg);
  const std::string path = path_of(id);
  std::lock_guard lock(cache().m);
  cache().by_path.erase(path);
  if (!io::stat_path(path)) return {};
  return io::remove_file(path);
}

bool settings_values::has_values(std::string_view id) const {
  if (!valid_settings_owner(id)) return false;
  std::lock_guard lock(cache().m);
  return !values_of(path_of(id)).o.empty();
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
  return values.value_json(addon_id, installed->m.settings, key);
}

void reset_settings_cache_for_tests() {
  std::lock_guard lock(cache().m);
  cache().by_path.clear();
}

}  // namespace mv::addon
