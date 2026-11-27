// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Where a shared folder's drive is, for `driftwire host` (docs/plans/self-hosted.md
// §6, §11): a PC's drives come and go, and a folder on one must read offline
// while its drive is not there, never as an empty folder on the system disk.
// A port (D9): volume_posix.cpp (Linux, macOS), volume_win.cpp.
//
//   POSIX    a folder that is a mount point when it is shared (on another
//            device than its parent) is kept with `mount`; while it is not one
//            (unmounted, left as an empty folder) it is not there. Its path
//            never changes.
//   Windows  a folder is kept by its volume's GUID path as well as its path;
//            while the letter names another volume, or none, it is not there,
//            and when the volume comes back under another letter (E: became F:)
//            the folder is found there again.
//
// Each call is a stat or two (GetVolume* on Windows): asked when a folder is
// shared, by the service where it asks whether a folder is there (roots, a
// failed request, a write; serve::server::options::root_present), and by the
// host's minute recheck. Never on anything that renders.
#pragma once

#include <optional>
#include <string>

#include "wire/serve_wire.h"

namespace mv::driftwire::cli {

struct volume_facts {
  bool mount = false;  // POSIX: the folder is a mount point now
  std::string volume;  // Windows: "\\?\Volume{...}\" of the folder's volume ("" when unknown)
};

// What to keep with a folder being shared (an absolute path, UTF-8).
[[nodiscard]] volume_facts volume_of(const std::string& path);

// Whether the folder's drive is the one it was shared on, and there. A folder
// kept with neither `mount` nor `volume` always is (the folder alone decides).
[[nodiscard]] bool volume_present(const serve::root_config& root);

// Where the folder is now: its own path while its drive is there; on Windows
// the same folder under the volume's new letter; nullopt when its volume is
// not mounted anywhere.
[[nodiscard]] std::optional<std::string> volume_locate(const serve::root_config& root);

}  // namespace mv::driftwire::cli
