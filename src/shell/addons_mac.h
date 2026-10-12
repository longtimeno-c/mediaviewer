// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Mac host's add-on entry points for main_mac.mm (addons_mac.mm).
// Main thread only.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

using MvAddonsOpenPathFn = void (*)(void* ctx, const char* utf8_path);

// Once at launch: verify and load Import, and verify the AI pack (Apple
// silicon) without loading it (issue #292: it starts at the first search or
// when Settings shows it), off the main thread; watch for cards to offer the
// one-time Import hint when Import is absent.
void MvAddonsStart(MvAddonsOpenPathFn open_path, void* ctx);
// Instead of MvAddonsStart, in a window whose process did not get the
// add-on host lock (main_mac.mm MvClaimAddonHost): Local search through the AI
// pack's read-only reader, nothing else. Loads nothing at launch: the reader
// starts on the first ⌘F (MvAddonsSearchOnDemand) or when Settings opens.
void MvAddonsStartReader(MvAddonsOpenPathFn open_path, void* ctx);
// ⌘F while Local search is not attached. True when it took the key: the pack
// (the first window, issue #292) or the reader (a later window) is starting
// and the panel opens once it attaches. In a later window, if nothing is
// indexed yet or it is not installed, file search opens instead, with a note
// for the former. False: do what ⌘F does without Local search (not
// installed; or a reader tried less than 10 s ago, the note then in the bar).
bool MvAddonsSearchOnDemand();
// Ctrl+Shift+I / ⌘⇧I: the Import window, with the viewer's marks.
void MvAddonsOpenImport(const std::vector<std::string>& marks);
// ⌘⇧F7: import these now with the last preset.
void MvAddonsImportNow(const std::vector<std::string>& paths);

// Milestone H (docs/design/17): the AI chrome follows the viewer. A directory was
// opened (never a result list); the item on the canvas changed ("" = none).
// Both are cheap message sends; with the AI pack absent they do nothing.
void MvAddonsFolderOpened(const std::string& dir);
void MvAddonsItemChanged(const std::string& path);
// search_open / search_similar / search_next_match / search_prev_match, from
// the command router. False when the AI chrome is not loaded or declined.
bool MvAddonsRunCommand(const char* name);
// docs/design/25 (2026-10-03): a command an add-on's manifest contributed
// (shell/commands.h addon_command_row), with the payload the row asked for as
// JSON. The add-on's chrome runs it; an older Import chrome without the
// generic entry gets its two frozen selectors. False when nothing ran.
bool MvAddonsRunContributedCommand(const std::string& addon, const std::string& id,
                                   const std::string& payload_json);
// Quit, on the main thread, never waiting on a pack (addon/host.h "Quit"):
// each chrome closes its table (the AI chrome without waiting out a slow
// read), and each add-on's stop starts on a thread of its own -- or, if its
// chrome may still be inside its table, it is left running. Quit never
// unloads a pack. MvAddonsWaitStopped (a worker, before the process exits)
// gives them until `seconds` after MvAddonsQuit. Whatever is still running
// then (a model load, a Core ML compile, a verify on the add-on queue) is left
// to the exit: an exit guard registered by MvAddonsQuit ends the process
// without static destructors, which those threads could still be using.
void MvAddonsQuit();
void MvAddonsWaitStopped(double seconds);

// docs/design/25 PRs 58-59: MediaViewer's own add-ons that fill screens and slots
// (an info-pane section, a search provider, items, badges, windows). Start
// loads the installed ones on the add-on queue (MvAddonsStart calls it after
// Import's and the AI pack's checks); Quit stops them, their items first.
void MvSlotAddonsStart();
void MvSlotAddonsQuit();
// [worker-thread] The viewer's JPEG-512 thumbnail of any still (cache hit, or made
// now on the calling thread), for a screen's thumbnail and thumbnail_grid nodes.
// "" when it has none (a clip, an unreadable file).
std::string MvAddonsThumbnailPath(const std::string& path);

// Defined in main_mac.mm: an exact seek on the clip on screen, only if the
// render thread has adopted it -- while the selected clip is still opening the
// live one is the previous clip, and the seek is dropped (false).
bool MvViewerSeekShownClip(int64_t position_ms);
