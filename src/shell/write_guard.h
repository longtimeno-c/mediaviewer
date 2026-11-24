// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Files the viewer shows but must never change (rule 5, issue #72): anything
// inside a Photos library bundle (`*.photoslibrary/`), which Local search now
// opens in place, read-only, and any file a list opener registered as
// read-only (the Photos previews and on-view iCloud downloads in the AI
// chrome's cache). Every host action that writes to, renames, moves or trashes
// a file, or saves a new file beside it, asks write_protected() first and
// refuses with a notice. Reading is never affected.
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace mv::shell {

// Inside a Photos library bundle, a Photos item key ("photos:<id>",
// docs/design/26), a remote folder's item (is_remote), under the registered
// prefix, or registered read-only. Any thread.
[[nodiscard]] bool write_protected(std::string_view path);
// Any of these.
[[nodiscard]] bool any_write_protected(std::span<const std::string> paths);
// Replaces the registered set (a list opener's files; empty clears). Any thread.
void set_read_only_paths(std::span<const std::string> paths);
// A folder whose every file is read-only (docs/design/26: the Photos previews and
// on-view downloads, ~/Library/Caches/MediaViewer/Photos Library). Empty
// clears. Any thread.
void set_read_only_prefix(std::string_view dir);

// docs/plans/desktop-client.md: a remote folder's key ("remote:...") or a file
// in its cache (its originals fetched here). Any thread.
[[nodiscard]] bool is_remote(std::string_view path);
void set_remote_cache_prefix(std::string_view dir);

// docs/plans/desktop-client.md §6: the file changes another computer's service does for a
// device it lets "change" -- nothing else. A rating, a metadata edit, an edit of an original
// or a sidecar beside it stays refused on a remote file (write_protected stays true for it).
enum class file_change : std::uint8_t { add, make_folder, move, rename, remove };
// A local path not write_protected, or a `remote:` key the registered check says this device
// may change now (the server's role and that folder's writable). Never a file in the remote
// cache, a Photos item or a read-only list's file. Any thread; never blocks.
[[nodiscard]] bool change_allowed(std::string_view path, file_change op);
// The check (the host's remote library's can_write); null: no remote key may be changed.
void set_remote_writable(std::function<bool(std::string_view key)> check);

// What a refused action says.
inline constexpr const char* kWriteProtectedNotice =
    "From your Photos library: read-only here. Open it in Photos to change it.";
inline constexpr const char* kRemoteWriteProtectedNotice =
    "On another computer: read-only from here. Copy it to this computer to change it.";
// The one of the two that fits `path`.
[[nodiscard]] const char* write_protected_notice(std::string_view path);

}  // namespace mv::shell
