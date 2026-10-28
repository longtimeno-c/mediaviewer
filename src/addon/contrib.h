// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// What an add-on contributes to screens and slots (docs/design/25 §7, §10; PRs 58-59).
//
// Plain data, parsed once from a manifest at load (manifest.cpp for MediaViewer's
// own schema 1 add-ons; a script add-on's manifest fills the same struct when
// PR 57 lands) and resolved into the tables slot_registry serves. Nothing here
// is looked up per frame, per tile or per key: the chromes read the tables.
// Portable (D9).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mv::addon {

// The contribution API this host serves (docs/design/25 §7). 4 = screens,
// 5 = chrome slots and search providers, 6 = the screen vocabulary's `link`
// node and a button's `copy` (2026-10-19), 7 = `opens` on a status item
// (#512, 2026-10-20), 8 = a screen in place "settings" (2026-10-21): drawn on
// the add-on's own page in Settings -> Add-ons, 9 = a surface's `glow` and a
// progress bar's "glow" look (2026-10-28, Enhance). Points of an API this host knows but
// has not built (settings pages, keymaps: PR 56; script commands: PR 57) make
// a manifest that names them "needs a newer MediaViewer", never half-loaded.
inline constexpr std::uint32_t kSlotsContributionApi = 9;
// The contribution API from which a status item may name the screen it opens.
inline constexpr std::uint32_t kStatusOpensApi = 7;
// The contribution API from which a screen may be in place "settings".
inline constexpr std::uint32_t kSettingsScreenApi = 8;
inline constexpr std::uint32_t kScreensApi = 4;
inline constexpr std::uint32_t kSlotsApi = 5;
// The host function table version that carries the screen / slot calls
// (mediaviewer_addon.h, v3). A manifest naming a point needs host_api.min >= this.
inline constexpr std::uint32_t kSlotsHostApi = 3;

// Where a screen shows (§10 "Places"). `settings`: on the add-on's own page in
// Settings -> Add-ons, under its card, opened while that page shows and closed
// when it goes. Only the chrome opens one: never an item, a status click or
// the add-on's screen_show. At most one per add-on.
enum class screen_place : std::uint8_t { info_section, window, pane, sheet, settings };

// The glyphs a chrome item or badge may name: each host maps them to its own
// symbol set (SF Symbols / Segoe Fluent Icons). An add-on ships no icon file
// for a slot, so every item looks like the rest of the chrome.
inline constexpr std::array<std::string_view, 25> kSymbols{
    "map",   "pin",    "star",  "tag",    "search", "info",  "person", "camera",
    "folder", "sparkle", "check", "flag",  "heart",  "clock", "globe",  "photo",
    "film",  "bolt",   "gear",  "plus",   "list",   "grid",  "share",  "warning", "minus"};

[[nodiscard]] constexpr bool known_symbol(std::string_view s) noexcept {
  for (std::string_view k : kSymbols) {
    if (k == s) return true;
  }
  return false;
}

struct screen_contrib {
  std::string id;      // [a-z0-9_-]{1,32}, unique among screens and info sections
  std::string title;   // window / pane / sheet title
  screen_place place = screen_place::window;
  std::uint32_t width = 0;   // points; 0 = the host's default for the place
  std::uint32_t height = 0;
};

struct info_section_contrib {
  std::string id;
  std::string title;            // the section's heading in the Metadata pane
  bool needs_location = false;  // "when": "location" -- asked only for a geotagged file
  std::int32_t order = 0;       // lower first; ties by add-on id then section id
};

struct search_provider_contrib {
  std::string id;
  std::string name;                   // "Places": shown beside a result ("matched by Places")
  std::vector<std::string> prefixes;  // "place:" -- a query starting with one goes to this provider alone
  bool files = false;                 // answers file search (Ctrl/Cmd+F, the base app)
  bool local = false;                 // answers Local search (the AI panel)
};

// A command-bar, path-bar, context-menu or status item.
enum class item_slot : std::uint8_t { bar, path, context, status };
enum class item_target : std::uint8_t { none, file, files, folder };

struct item_contrib {
  item_slot slot = item_slot::bar;
  std::string id;
  std::string label;    // the visible / accessible name
  std::string symbol;   // one of kSymbols; empty for a context-menu entry or a status item
  std::string opens;    // a screen id of this add-on to open, or empty (the add-on gets the event)
  item_target target = item_target::none;  // what rides along to the add-on
};

struct badge_contrib {
  std::string id;
  std::string label;   // read by VoiceOver / Narrator with the tile ("Has a place")
  std::string symbol;  // one of kSymbols
};

struct contributions {
  std::uint32_t api_min = 0;  // contributes.api; 0 when the manifest names no screen or slot
  std::uint32_t api_max = 0;
  std::vector<screen_contrib> screens;
  std::vector<info_section_contrib> info_sections;
  std::vector<search_provider_contrib> search_providers;
  std::vector<item_contrib> items;
  std::vector<badge_contrib> badges;
  // The manifest's command ids (`contributes.commands`). For an add-on that
  // fills slots, running one is a command event to it (slot_registry::
  // run_command, 2026-10-15): it has no chrome to send it to. Filled at load.
  std::vector<std::string> commands;

  [[nodiscard]] bool empty() const noexcept {
    return screens.empty() && info_sections.empty() && search_providers.empty() && items.empty() &&
           badges.empty();
  }
};

}  // namespace mv::addon
