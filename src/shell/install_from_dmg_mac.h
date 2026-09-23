// SPDX-License-Identifier: GPL-2.0-or-later
// First-install helper for the macOS disk image (plan/13 "macOS first install").
#pragma once

namespace mv::shell {

// If this app bundle is running from a mounted disk image, offer to copy it to
// /Applications, relaunch the copy and eject the image. Returns true when the
// copy has been launched and this process should exit at once; false means keep
// starting normally (not on a disk image, user declined, or the copy failed).
// Call on the main thread after NSApplication exists, before any window.
bool offer_install_from_disk_image() noexcept;

}  // namespace mv::shell
