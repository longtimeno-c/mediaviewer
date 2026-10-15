// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Keymap packs applied to the live binding table (docs/design/16 "Keymap packs",
// docs/design/25 "Keymap"). A pack is data an add-on ships (addon/keymap.h); this
// is where its names become rows and its labels become keys.
//
// Applied ONCE, when the person picks the pack in Settings: the table is
// reset to default_bindings() and the pack's keys are written into the rows
// it names. Nothing is looked up per key afterwards; the router is rebuilt
// from the table as for any remap and stays an array index. What the pack
// wrote is then the person's table, persisted like any remap, and stays when
// the add-on goes. Reset restores default_bindings().
//
// Pure C++, no platform header: both hosts and the unit tests call it.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "addon/keymap.h"
#include "shell/commands.h"

namespace mv::shell {

// A command's stable name for packs and the SDK: its enumerator's name in
// commands.h ("next", "zoom_in", "toggle_mark"). "" for none, the retired
// palette id, and the add-on slots (named "<add-on id>/<command id>").
[[nodiscard]] std::string_view command_slug(command_id id) noexcept;
// The inverse; command_id::none for a name no command has.
[[nodiscard]] command_id command_by_slug(std::string_view slug) noexcept;

struct keymap_result {
  int rows_changed = 0;  // rows whose key the pack set (or cleared)
  int displaced = 0;     // other rows left without a key because the pack took it
  // What was not applied, one line each, for Settings: "unknown command:
  // foo", "not a key: Hyper+X", "Ctrl+K is already given to next", ...
  std::vector<std::string> skipped;
};

// Resets the live table to the defaults, then applies `bindings`:
//  - each binding's keys go, in turn, to the rows of its command in table
//    order; "" leaves a row without a key; rows past the end keep their default;
//  - a key two bindings of the pack claim in overlapping modes goes to the
//    first; the later one is skipped;
//  - any other row holding a key the pack gave out, in an overlapping mode,
//    is left without a key (shown so in Settings, never silent);
//  - a key `host_owns` says the host keeps for itself (the Mac's fixed menu
//    chords: ⌘Q, ⌘H, ⌘M, ⌘⌫, which the menu sees before any row) is skipped,
//    as Settings' key capture refuses it (issue #182); the row keeps its
//    default.
// The caller rebuilds the router, persists the table and republishes `?`.
using host_owns_key = bool (*)(key k, std::uint8_t mods, void* context);
keymap_result apply_keymap(std::span<const addon::keymap_binding> bindings,
                           host_owns_key host_owns = nullptr, void* context = nullptr);

// What applying did, for Settings on both hosts to say:
// {"changed": n, "displaced": n, "skipped": ["...", ...]}.
[[nodiscard]] std::string keymap_report_json(const keymap_result& r);

// {"addon", "id", "name", "bindings": [...]} (addon/open_json.h
// open_keymap_json) to its bindings; false when it is not that shape.
[[nodiscard]] bool keymap_bindings_from_json(std::string_view json_text,
                                             std::vector<addon::keymap_binding>& out);

}  // namespace mv::shell
