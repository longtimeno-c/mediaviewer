// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Manifest schema 2: an add-on from anyone (docs/design/25 "The manifest").
//
// Schema 1 (manifest.h) is MediaViewer's own add-ons, signed with the pinned
// release key. Schema 2 is signed by its PUBLISHER, whose public key is a
// field of the manifest itself. That signature says two things and no more:
// the files are the ones the publisher packed, and a later version comes from
// the same publisher. It does not say who the publisher is, and nothing here
// pretends it does; the person installing is shown the name and the key's
// fingerprint and decides (open_store.h).
//
// What a schema 2 add-on may hold is set by the contribution API it declares
// (`api`), not by what the package happens to carry. API 1 and 2 are data
// only. API 3 (PR 57) adds code: Lua text files (`scripts`), run by the
// sandboxed runtime (script_runtime.h), the permissions they ask for, and the
// commands they contribute. Native code (`native`, `chrome`, `main`) is
// refused by name, whatever the range: other makers' code runs only as script.
// Portable (D9).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "addon/keymap.h"
#include "addon/manifest.h"
#include "addon/settings_schema.h"

namespace mv::addon {

inline constexpr int kOpenManifestSchema = 2;

// The contribution API this host serves (docs/design/25 "Contribution API").
//   1  themes.
//   2  settings pages, keymap packs, theme shape (PR 56).
//   3  scripts, permissions, commands (PR 57).
//   4  screens; 5  chrome slots and search providers (PRs 58-59, 2026-10-11).
//      An open add-on cannot fill screens or slots yet: they are filled by
//      code, and scripts have no binding for them (code_not_allowed).
//   6  2026-10-19: the screen vocabulary's `link` node and a button's `copy`
//      (Driftwire). No new point: a screen tree that uses them asks for 6,
//      so an older host says "needs a newer MediaViewer", not a blank window.
//   7  2026-10-20 (#512): `opens` on a status item. An add-on that names one
//      asks for 7; an older host refused it as malformed.
//   8  2026-10-21: a screen in place "settings", drawn on the add-on's page in
//      Settings -> Add-ons (Driftwire's settings). Schema 1 only, as screens.
//   9  2026-10-28: `reveal` on a screen stack (Driftwire's transfer card): the
//      host scrolls it into view and marks it. A tree that uses it asks for 9.
inline constexpr std::uint32_t kContributionApi = 9;
// The API that introduced code (`scripts`, `permissions`, commands).
inline constexpr std::uint32_t kScriptApi = 3;
inline constexpr std::uint32_t kContributionApiOldest = 1;

// Every contribution point docs/design/25 §7 names, with the API that
// introduces it. One table, the same on every branch: a host parses the points
// it has built (open_manifest.cpp) and holds the rest to the strictness rule
// (a manifest that claims no API newer than the host may not name a point the
// host does not serve). Add a row here when §7 gains a point.
struct contribution_point {
  std::string_view key;  // the key under `contributes`
  std::uint32_t api;     // the contribution API that introduces it
};
inline constexpr std::array<contribution_point, 12> kContributionPoints{{
    {"themes", 1},
    {"settings", 2},
    {"keymaps", 2},
    {"commands", 3},
    {"screens", 4},
    {"info_sections", 5},
    {"search_providers", 5},
    {"bar_items", 5},
    {"path_items", 5},
    {"context_menu", 5},
    {"status_items", 5},
    {"tile_badges", 5},
}};

// The API that introduces `key` under `contributes`, or 0 if §7 names no
// such point.
[[nodiscard]] constexpr std::uint32_t contribution_api(std::string_view key) noexcept {
  for (const contribution_point& p : kContributionPoints) {
    if (p.key == key) return p.api;
  }
  return 0;
}

struct theme_ref {
  std::string id;    // unique in the add-on: [a-z0-9-]{1,32}
  std::string name;  // shown in Settings
  std::string path;  // one of `files`, a theme JSON (theme.h)
};

// What a script may reach, each a line in the consent sheet and none granted
// by default (docs/design/25 §9). Without a permission its API is absent from
// the script's environment, not merely refused. There is no network
// permission, and no socket call exists to grant. `files.write` and
// `metadata.write` are PR 60's: a manifest naming them needs a newer host.
enum permission : std::uint32_t {
  perm_folder_read = 1u << 0,  // "folder.read": the open folder's files, by name
  perm_thumbnails = 1u << 1,   // "thumbnails": thumbnail nodes in screens (PR 58)
  perm_selection = 1u << 2,    // "selection": the file on screen, and marks with a command
};
inline constexpr std::uint32_t kPermissionsAll = perm_folder_read | perm_thumbnails | perm_selection;
// "folder.read" for perm_folder_read; null for anything else.
[[nodiscard]] const char* permission_name(std::uint32_t bit) noexcept;
// The bit for a name this host grants; 0 for anything else.
[[nodiscard]] std::uint32_t permission_from_name(std::string_view name) noexcept;

// A command a script contributes (contribution point "commands", API 3).
struct script_command {
  std::string id;       // unique in the add-on: [a-z0-9-]{1,32}
  std::string name;     // the row's name in `?`, Settings and the menu
  std::string key;      // "Ctrl+Shift+K" (Ctrl is Cmd on the Mac); empty: no key
  std::string run;      // the script's global function it calls
  std::string modes;    // "viewing" (default) | "video" | "browse" | "all"
  std::string payload;  // "none" (default) | "marks" | "marked_or_current" | "screen"
};

struct open_manifest {
  std::string id;           // "acme.film-tones": publisher.name, dotted
  std::string name;
  std::string version;      // x.y.z
  std::string description;  // one line for the consent sheet and Settings; may be empty
  std::string licence;      // SPDX of the add-on as a whole
  std::string publisher_name;
  std::string publisher_url;  // https, or empty
  std::string publisher_key;  // 64 lowercase hex: the Ed25519 public key that signed this
  std::string update_url;     // https link to the newest package, or empty
  std::uint32_t api_min = 0;
  std::uint32_t api_max = 0;
  std::uint64_t installed_size = 0;  // the sum of `files`
  std::vector<manifest_file> files;
  std::vector<theme_ref> themes;
  settings_page settings;            // API 2; empty when the add-on has no page
  std::vector<keymap_pack> keymaps;  // API 2
  // API 3: code. Empty `scripts` is a data add-on.
  std::vector<std::string> scripts;  // .lua paths of `files`, run in this order
  std::uint32_t permissions = 0;     // permission bits
  std::vector<script_command> commands;
  [[nodiscard]] bool runs_code() const noexcept { return !scripts.empty(); }
};

struct open_decision {
  rejection why = rejection::malformed;
  open_manifest m;
  [[nodiscard]] bool ok() const noexcept { return why == rejection::none; }
};

// Shape, then the publisher's signature over the exact bytes, then policy.
// A manifest whose range starts above `api` is needs_update with `m` filled,
// so the app can say which add-on needs a newer MediaViewer.
[[nodiscard]] open_decision check_open_manifest(std::span<const std::uint8_t> manifest_bytes,
                                                std::span<const std::uint8_t> signature,
                                                std::uint32_t api = kContributionApi,
                                                std::uint32_t api_oldest = kContributionApiOldest);

// "publisher.name": two or more dot-separated parts of [a-z0-9-], each
// starting and ending with a letter or digit, 64 characters at most. The
// dot keeps these apart from MediaViewer's own ids ("import", "ai-faces"),
// `mediaviewer.` is kept for MediaViewer, and no part is a name Windows
// reserves for a device (the id is a folder on disk).
[[nodiscard]] bool valid_open_id(std::string_view id) noexcept;

// An https URL with a host and nothing a person could not read: no
// credentials, no control characters, no spaces, 2048 characters at most.
[[nodiscard]] bool valid_https_url(std::string_view url) noexcept;

// A key as people compare it: the first 8 bytes of SHA-256(key), as four
// groups of four hex digits ("3f9a-02c1-77de-b410").
[[nodiscard]] std::string key_fingerprint(std::string_view key_hex);

}  // namespace mv::addon
