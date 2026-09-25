// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// First-install helper for the macOS disk image (plan/13 "macOS first install").
#pragma once

#include <string>

namespace mv::shell {

// If this app bundle is running from a mounted disk image, offer to copy it to
// /Applications, relaunch the copy and eject the image. Returns true when the
// copy has been launched and this process should exit at once; false means keep
// starting normally (not on a disk image, user declined, or the copy failed).
// Call on the main thread after NSApplication exists, before any window.
bool offer_install_from_disk_image() noexcept;

// What the first-launch setup sheet can tidy up after a drag install (plan/13):
// our installer disk still mounted, and/or the .dmg it came from. Local paths,
// shown in the sheet and never logged (rule 6).
struct installer_leftover {
  std::string mount;  // mount point; empty once the image is ejected
  std::string image;  // the .dmg file; empty when it cannot be found
  [[nodiscard]] bool any() const noexcept { return !mount.empty() || !image.empty(); }
};

// Looks for a leftover on a utility queue (hdiutil never runs on the main
// thread); `done` runs on the main queue. Nothing is found while this copy is
// itself running from, or translocated off, a disk image.
void find_installer_leftover(void (^done)(installer_leftover found));

// Ejects the image (when mounted), then moves the .dmg to the Trash, on a
// utility queue; `done(ok)` on the main queue. Nothing is deleted outright.
void clean_up_installer(installer_leftover what, void (^done)(bool ok));

// The sheet was answered without the clean-up: forget the remembered image.
void forget_installer_leftover();

}  // namespace mv::shell
