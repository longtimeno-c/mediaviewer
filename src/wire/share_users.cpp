// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "wire/share_users.h"

#include <algorithm>
#include <array>
#include <span>

#include "core/json.h"

namespace mv::driftwire::serve::share_users {

namespace {

// Names an OS keeps for itself, on any of the three, refused whatever the case.
constexpr std::array<std::string_view, 22> kReserved = {
    "admin",  "administrator", "administrators", "guest", "guests",  "root",    "nobody",    "daemon",
    "everyone", "system",      "users",          "user",  "wheel",   "staff",   "operator",  "defaultaccount",
    "wdagutilityaccount", "krbtgt", "network", "service", "smb", "mvshare"};

bool lower_alnum(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

std::string lowered(std::string_view s) {
  std::string out(s);
  for (auto& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

bool read_folders(const json::value& e, std::optional<std::vector<std::string>>& out) {
  out.reset();
  const json::value* f = e.find("folders");
  if (!f) return true;
  if (f->k != json::kind::array || f->a.empty() || f->a.size() > kMaxFolders) return false;
  std::vector<std::string> labels;
  for (const auto& x : f->a) {
    if (x.k != json::kind::string || !valid_label(x.s)) return false;
    if (std::find(labels.begin(), labels.end(), x.s) == labels.end()) labels.push_back(x.s);
  }
  out = std::move(labels);
  return true;
}

}  // namespace

const char* role_name(role r) noexcept {
  switch (r) {
    case role::view:
      return "view";
    case role::change:
      return "change";
    case role::admin:
      return "admin";
  }
  return "view";
}

const char* role_words(role r) noexcept {
  switch (r) {
    case role::view:
      return "can view files";
    case role::change:
      return "can change files";
    case role::admin:
      return "admin";
  }
  return "can view files";
}

std::optional<role> parse_role(std::string_view s) noexcept {
  if (s == "view") return role::view;
  if (s == "change") return role::change;
  if (s == "admin") return role::admin;
  return std::nullopt;
}

bool user::sees(std::string_view label) const {
  return !folders || std::find(folders->begin(), folders->end(), label) != folders->end();
}

bool valid_name(std::string_view name, std::string_view self) {
  if (name.empty() || name.size() > kMaxNameBytes) return false;
  if (!(name[0] >= 'a' && name[0] <= 'z')) return false;
  for (char c : name) {
    if (!lower_alnum(c) && c != '.' && c != '_' && c != '-') return false;
  }
  if (name.back() == '.') return false;  // Windows drops a trailing dot
  if (std::find(kReserved.begin(), kReserved.end(), name) != kReserved.end()) return false;
  if (!self.empty() && lowered(self) == name) return false;
  return true;
}

bool valid_label(std::string_view label) {
  if (label.empty() || label.size() > kMaxLabelBytes) return false;
  return std::none_of(label.begin(), label.end(), [](char c) {
    const auto u = static_cast<unsigned char>(c);
    return u < 0x20 || u == 0x7f;
  });
}

std::string encode(const std::vector<user>& list) {
  json::writer w;
  w.begin_object().key("v").integer(1).key("users").begin_array();
  for (const auto& u : list) {
    w.begin_object().key("name").string(u.name).key("role").string(role_name(u.r));
    if (u.folders) {
      w.key("folders").begin_array();
      for (const auto& f : *u.folders) w.string(f);
      w.end_array();
    }
    w.key("created").integer(u.created).key("applied").integer(u.applied).end_object();
  }
  w.end_array().end_object();
  return w.take();
}

std::optional<std::vector<user>> decode(std::string_view text) {
  if (text.size() > 64 * 1024) return std::nullopt;
  const auto doc = json::parse(text, 5);
  const json::value* list = doc ? doc->find("users") : nullptr;
  if (!doc || doc->integer("v") != 1 || !list || list->k != json::kind::array || list->a.size() > kMaxUsers) {
    return std::nullopt;
  }
  std::vector<user> out;
  for (const auto& e : list->a) {
    user u;
    const std::string* name = e.str("name");
    const std::string* r = e.str("role");
    const auto created = e.integer("created");
    const auto applied = e.integer("applied");
    const auto parsed = r ? parse_role(*r) : std::nullopt;
    if (!name || !valid_name(*name) || !parsed || !created || !applied || *created < 0 || *applied < 0 ||
        !read_folders(e, u.folders)) {
      return std::nullopt;
    }
    if (find(out, *name)) return std::nullopt;
    u.name = *name;
    u.r = *parsed;
    u.created = *created;
    u.applied = *applied;
    out.push_back(std::move(u));
  }
  return out;
}

bool load(secrets::store& store, std::vector<user>& out) {
  out.clear();
  if (!store.exists(kUsersName)) return true;
  const auto bytes = store.get(kUsersName);
  if (!bytes) return false;
  auto list = decode(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
  if (!list) return false;
  out = std::move(*list);
  return true;
}

bool save(secrets::store& store, const std::vector<user>& list) {
  if (list.empty()) {
    (void)store.erase(kUsersName);
    return !store.exists(kUsersName);
  }
  const std::string text = encode(list);
  return store.put(kUsersName, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

const user* find(const std::vector<user>& list, std::string_view name) {
  const auto it = std::find_if(list.begin(), list.end(), [&](const user& u) { return u.name == name; });
  return it == list.end() ? nullptr : &*it;
}

std::string upsert(std::vector<user>& list, user u, std::int64_t now, std::string_view self) {
  if (!valid_name(u.name, self)) return "bad-name";
  if (u.folders) {
    if (u.folders->empty() || u.folders->size() > kMaxFolders ||
        !std::all_of(u.folders->begin(), u.folders->end(), [](const std::string& f) { return valid_label(f); })) {
      return "bad-folders";
    }
    std::vector<std::string> unique;
    for (auto& f : *u.folders) {
      if (std::find(unique.begin(), unique.end(), f) == unique.end()) unique.push_back(std::move(f));
    }
    u.folders = std::move(unique);
  }
  const auto it = std::find_if(list.begin(), list.end(), [&](const user& x) { return x.name == u.name; });
  if (it == list.end()) {
    if (list.size() >= kMaxUsers) return "too-many";
    u.created = now;
    u.applied = 0;
    list.push_back(std::move(u));
    return {};
  }
  const bool same = it->r == u.r && it->folders == u.folders;
  u.created = it->created;
  u.applied = same ? it->applied : 0;
  *it = std::move(u);
  return {};
}

bool remove(std::vector<user>& list, std::string_view name) {
  const auto before = list.size();
  std::erase_if(list, [&](const user& u) { return u.name == name; });
  return list.size() != before;
}

std::uint32_t windows_share_mask(role r) noexcept {
  // FILE_GENERIC_READ | FILE_GENERIC_EXECUTE; + write and DELETE (Change); GENERIC full (FILE_ALL_ACCESS).
  switch (r) {
    case role::view:
      return 0x001200A9u;
    case role::change:
      return 0x001301BFu;
    case role::admin:
      return 0x001F01FFu;
  }
  return 0x001200A9u;
}

std::uint32_t windows_ntfs_mask(role r) noexcept {
  // Read & execute (RX), Modify (M), Full control (F): the same masks as icacls names.
  return windows_share_mask(r);
}

std::string mac_acl_perms(role r) {
  std::string p = "list,search,readattr,readextattr,readsecurity";
  if (r == role::change || r == role::admin) p += ",add_file,add_subdirectory,delete,delete_child,writeattr,writeextattr";
  if (r == role::admin) p += ",writesecurity,chown";
  return p;
}

std::string share_name(std::string_view label) {
  static constexpr std::string_view kBad = "\"/\\[]:|<>+=;,?*";
  std::string out;
  for (char c : label) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f || kBad.find(c) != std::string_view::npos) continue;
    out.push_back(c);
  }
  while (!out.empty() && (out.front() == ' ' || out.front() == '.')) out.erase(out.begin());
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  if (out.size() > 80) {
    out.resize(80);
    // Never cut a UTF-8 character in two.
    while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) out.pop_back();
    if (!out.empty() && static_cast<unsigned char>(out.back()) >= 0xC0) out.pop_back();
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  }
  if (out.empty()) out = "Share";
  return out;
}

std::string unc_address(std::string_view computer, std::string_view share) {
  if (computer.empty() || share.empty()) return {};
  return "\\\\" + std::string(computer) + "\\" + std::string(share);
}

std::string smb_url(std::string_view host, std::string_view share) {
  if (host.empty() || share.empty()) return {};
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out = "smb://" + std::string(host) + "/";
  for (char c : share) {
    const auto u = static_cast<unsigned char>(c);
    const bool plain = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || c == '-' ||
                       c == '_' || c == '.' || c == '~';
    if (plain) {
      out.push_back(c);
    } else {
      out.push_back('%');
      out.push_back(kHex[u >> 4]);
      out.push_back(kHex[u & 15]);
    }
  }
  return out;
}

}  // namespace mv::driftwire::serve::share_users
