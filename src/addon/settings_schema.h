// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A settings page an add-on describes as data (docs/design/25 "Settings page",
// contribution API 2). Both hosts render it with their own controls, in the
// order it is written, so one schema gives the same page in SwiftUI and WinUI
// (D9). The add-on writes no UI code.
//
//   "settings": {
//     "title": "Places",
//     "sections": [
//       { "title": "Place", "help": "How a photo's place is named.",
//         "items": [
//           { "type": "toggle", "key": "show_map", "label": "Show the map",
//             "default": true },
//           { "type": "toggle", "key": "show_town", "label": "Name the town",
//             "default": true },
//           { "type": "number", "key": "town_radius_km", "label": "Town radius",
//             "min": 1, "max": 50, "step": 1, "unit": "km", "default": 15,
//             "help": "Name the nearest town within this distance.",
//             "enabled_by": "show_town" },
//           { "type": "choice", "key": "label_names", "label": "Place names",
//             "options": [ { "value": "english", "label": "English" },
//                          { "value": "local", "label": "Local" } ],
//             "default": "english" },
//           { "type": "text", "key": "home", "label": "Home town",
//             "max_length": 64, "placeholder": "Lisbon", "default": "" } ] } ] }
//
// Strict: an unknown key, a duplicate setting key, a default outside its own
// rule, or text a person could be misled by (display_text) refuses the page,
// and with it the add-on. Bounded everywhere: the page comes from a stranger.
// Portable (D9).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.h"

namespace mv::addon {

inline constexpr std::size_t kSettingsMaxSections = 16;
inline constexpr std::size_t kSettingsMaxItems = 64;  // over the whole page
inline constexpr std::size_t kSettingsMaxOptions = 32;
inline constexpr std::size_t kSettingsMaxText = 1024;  // a text value's ceiling
inline constexpr double kSettingsNumberLimit = 1e9;

enum class setting_kind : std::uint8_t { toggle = 0, choice, number, text };

[[nodiscard]] const char* setting_kind_name(setting_kind k) noexcept;

struct setting_option {
  std::string value;  // [a-z0-9_-]{1,40}; what is stored
  std::string label;  // what is shown
};

struct setting_item {
  setting_kind kind = setting_kind::toggle;
  std::string key;         // [a-z][a-z0-9_]{0,39}; unique on the page
  std::string label;
  std::string help;        // may be empty
  std::string enabled_by;  // a toggle earlier on the page, or empty
  // The default, by kind: toggle `flag`; number `number`; choice and text `text`.
  bool flag = false;
  double number = 0.0;
  std::string text;
  // number
  double min = 0.0, max = 0.0, step = 1.0;
  std::string unit;  // "km"; may be empty
  // choice
  std::vector<setting_option> options;
  // text
  std::uint32_t max_length = 256;
  std::string placeholder;
};

struct setting_section {
  std::string title;
  std::string help;
  std::vector<setting_item> items;
};

struct settings_page {
  std::string title;  // may be empty: the host uses the add-on's name
  std::vector<setting_section> sections;
  [[nodiscard]] bool empty() const noexcept { return sections.empty(); }
  [[nodiscard]] const setting_item* find(std::string_view key) const noexcept;
};

// The page under `contributes.settings`. nullopt when it breaks any rule;
// `detail` (when given) names the first rule broken, never more than the key.
[[nodiscard]] std::optional<settings_page> parse_settings_page(const json::value& v,
                                                               std::string* detail = nullptr);
// The same from text (the fuzz harness, the SDK cross-check).
[[nodiscard]] std::optional<settings_page> parse_settings_page_text(std::string_view json_text,
                                                                    std::string* detail = nullptr);

// Whether `value` is one `item` may hold: the right JSON kind, inside its
// range, one of its options, short enough.
[[nodiscard]] bool setting_value_fits(const setting_item& item, const json::value& value) noexcept;

// The item's default as JSON text: true, 15, 2.5, "english".
[[nodiscard]] std::string setting_default_json(const setting_item& item);

// A value as JSON text, the number form shared by the store and the hosts:
// integers without a point, anything else to three decimals.
[[nodiscard]] std::string setting_value_json(const json::value& value);

// One line a person reads (a label, a title, help): no control characters,
// nothing that reorders or hides what is beside it (bidi controls, zero
// width), no leading or trailing space, at most `max_bytes`. The rule the
// consent sheet's text is held to (open_manifest.cpp).
[[nodiscard]] bool valid_display_text(std::string_view s, std::size_t max_bytes,
                                      bool may_be_empty) noexcept;

}  // namespace mv::addon
