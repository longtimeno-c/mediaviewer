// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Mac's half of a paired server's LAN share in the Finder
// (remote/lan_share.h, docs/plans/desktop-client.md §6a): what SMB volumes are
// mounted, and NetFS mounts and unmounts. Nothing here runs on the main
// thread's time: mounts are NetFSMountURLAsync on its own queue, unmounts
// NSFileManager's asynchronous one, and the volume list a non-waiting
// getfsstat. No password passes through: with `allow_ui` the OS's own sign-in
// asks and its "Remember this password in my keychain" keeps it; without, the
// OS uses what the Keychain has, or answers EAUTH. Nothing is logged (rule 6).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "remote/lan_share.h"

namespace mv::shell::lan_share_mac {

struct volume {
  remote::lan_share::mounted from;
  std::string path;  // where it is mounted ("/Volumes/Photos")
};

// SMB volumes mounted now (MNT_NOWAIT: never waits on a server). Any thread.
[[nodiscard]] std::vector<volume> smb_volumes();
[[nodiscard]] std::vector<remote::lan_share::mounted> mounted_now();

// Whether a NetFS status means the OS had no sign-in it could use (EAUTH, a
// cancelled or refused dialog, a password the server wants changed).
[[nodiscard]] bool needs_sign_in(int netfs_status) noexcept;

// Mounts each URL, one after another, through NetFS. `done` runs on the main
// queue with how it went (outcome.asked = allow_ui). An already-mounted share
// (EEXIST) counts as mounted.
void mount(std::vector<std::string> urls, bool allow_ui, std::function<void(remote::lan_share::outcome)> done);

// Unmounts every SMB volume of `host` (not ejecting anything else). `done` on
// the main queue: how many went and how many would not (a file still open).
void unmount(std::string host, std::function<void(int unmounted, int failed)> done);

}  // namespace mv::shell::lan_share_mac
