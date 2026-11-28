// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "wire/people.h"

#include <sodium.h>

#include <algorithm>
#include <span>

#include "core/json.h"

namespace mv::driftwire::serve::people {
namespace {

std::string hex(const key32& k) {
  std::string out(64, '0');
  sodium_bin2hex(out.data(), out.size() + 1, k.data(), k.size());
  return out;
}

std::optional<key32> unhex(std::string_view s) {
  if (s.size() != 64) return std::nullopt;
  key32 out{};
  std::size_t len = 0;
  if (sodium_hex2bin(out.data(), out.size(), s.data(), s.size(), nullptr, &len, nullptr) != 0 || len != 32) {
    return std::nullopt;
  }
  return out;
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool same_name(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

}  // namespace

std::optional<std::string> clean_name(std::string_view name) {
  while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
  while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
  if (name.empty() || name.size() > kMaxNameBytes) return std::nullopt;
  for (char c : name) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f) return std::nullopt;
  }
  return std::string(name);
}

bool valid_role(std::string_view role) noexcept { return role == "view" || role == "change"; }

std::string encode(const std::vector<person>& list) {
  json::writer w;
  w.begin_object().key("v").integer(1).key("people").begin_array();
  for (const auto& p : list) {
    w.begin_object().key("name").string(p.name).key("role").string(p.role).key("devices").begin_array();
    for (const auto& d : p.devices) w.string(hex(d));
    w.end_array().end_object();
  }
  w.end_array().end_object();
  return w.take();
}

std::optional<std::vector<person>> decode(std::string_view text) {
  const auto doc = json::parse(text, 5);
  const json::value* list = doc ? doc->find("people") : nullptr;
  if (!doc || doc->integer("v") != 1 || !list || list->k != json::kind::array || list->a.size() > kMaxPeople) {
    return std::nullopt;
  }
  std::vector<person> out;
  for (const auto& e : list->a) {
    person p;
    const std::string* name = e.str("name");
    const std::string* role = e.str("role");
    const json::value* devices = e.find("devices");
    if (!name || !role || !valid_role(*role) || !devices || devices->k != json::kind::array ||
        devices->a.size() > kMaxDevices) {
      return std::nullopt;
    }
    auto clean = clean_name(*name);
    if (!clean || *clean != *name) return std::nullopt;
    for (const auto& q : out) {
      if (same_name(q.name, *name)) return std::nullopt;
    }
    p.name = *name;
    p.role = *role;
    for (const auto& d : devices->a) {
      const auto k = d.k == json::kind::string ? unhex(d.s) : std::nullopt;
      if (!k || owner_of(out, *k) || std::find(p.devices.begin(), p.devices.end(), *k) != p.devices.end()) {
        return std::nullopt;
      }
      p.devices.push_back(*k);
    }
    out.push_back(std::move(p));
  }
  return out;
}

bool load(secrets::store& store, std::vector<person>& out) {
  out.clear();
  if (!store.exists(kName)) return true;
  const auto bytes = store.get(kName);
  if (!bytes) return false;
  auto decoded = decode(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
  if (!decoded) return false;
  out = std::move(*decoded);
  return true;
}

bool save(secrets::store& store, const std::vector<person>& list) {
  if (list.empty()) {
    (void)store.erase(kName);
    return !store.exists(kName);
  }
  const std::string text = encode(list);
  return store.put(kName, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

const person* owner_of(const std::vector<person>& list, const key32& device) {
  for (const auto& p : list) {
    if (std::find(p.devices.begin(), p.devices.end(), device) != p.devices.end()) return &p;
  }
  return nullptr;
}

person* find(std::vector<person>& list, std::string_view name) {
  for (auto& p : list) {
    if (same_name(p.name, name)) return &p;
  }
  return nullptr;
}

std::string set(std::vector<person>& list, std::string_view name, std::string_view role) {
  const auto clean = clean_name(name);
  if (!clean) return "bad-name";
  if (!valid_role(role)) return "bad-role";
  if (person* p = find(list, *clean)) {
    p->role = std::string(role);
    return {};
  }
  if (list.size() >= kMaxPeople) return "too-many";
  person p;
  p.name = *clean;
  p.role = std::string(role);
  list.push_back(std::move(p));
  return {};
}

bool remove(std::vector<person>& list, std::string_view name) {
  const auto before = list.size();
  std::erase_if(list, [&](const person& p) { return same_name(p.name, name); });
  return list.size() != before;
}

std::string link(std::vector<person>& list, const key32& device, std::string_view name) {
  person* to = nullptr;
  if (!name.empty()) {
    to = find(list, name);
    if (!to) return "not-found";
    if (std::find(to->devices.begin(), to->devices.end(), device) != to->devices.end()) return {};
    if (to->devices.size() >= kMaxDevices) return "too-many";
  }
  (void)forget_device(list, device);
  if (to) to->devices.push_back(device);
  return {};
}

bool forget_device(std::vector<person>& list, const key32& device) {
  bool any = false;
  for (auto& p : list) {
    const auto before = p.devices.size();
    std::erase(p.devices, device);
    any = any || p.devices.size() != before;
  }
  return any;
}

}  // namespace mv::driftwire::serve::people
