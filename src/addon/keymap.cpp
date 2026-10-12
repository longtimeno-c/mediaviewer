// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/keymap.h"

#include <algorithm>
#include <set>

#include "addon/settings_schema.h"

namespace mv::addon {
namespace {

bool fail(std::string* detail, std::string_view what) {
  if (detail) detail->assign(what);
  return false;
}

bool pack_id(std::string_view s) noexcept {
  if (s.empty() || s.size() > 32 || s.front() == '-' || s.back() == '-') return false;
  return std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
  });
}

// A command's stable name, or "<add-on id>/<command id>": lowercase ASCII,
// digits, '_', '-', '.', and at most one '/'.
bool command_name(std::string_view s) noexcept {
  if (s.empty() || s.size() > 100 || s.front() == '/' || s.back() == '/') return false;
  int slashes = 0;
  for (const char c : s) {
    if (c == '/') {
      if (++slashes > 1) return false;
      continue;
    }
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
      return false;
    }
  }
  return true;
}

// A key label: printable ASCII, no spaces at the ends, 32 bytes at most.
// Empty means "no key".
bool label_text(std::string_view s) noexcept {
  if (s.size() > 32) return false;
  if (!s.empty() && (s.front() == ' ' || s.back() == ' ')) return false;
  return std::all_of(s.begin(), s.end(), [](char c) { return c >= 0x20 && c <= 0x7E; });
}

}  // namespace

std::optional<std::vector<keymap_pack>> parse_keymaps(const json::value& v, std::string* detail) {
  if (v.k != json::kind::array || v.a.empty() || v.a.size() > kKeymapMaxPacks) {
    fail(detail, "keymaps");
    return std::nullopt;
  }
  std::vector<keymap_pack> out;
  std::set<std::string> ids;
  for (const json::value& p : v.a) {
    if (p.k != json::kind::object) {
      fail(detail, "pack");
      return std::nullopt;
    }
    for (const auto& [name, value] : p.o) {
      (void)value;
      if (name != "id" && name != "name" && name != "bindings") {
        fail(detail, "pack: unknown key");
        return std::nullopt;
      }
    }
    keymap_pack pack;
    const std::string* id = p.str("id");
    const std::string* name = p.str("name");
    const json::value* bindings = p.find("bindings");
    if (!id || !pack_id(*id) || !ids.insert(*id).second) {
      fail(detail, "pack: id");
      return std::nullopt;
    }
    pack.id = *id;
    if (!name || !valid_display_text(*name, 64, false)) {
      fail(detail, pack.id + ": name");
      return std::nullopt;
    }
    pack.name = *name;
    if (!bindings || bindings->k != json::kind::array || bindings->a.empty() ||
        bindings->a.size() > kKeymapMaxBindings) {
      fail(detail, pack.id + ": bindings");
      return std::nullopt;
    }
    std::set<std::string> commands;
    for (const json::value& b : bindings->a) {
      if (b.k != json::kind::object) {
        fail(detail, pack.id + ": binding");
        return std::nullopt;
      }
      for (const auto& [name2, value] : b.o) {
        (void)value;
        if (name2 != "command" && name2 != "keys") {
          fail(detail, pack.id + ": binding: unknown key");
          return std::nullopt;
        }
      }
      const std::string* command = b.str("command");
      const json::value* keys = b.find("keys");
      if (!command || !command_name(*command) || !commands.insert(*command).second) {
        fail(detail, pack.id + ": command");
        return std::nullopt;
      }
      keymap_binding kb;
      kb.command = *command;
      if (!keys || keys->k != json::kind::array || keys->a.empty() ||
          keys->a.size() > kKeymapMaxKeys) {
        fail(detail, kb.command + ": keys");
        return std::nullopt;
      }
      for (const json::value& k : keys->a) {
        if (k.k != json::kind::string || !label_text(k.s)) {
          fail(detail, kb.command + ": key");
          return std::nullopt;
        }
        kb.keys.push_back(k.s);
      }
      pack.bindings.push_back(std::move(kb));
    }
    // What both hosts carry (issue #356): refused here, as the SDK refuses it,
    // rather than by one host when the pack is applied.
    if (keymap_pack_json_bytes(pack) > kKeymapMaxPackBytes) {
      fail(detail, pack.id + ": too large");
      return std::nullopt;
    }
    out.push_back(std::move(pack));
  }
  return out;
}

std::optional<std::vector<keymap_pack>> parse_keymaps_text(std::string_view text,
                                                           std::string* detail) {
  if (text.empty() || text.size() > (256u << 10)) return std::nullopt;
  const auto doc = json::parse(text, 8);
  if (!doc) {
    fail(detail, "json");
    return std::nullopt;
  }
  return parse_keymaps(*doc, detail);
}

namespace {

void write_pack(json::writer& w, const keymap_pack& p) {
  w.begin_object();
  w.key("id").string(p.id);
  w.key("name").string(p.name);
  w.key("bindings").begin_array();
  for (const keymap_binding& b : p.bindings) {
    w.begin_object().key("command").string(b.command).key("keys").begin_array();
    for (const std::string& k : b.keys) w.string(k);
    w.end_array().end_object();
  }
  w.end_array();
  w.end_object();
}

}  // namespace

std::string keymaps_to_json(const std::vector<keymap_pack>& packs) {
  json::writer w;
  w.begin_array();
  for (const keymap_pack& p : packs) write_pack(w, p);
  w.end_array();
  return w.take();
}

std::size_t keymap_pack_json_bytes(const keymap_pack& pack) {
  json::writer w;
  write_pack(w, pack);
  return w.str().size();
}

}  // namespace mv::addon
