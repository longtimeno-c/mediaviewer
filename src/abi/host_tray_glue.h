// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/self-hosted.md "Tray": the app's one host watcher
// (addons/phone/host_tray.h) glued to the add-on registry's shell keep (slots.h), so
// both hosts' existing background code (#452) draws the host's rows and keeps
// the process while `driftwire host` runs. Started in the first window's
// process (Windows: mv_host_tray_start from IslandHost.Addons.cs; the Mac:
// main_mac.mm once it holds the add-on host), stopped at quit before the
// add-ons unload. The base app's own, with or without the Phone access add-on. Any thread;
// neither waits on I/O (stop joins the watcher's thread, which waits on none
// but a verb it is running: at most that verb's 30 s, only at quit).
#pragma once

namespace mv::abi {

void start_host_tray();
void stop_host_tray() noexcept;

}  // namespace mv::abi
