// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Keymap packs: other keys for MediaViewer's own commands (docs/design/25
// "Keymap", contribution API 2; docs/design/16 "Keymap packs"). The v1.1
// "named layouts" (FastStone, IrfanView, vim) are packs.
//
//   "keymaps": [
//     { "id": "vim", "name": "Vim",
//       "bindings": [
//         { "command": "next", "keys": ["L", "Right"] },
//         { "command": "prev", "keys": ["H", "Left"] },
//         { "command": "toggle_mark", "keys": [""] },          // unbound
//         { "command": "acme.tools/count-files", "keys": ["Ctrl+Shift+K"] } ] } ]
//
// `command` is a command's stable name (shell/keymap.h command_slug), or
// "<add-on id>/<command id>" for a row an add-on contributes. `keys` are
// key labels ("Ctrl+Shift+O", "Page Down", "?"; Ctrl reads as Cmd on a Mac)
// given in turn to that command's rows in the default table; "" leaves a row
// without a key, and a row past the end of `keys` keeps its default.
//
// This file checks the SHAPE only: names and labels as bounded text. Which
// command a name means, and whether a label is a key, is the host's table's
// to say when the pack is applied (shell/keymap.h), so a pack written for a
// newer MediaViewer applies what this one knows and says what it skipped.
// Portable (D9).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.h"

namespace mv::addon {

inline constexpr std::size_t kKeymapMaxPacks = 8;
inline constexpr std::size_t kKeymapMaxBindings = 256;
inline constexpr std::size_t kKeymapMaxKeys = 8;
// One pack, as keymaps_to_json writes it ({"id","name","bindings"}, compact,
// '"' and '\' escaped), is at most this many bytes. The counts above allow
// about 170 KB; this is what both hosts carry (issue #356): Windows moves the
// pack through the chrome's 128 KB parked-text buffer (shell/chrome_host.cpp,
// which static_asserts the room), the Mac by pointer. The SDK checks the same
// (tools/addon-sdk/mvaddon.py KEYMAP_MAX_PACK_BYTES).
inline constexpr std::size_t kKeymapMaxPackBytes = 64u << 10;

struct keymap_binding {
  std::string command;
  std::vector<std::string> keys;
};

struct keymap_pack {
  std::string id;    // [a-z0-9-]{1,32}, unique in the add-on
  std::string name;  // shown in Settings
  std::vector<keymap_binding> bindings;
};

// `contributes.keymaps`. nullopt when it breaks any rule; `detail` names it.
[[nodiscard]] std::optional<std::vector<keymap_pack>> parse_keymaps(const json::value& v,
                                                                    std::string* detail = nullptr);
[[nodiscard]] std::optional<std::vector<keymap_pack>> parse_keymaps_text(
    std::string_view json_text, std::string* detail = nullptr);

// The packs as the chromes and the shell read them back:
// [{"id","name","bindings":[{"command","keys":[...]}]}].
[[nodiscard]] std::string keymaps_to_json(const std::vector<keymap_pack>& packs);
// One pack's bytes in that JSON (kKeymapMaxPackBytes bounds it).
[[nodiscard]] std::size_t keymap_pack_json_bytes(const keymap_pack& pack);

}  // namespace mv::addon
