// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// What people chose on add-ons' settings pages (docs/design/25 "Settings page").
//
//   <root>/<add-on id>.json   {"schema": 1, "values": {"show_map": false, ...}}
//
// <root> is io::addon_settings_dir(), beside the add-on folders and never in
// one, so removing an add-on can keep its settings (the default) or forget
// them, and installing it again finds them. Namespaced by add-on id: the
// file name is the id, which is a folder-safe name for both kinds (an open
// add-on's dotted id, a first-party one's plain id). Nothing is created until
// a value is written.
//
// One store for both hosts and for add-on code: the SwiftUI and WinUI
// Settings pages write here through the bridge / ABI, a first-party add-on
// reads through the host table (settings_get), and a script (PR 57) through
// the same getter. Values are checked against the page on the way in, and
// again on the way out, so a page that changed in an update never hands an
// add-on a value its new page does not allow: it gets the default.
//
// Thread-safe. Reads are served from a process-wide cache after the first;
// writes go to disk atomically before they return. Worker threads only
// (rule 1): a first read and every write touch the disk.
#pragma once

#include <string>
#include <string_view>

#include "addon/settings_schema.h"
#include "core/result.h"

namespace mv::addon {

class open_store;

// An id a settings file may be named for: an open add-on's id
// (valid_open_id) or a first-party add-on's ([a-z0-9-]{1,40}, no dot).
[[nodiscard]] bool valid_settings_owner(std::string_view id) noexcept;

class settings_values {
 public:
  explicit settings_values(std::string root);

  // The page with each item's current value folded in, as the hosts render
  // it, in the order it was written:
  //   {"id", "title", "sections": [{"title", "help", "items": [{"type", "key",
  //    "label", "help", "enabled_by", "default", "value", ...type fields}]}]}
  // Type fields: number "min", "max", "step", "unit"; choice "options":
  // [{"value", "label"}]; text "max_length", "placeholder". `title` is the
  // page's own, else `fallback_title`.
  [[nodiscard]] std::string page_json(std::string_view id, const settings_page& page,
                                      std::string_view fallback_title) const;

  // The value of `key` as JSON text (true, 15, "english"): what was chosen if
  // it still fits the item, else the item's default. invalid_arg when the
  // page has no such key.
  [[nodiscard]] result<std::string> value_json(std::string_view id, const settings_page& page,
                                               std::string_view key) const;

  // Stores `value_json` for `key` once it fits the item. invalid_arg for an
  // unknown key, a value of the wrong kind or outside the item's rule.
  [[nodiscard]] expected set(std::string_view id, const settings_page& page, std::string_view key,
                             std::string_view value_json) const;

  // Removes everything chosen for `id` (remove without keep). OK when there
  // was nothing.
  [[nodiscard]] expected forget(std::string_view id) const;

  // Whether anything is stored for `id`.
  [[nodiscard]] bool has_values(std::string_view id) const;

  [[nodiscard]] const std::string& root() const noexcept { return root_; }

 private:
  [[nodiscard]] std::string path_of(std::string_view id) const;
  std::string root_;
};

// The store over io::addon_settings_dir().
[[nodiscard]] result<settings_values> default_settings_values();

// An installed open add-on's value for `key` (the getter add-on code reads;
// PR 57's mv.settings.get): the page from its verified manifest, the value
// from default_settings_values(). invalid_arg when the add-on is not
// installed or its page has no such key.
[[nodiscard]] result<std::string> open_setting_value_json(const open_store& store,
                                                          std::string_view addon_id,
                                                          std::string_view key);

// Tests: forget what the process-wide cache holds.
void reset_settings_cache_for_tests();

}  // namespace mv::addon
