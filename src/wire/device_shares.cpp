// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "wire/device_shares.h"

#include <sodium.h>

#include <algorithm>
#include <span>

#include "core/json.h"
#include "wire/share_users.h"

namespace mv::driftwire::serve::device_shares {
namespace {

std::string hex(std::span<const std::uint8_t> b) {
  std::string out(b.size() * 2, '0');
  sodium_bin2hex(out.data(), out.size() + 1, b.data(), b.size());
  return out;
}

std::optional<key32> unhex(const std::string* s) {
  if (!s || s->size() != 64) return std::nullopt;
  key32 out{};
  std::size_t len = 0;
  if (sodium_hex2bin(out.data(), out.size(), s->data(), s->size(), nullptr, &len, nullptr) != 0 || len != 32) {
    return std::nullopt;
  }
  return out;
}

std::optional<std::vector<std::string>> sorted(const std::optional<std::vector<std::string>>& f) {
  if (!f) return std::nullopt;
  auto v = *f;
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

}  // namespace

desk::~desk() { sodium_memzero(password.data(), password.size()); }

std::string account_for(const key32& key) {
  return "mv-" + hex(std::span<const std::uint8_t>(key.data(), 4));
}

std::string encode(const std::vector<desk>& list) {
  json::writer w;
  w.begin_object().key("v").integer(1).key("desks").begin_array();
  for (const auto& d : list) {
    w.begin_object().key("key").string(hex(d.key)).key("account").string(d.account);
    if (!d.password.empty()) w.key("password").string(d.password);
    if (!d.role.empty()) w.key("role").string(d.role);
    if (d.folders) {
      w.key("folders").begin_array();
      for (const auto& f : *d.folders) w.string(f);
      w.end_array();
    }
    w.key("applied").integer(d.applied);
    if (d.gone) w.key("gone").boolean(true);
    w.end_object();
  }
  w.end_array().end_object();
  return w.take();
}

std::optional<std::vector<desk>> decode(std::string_view text) {
  const auto doc = json::parse(text, 5);
  const json::value* list = doc ? doc->find("desks") : nullptr;
  if (!doc || doc->integer("v") != 1 || !list || list->k != json::kind::array || list->a.size() > kMaxDesks) {
    return std::nullopt;
  }
  std::vector<desk> out;
  for (const auto& e : list->a) {
    desk d;
    const auto key = unhex(e.str("key"));
    if (!key) return std::nullopt;
    d.key = *key;
    d.account = account_for(d.key);
    if (const std::string* a = e.str("account"); !a || *a != d.account) return std::nullopt;
    if (const std::string* p = e.str("password")) d.password = *p;
    if (const std::string* r = e.str("role")) {
      if (*r != "view" && *r != "change") return std::nullopt;
      d.role = *r;
    }
    if (const json::value* f = e.find("folders")) {
      if (f->k != json::kind::array) return std::nullopt;
      std::vector<std::string> labels;
      for (const auto& l : f->a) {
        if (l.k != json::kind::string || !share_users::valid_label(l.s)) return std::nullopt;
        labels.push_back(l.s);
      }
      d.folders = std::move(labels);
    }
    d.applied = e.integer("applied").value_or(0);
    d.gone = e.boolean("gone").value_or(false);
    if (find(out, d.key)) return std::nullopt;
    out.push_back(std::move(d));
  }
  return out;
}

bool load(secrets::store& store, std::vector<desk>& out) {
  out.clear();
  if (!store.exists(kName)) return true;
  auto bytes = store.get(kName);
  if (!bytes) return false;
  auto decoded = decode(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
  sodium_memzero(bytes->data(), bytes->size());
  if (!decoded) return false;
  out = std::move(*decoded);
  return true;
}

bool save(secrets::store& store, const std::vector<desk>& list) {
  if (list.empty()) {
    (void)store.erase(kName);
    return !store.exists(kName);
  }
  std::string text = encode(list);
  const bool ok = store.put(kName, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
  sodium_memzero(text.data(), text.size());
  return ok;
}

bool add(std::vector<desk>& list, const key32& key) {
  for (auto& d : list) {
    if (d.key != key) continue;
    if (!d.gone) return false;
    d.gone = false;  // paired again before its account went: kept
    return true;
  }
  if (list.size() >= kMaxDesks) return false;
  desk d;
  d.key = key;
  d.account = account_for(key);
  // Another computer's key starts with the same bytes: never two computers on one account (one
  // would get the other's rights, and unpairing either would take both logins away). This one
  // has no account of its own; it still opens the folders in the app.
  for (const auto& other : list) {
    if (other.account == d.account) return false;
  }
  list.push_back(std::move(d));
  return true;
}

bool remove(std::vector<desk>& list, const key32& key) {
  for (auto it = list.begin(); it != list.end(); ++it) {
    if (it->key != key) continue;
    if (it->applied == 0) {
      list.erase(it);
      return true;
    }
    if (it->gone) return false;
    it->gone = true;
    return true;
  }
  return false;
}

const desk* find(const std::vector<desk>& list, const key32& key) {
  for (const auto& d : list) {
    if (d.key == key) return &d;
  }
  return nullptr;
}

bool waiting(const desk& d, std::string_view role, const std::optional<std::vector<std::string>>& folders) {
  if (d.gone || d.applied == 0 || d.password.empty()) return true;
  return d.role != role || sorted(d.folders) != sorted(folders);
}

want want_of(const grants::grant* limited, const std::vector<root_config>& roots) {
  want w;
  w.role = (!limited || limited->backup) ? "change" : "view";
  if (limited && limited->roots) {
    std::vector<std::string> labels;
    for (const auto& r : roots) {
      if (std::find(limited->roots->begin(), limited->roots->end(), r.id) != limited->roots->end()) labels.push_back(r.label);
    }
    w.folders = std::move(labels);
  }
  return w;
}

std::string new_password() {
  std::array<std::uint8_t, kPasswordBytes> raw{};
  randombytes_buf(raw.data(), raw.size());
  std::string out(sodium_base64_ENCODED_LEN(raw.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING), '\0');
  sodium_bin2base64(out.data(), out.size(), raw.data(), raw.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
  out.resize(out.size() - 1);  // the terminator
  sodium_memzero(raw.data(), raw.size());
  return out;
}

}  // namespace mv::driftwire::serve::device_shares
