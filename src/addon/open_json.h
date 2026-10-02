// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Open add-ons as the chromes see them (plan/23): one JSON shape for both
// hosts, so the WinUI and SwiftUI consent sheets are fed the same fields and
// cannot drift (D9). The Windows ABI (abi/addon_abi.cpp) and the Mac host
// (shell/addons_mac.mm) are thin wrappers over these.
//
// Every call reads files: worker threads only. Nothing here creates the open
// add-ons folder except a successful install.
#pragma once

#include <string>
#include <string_view>

#include "addon/open_store.h"
#include "core/result.h"

namespace mv::addon {

// What a package is, for the consent sheet. Always JSON, never an error:
//   {"ok", "why", "detail", "message", "sha256", "relation",
//    "installed_version",
//    "adds", "can": [...], "cannot",
//    "id", "name", "version", "description", "licence", "size", "update_url",
//    "publisher": {"name", "url", "key", "fingerprint"},
//    "api": {"min", "max"}, "themes": [{"id", "name"}]}
// The add-on's fields are present whenever the publisher's signature held.
// "message" is what to tell the person when "ok" is false, in words; "adds",
// "can" and "cannot" are the sheet's lines, worked out from the manifest and
// never taken from what its maker wrote. One wording for both hosts.
[[nodiscard]] std::string open_inspect_json(const open_store& store,
                                            std::string_view package_path_utf8);

// Installs the package the user agreed to (`approved_sha256` is the inspect
// result's "sha256"): {"ok", "why", "message", "id", "version"}.
[[nodiscard]] std::string open_install_json(const open_store& store,
                                            std::string_view package_path_utf8,
                                            std::string_view approved_sha256);

// [{"folder", "id", "name", "version", "state": "ok|needs_update|invalid", "why",
//   "description", "licence", "size", "update_url", "publisher": {...},
//   "themes": [{"id", "name"}]}]. "[]" when none, or no folder.
[[nodiscard]] std::string open_list_json(const open_store& store);

// The store over io::open_addons_dir().
[[nodiscard]] result<open_store> default_open_store();

}  // namespace mv::addon
