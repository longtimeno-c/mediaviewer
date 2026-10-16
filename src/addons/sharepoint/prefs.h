// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// What Sharepoint keeps across launches (issue #442): whether this machine
// keeps receiving with the window closed (and with MediaViewer closed, #452),
// and the folder Save to last chose.
// A small JSON file in the add-on's own data folder (the host table's
// data_dir), never the app's settings: Remove without keep takes it with the
// add-on.
//
// Written whole on a worker (the engine's control thread), crash-safe without
// a replacing rename: the new text goes to "<file>.new" (flushed), the old file
// is removed, then the new one is renamed into place with no-replace; a read
// that finds no file takes "<file>.new". Portable (D9): the file port only.
#pragma once

#include <optional>
#include <string>

namespace mv::sharepoint {

struct prefs {
  bool receiving = false;   // "Keep receiving when this window is closed"
  // "Receive when MediaViewer is closed" (#452): the process stays (menu-bar
  // item / tray icon) with every window closed. Implies `receiving`.
  bool background = false;
  std::string destination;  // Save to; "" = the default (Downloads)
  bool operator==(const prefs&) const = default;
};

inline constexpr const char* kPrefsFile = "sharepoint.json";

// The prefs at `path`; nullopt when there is none or it does not parse (the
// defaults then apply). One small read.
[[nodiscard]] std::optional<prefs> load_prefs(const std::string& path);
// Writes them; false on any I/O error (the old file stays).
[[nodiscard]] bool save_prefs(const std::string& path, const prefs& p);

}  // namespace mv::sharepoint
