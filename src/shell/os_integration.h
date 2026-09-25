// SPDX-License-Identifier: GPL-2.0-or-later
// PR 15 (plan/10 "OS integration", plan/16 View): the portable halves of the
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

// Ctrl+Shift+C / ⌘⇧C: the paths as text, one per line, in the order given,
// separated by `newline` ("\r\n" on Windows, "\n" on macOS) with none after
// the last. Unquoted, so a single path pastes straight into a path box.
[[nodiscard]] std::string paths_as_text(std::span<const std::string> utf8_paths,
                                        std::string_view newline);

}  // namespace mv::shell
