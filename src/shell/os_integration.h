// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 15 (docs/design/10 "OS integration", docs/design/16 View): the portable halves of the
// shell verbs each host wires to its OS. Recent folders feed the Windows jump
// list and the macOS Dock menu; the path text is what Ctrl+Shift+C / ⌘⇧C puts
// on the clipboard; the flattened-copy name is what Ctrl+Alt+C / ⌘⌥C writes.
// Pure C++, no platform header, so both hosts and the tests share one rule.
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "shell/welcome_layout.h"

namespace mv::shell {

// The jump list's and the Dock menu's "Recent folders", most recent first.
// Ten: the length of the Windows jump list's default recent category.
inline constexpr std::size_t kMaxRecentFolders = 10;

// Pure: the list after `utf8_dir` was opened. Moves it to the front, drops an
// earlier spelling of the same folder (ASCII case and a trailing separator are
// ignored, and `/` equals `\`: NTFS and APFS both compare case-insensitively
// by default), and keeps at most `max`. An empty `utf8_dir` changes nothing.
[[nodiscard]] std::vector<std::string> push_recent_folder(std::vector<std::string> list,
                                                          std::string_view utf8_dir,
                                                          std::size_t max = kMaxRecentFolders);

// The label a recent folder shows: its last component ("2026-09 Iceland"),
// or the path itself for a root ("D:\", "/"). Separators may be either kind.
[[nodiscard]] std::string folder_display_name(std::string_view utf8_dir);

// The labels a list of recent folders shows, in order: each one's display
// name, and where two share it, " — <parent>" on each of those, so "DCIM" on
// one card is not "DCIM" on another.
[[nodiscard]] std::vector<std::string> recent_folder_labels(std::span<const std::string> utf8_dirs);

// The welcome card's rows for `utf8_dirs` (most recent first; the first
// welcome_recents::kMax): each folder's display name, and where it lives -- its
// parent, with `home` (the user's home folder, "" for none) written as "~".
// Text is cut at a UTF-8 boundary to fit. The hover is cleared. `icloud`
// (the Mac host, while the Photos library is added: docs/design/26) puts an
// "iCloud Photos" row first and the folders after it, still kMax rows in all.
void fill_welcome_recents(std::span<const std::string> utf8_dirs, std::string_view home,
                          welcome_recents& out, bool icloud = false) noexcept;
// The same with any cloud place leading (Windows: "OneDrive", 2026-10-07);
// an empty `cloud_label` is none.
void fill_welcome_recents(std::span<const std::string> utf8_dirs, std::string_view home,
                          welcome_recents& out, std::string_view cloud_label,
                          std::string_view cloud_where) noexcept;

// Ctrl+Shift+C / ⌘⇧C: the paths as text, one per line, in the order given,
// separated by `newline` ("\r\n" on Windows, "\n" on macOS) with none after
// the last. Unquoted, so a single path pastes straight into a path box.
[[nodiscard]] std::string paths_as_text(std::span<const std::string> utf8_paths,
                                        std::string_view newline);

// ---- The Explorer thumbnail handler's install (Windows; the rule is pure) ----
// The handler runs from <root>\shellext\<version>\, a copy made by the app,
// never from current\: a surrogate can hold the DLL for minutes after
// Explorer last asked, and an update must not have to replace it.

// MediaViewerThumbs.files: one file name per line (the handler, then the DLLs
// it loads). Blank lines and a trailing CR are ignored. A name with a path
// separator, a drive, or "." / ".." is refused, and so is the whole list:
// the copy never leaves the folders it was given.
[[nodiscard]] std::vector<std::string> parse_shellext_file_list(std::string_view text);

struct shellext_plan {
  std::string version_dir;          // the folder name under <root>\shellext
  std::vector<std::string> prune;   // older folders to try to remove
};
// `version` must be a plain version string (letters, digits, `.`, `-`, `+`);
// anything else is no plan (empty version_dir). `existing` is what
// <root>\shellext holds now; every other entry there is pruned.
[[nodiscard]] shellext_plan plan_shellext_install(std::string_view version,
                                                  std::span<const std::string> existing);

}  // namespace mv::shell
