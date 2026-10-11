// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Keymap packs on the live table (docs/design/16 "Keymap packs", docs/design/25 PR 56
// verify: a keymap pack rebinds and Reset restores; `?` lists what is bound).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "shell/commands.h"
#include "shell/key_router.h"
#include "shell/keymap.h"

using namespace mv::shell;
using mv::addon::keymap_binding;

namespace {

bool same_table(std::span<const binding> a, std::span<const binding> b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].k != b[i].k || a[i].mods != b[i].mods || a[i].command != b[i].command ||
        a[i].modes != b[i].modes) {
      return false;
    }
  }
  return true;
}

// The `?` line of live row `row` ("id\tmodes\tname\tkeys\tpalette\trow").
std::string help_line(std::size_t row) {
  const std::string table = describe_commands();
  const std::string tail = "\t" + std::to_string(row) + "\n";
  std::size_t at = 0;
  while ((at = table.find(tail, at)) != std::string::npos) {
    const std::size_t start = table.rfind('\n', at);
    const std::string line =
        table.substr(start == std::string::npos ? 0 : start + 1, at + tail.size() - (start + 1));
    // Six fields: the row index is the last.
    if (std::count(line.begin(), line.end(), '\t') == 5) return line;
    at += tail.size();
  }
  return {};
}

bool addon_command_slot(command_id id) {
  const int n = static_cast<int>(id) - static_cast<int>(command_id::addon_cmd_0);
  return n >= 0 && n < kAddonCommandSlots;
}

struct restore_table {
  ~restore_table() {
    set_addon_commands({});
    reset_live_bindings();
  }
};

view_state still() {
  view_state s;
  s.item = item_kind::still;
  return s;
}

}  // namespace

TEST_CASE("every command a row runs has a stable name, and names are unique", "[keys][keymap]") {
  std::set<std::string_view> seen;
  for (const binding& b : default_bindings()) {
    for (const command_id id : {b.command, b.hold, b.release}) {
      if (id == command_id::none) continue;
      const std::string_view slug = command_slug(id);
      INFO(static_cast<int>(id));
      REQUIRE_FALSE(slug.empty());
      REQUIRE(command_by_slug(slug) == id);
    }
  }
  for (const command_info& info : command_infos()) {
    if (is_retired_command(static_cast<int>(info.id)) || addon_command_slot(info.id)) continue;
    const std::string_view slug = command_slug(info.id);
    INFO(info.name);
    REQUIRE_FALSE(slug.empty());
    REQUIRE(seen.insert(slug).second);
  }
  REQUIRE(command_slug(command_id::none).empty());
  REQUIRE(command_slug(command_id::palette).empty());
  REQUIRE(command_slug(command_id::addon_cmd_0).empty());
  REQUIRE(command_by_slug("next") == command_id::next);
  REQUIRE(command_by_slug("Next") == command_id::none);
  REQUIRE(command_by_slug("") == command_id::none);
}

TEST_CASE("a pack rebinds any built-in row, `?` lists it, and Reset restores", "[keys][keymap]") {
  restore_table restore;
  reset_live_bindings();
  const auto defaults = default_bindings();
  // A chord no default row holds.
  constexpr const char* kFree = "Ctrl+Shift+Alt+F11";
  key free_k = key::none;
  std::uint8_t free_mods = 0;
  REQUIRE(parse_key_label(kFree, free_k, free_mods));
  for (const binding& b : defaults) REQUIRE_FALSE((b.k == free_k && b.mods == free_mods));

  std::set<command_id> done;
  for (std::size_t i = 0; i < defaults.size(); ++i) {
    const command_id id = defaults[i].command;
    if (!done.insert(id).second) continue;  // a command's first row
    const std::string slug(command_slug(id));
    INFO(slug);
    const keymap_binding kb{slug, {kFree}};
    const keymap_result r = apply_keymap(std::span<const keymap_binding>(&kb, 1));
    REQUIRE(r.skipped.empty());
    REQUIRE(r.displaced == 0);
    const auto live = live_bindings();
    REQUIRE(live[i].k == free_k);
    REQUIRE(live[i].mods == free_mods);
    // Every other row is as it was.
    for (std::size_t j = 0; j < defaults.size(); ++j) {
      if (j == i) continue;
      REQUIRE(live[j].k == defaults[j].k);
      REQUIRE(live[j].mods == defaults[j].mods);
    }
    // `?` reads the same table: the row's line names the new key.
    const std::string line = help_line(i);
    if (!line.empty()) REQUIRE(line.find(key_label(free_k, free_mods)) != std::string::npos);
    reset_live_bindings();
    REQUIRE(same_table(live_bindings(), defaults));
  }
  REQUIRE(done.size() > 100);
}

TEST_CASE("a pack's keys go to a command's rows in turn; the router follows", "[keys][keymap]") {
  restore_table restore;
  // next's rows in table order: give the first L, clear the second, keep the rest.
  std::vector<int> rows;
  const auto defaults = default_bindings();
  for (std::size_t i = 0; i < defaults.size(); ++i) {
    if (defaults[i].command == command_id::next) rows.push_back(static_cast<int>(i));
  }
  REQUIRE(rows.size() >= 3);
  const std::vector<keymap_binding> pack{{"next", {"L", ""}}};
  const keymap_result r = apply_keymap(pack);
  REQUIRE(r.skipped.empty());
  REQUIRE(r.rows_changed == 2);
  const auto live = live_bindings();
  REQUIRE(live[static_cast<std::size_t>(rows[0])].k == char_key('L'));
  REQUIRE(live[static_cast<std::size_t>(rows[1])].k == key::none);
  REQUIRE(live[static_cast<std::size_t>(rows[2])].k == defaults[static_cast<std::size_t>(rows[2])].k);

  key_router router;
  router.rebuild(live_bindings());
  REQUIRE(router.on_key({char_key('L'), mod_none, false, false}, still()).command == command_id::next);
  reset_live_bindings();
  router.rebuild(live_bindings());
  REQUIRE(router.on_key({char_key('L'), mod_none, false, false}, still()).command != command_id::next);
}

TEST_CASE("a key the pack gives out is taken from the row that had it", "[keys][keymap]") {
  restore_table restore;
  // Insert marks by default; a pack that puts next on Insert leaves the mark
  // row without a key (shown so in Settings), never two rows on one key.
  key_router router;
  REQUIRE(router.on_key({key::insert, mod_none, false, false}, still()).command ==
          command_id::toggle_mark);
  const std::vector<keymap_binding> pack{{"next", {key_label(key::insert, mod_none)}}};
  const keymap_result r = apply_keymap(pack);
  REQUIRE(r.skipped.empty());
  REQUIRE(r.displaced >= 1);
  router.rebuild(live_bindings());
  REQUIRE(router.on_key({key::insert, mod_none, false, false}, still()).command == command_id::next);
  for (const binding& b : live_bindings()) {
    if (b.command == command_id::toggle_mark) REQUIRE_FALSE((b.k == key::insert && b.mods == mod_none));
  }
}

TEST_CASE("what a pack cannot apply is skipped and said, the rest applies", "[keys][keymap]") {
  restore_table restore;
  const std::vector<keymap_binding> pack{
      {"no_such_command", {"Q"}},
      {"next", {"Hyper+X"}},
      {"prev", {"J"}},
      {"first", {"J"}},  // J went to prev first
      {"toggle_mark", {"K", "Ctrl+K", "Alt+K", "Shift+K", "Ctrl+Alt+K", "Ctrl+Shift+K", "F9", "F10"}},
      {"acme.tools/count-files", {"Ctrl+Shift+Y"}},  // its add-on is not loaded
  };
  const keymap_result r = apply_keymap(pack);
  REQUIRE(r.skipped.size() >= 4);
  const auto has = [&](const std::string& part) {
    return std::any_of(r.skipped.begin(), r.skipped.end(),
                       [&](const std::string& s) { return s.find(part) != std::string::npos; });
  };
  REQUIRE(has("unknown command: no_such_command"));
  REQUIRE(has("not a key: Hyper+X"));
  REQUIRE(has("J is already given to prev"));
  REQUIRE(has("extra key"));
  REQUIRE(has("unknown command: acme.tools/count-files"));
  key_router router;
  router.rebuild(live_bindings());
  REQUIRE(router.on_key({char_key('J'), mod_none, false, false}, still()).command == command_id::prev);
}

TEST_CASE("a pack may bind a row an add-on contributes, by add-on and id", "[keys][keymap]") {
  restore_table restore;
  addon_command_row row;
  row.addon = "acme.tools";
  row.id = "count-files";
  row.name = "Count files";
  row.modes = parse_modes("viewing");
  row.payload = "none";
  set_addon_commands(std::span<const addon_command_row>(&row, 1));
  const std::vector<keymap_binding> pack{{"acme.tools/count-files", {"Ctrl+Shift+Y"}}};
  const keymap_result r = apply_keymap(pack);
  REQUIRE(r.skipped.empty());
  key_router router;
  router.rebuild(live_bindings());
  REQUIRE(router.on_key({char_key('Y'), static_cast<std::uint8_t>(mod_ctrl | mod_shift), false, false},
                        still())
              .command == command_id::addon_cmd_0);
}

TEST_CASE("Reset keeps an add-on's superseded built-in rows without their key", "[keys][keymap]") {
  restore_table restore;
  addon_command_row row;
  row.addon = "import";
  row.id = "open";
  row.name = "Import…";
  REQUIRE(parse_key_label("Ctrl+Shift+I", row.k, row.mods));
  row.modes = parse_modes("viewing");
  set_addon_commands(std::span<const addon_command_row>(&row, 1));
  reset_live_bindings();
  for (const binding& b : live_bindings()) {
    if (b.command == command_id::open_import) REQUIRE(b.k == key::none);
  }
}

TEST_CASE("a saved table puts back unbound rows exactly", "[keys][keymap]") {
  restore_table restore;
  const std::vector<keymap_binding> pack{{"next", {"L", ""}}, {"toggle_mark", {""}}};
  (void)apply_keymap(pack);
  // What the hosts persist: every row that differs from the default.
  struct saved {
    int row;
    key k;
    std::uint8_t mods;
  };
  std::vector<saved> out;
  const auto live = live_bindings();
  const auto def = default_bindings();
  for (std::size_t i = 0; i < def.size(); ++i) {
    if (live[i].k != def[i].k || live[i].mods != def[i].mods) {
      out.push_back({static_cast<int>(i), live[i].k, live[i].mods});
    }
  }
  const std::vector<binding> before(live.begin(), live.end());
  reset_live_bindings();
  for (const saved& s : out) REQUIRE(set_live_key(s.row, s.k, s.mods));
  REQUIRE(same_table(live_bindings(), before));
  REQUIRE_FALSE(set_live_key(-1, key::none, 0));
  REQUIRE_FALSE(set_live_key(100000, key::none, 0));
}

TEST_CASE("a pack read back from the core's JSON", "[keys][keymap]") {
  std::vector<keymap_binding> out;
  REQUIRE(keymap_bindings_from_json(
      R"({"addon":"acme.vim","id":"vim","name":"Vim","bindings":[{"command":"next","keys":["L","Right"]}]})",
      out));
  REQUIRE(out.size() == 1);
  REQUIRE(out[0].keys.size() == 2);
  REQUIRE_FALSE(keymap_bindings_from_json("[]", out));
  REQUIRE_FALSE(keymap_bindings_from_json(R"({"bindings":[{"command":"next","keys":[1]}]})", out));
  REQUIRE_FALSE(keymap_bindings_from_json(R"({"bindings":[{"keys":["L"]}]})", out));
}
