// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Mac host's add-on entry points for main_mac.mm (addons_mac.mm).
// Main thread only.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

using MvAddonsOpenPathFn = void (*)(void* ctx, const char* utf8_path);

// Once at launch: verify and load the installed add-ons (Import; on Apple
// silicon the AI pack) off the main thread, and watch for cards to offer the
// one-time Import hint when it is absent.
void MvAddonsStart(MvAddonsOpenPathFn open_path, void* ctx);
// Ctrl+Shift+I / ⌘⇧I: the Import window, with the viewer's marks.
void MvAddonsOpenImport(const std::vector<std::string>& marks);
// ⌘⇧F7: import these now with the last preset.
void MvAddonsImportNow(const std::vector<std::string>& paths);

// Milestone H (plan/17): the AI chrome follows the viewer. A directory was
// opened (never a result list); the item on the canvas changed ("" = none).
// Both are cheap message sends; with the AI pack absent they do nothing.
void MvAddonsFolderOpened(const std::string& dir);
void MvAddonsItemChanged(const std::string& path);
// search_open / search_similar / search_next_match / search_prev_match, from
// the command router. False when the AI chrome is not loaded or declined.
bool MvAddonsRunCommand(const char* name);
// Quit, on the main thread: each chrome shuts down first (it stops taking
// table calls and drains those in flight), then the add-ons stop off the main
// thread. MvAddonsWaitStopped (a worker, before the process exits) waits for
// that, up to `seconds`: a pack must not be torn down by static destruction
// while a chrome or a worker of it still runs.
void MvAddonsQuit();
void MvAddonsWaitStopped(double seconds);

// Defined in main_mac.mm: an exact seek on the clip on screen, only if the
// render thread has adopted it -- while the selected clip is still opening the
// live one is the previous clip, and the seek is dropped (false).
bool MvViewerSeekShownClip(int64_t position_ms);
