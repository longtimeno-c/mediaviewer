// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Photos library as items the viewer can list (plan/26-photos-library.md,
// the Mac half of issue #72's follow-up). A library asset has no path: it is
// a VIRTUAL item keyed "photos:<localIdentifier>" (the key the Local search
// pack already uses), listed through folder_model::open_list as a virtual
// entry and resolved to a real file only when it is about to be shown.
//
// What this gives the host, all PhotoKit, all on worker threads (rule 1):
//   enumerate   every asset of the user's own library (iCloud Photos
//               included), oldest first, as the Photos app lists them;
//   describe    the same for a given set of keys (a search result list);
//   thumb_jpeg  a tile from PhotoKit's own cached rendition, never the network;
//   resolve     where Photos keeps the current rendition (read in place,
//               never written: shell/write_guard.h), or, when only iCloud
//               has it, a "(preview).jpg" of the best picture this Mac holds;
//               with want_original the original is fetched from iCloud into
//               the cache folder. That fetch is the ONE network request, and
//               it is made only for the item the user is viewing (owner,
//               2026-09-28: "when viewing but cleared after") or copying out.
//   clear_*     the cache folder: downloads go when the list closes and at
//               quit; everything goes when the app starts.
//
// The library counts as "added" once the user added it in Settings (Local
// search -> Add Photos Library sets the flag; Remove clears it): the folder
// row, the menu item and the backup tool show only then (added()). Nothing
// here ever raises the permission prompt: with access not granted every call
// answers permission_denied without touching the library.
//
// Identifiers are paths: never logged, never in a crash report (rule 6).
#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace mv::shell::photos {

inline constexpr std::string_view kKeyPrefix = "photos:";
inline constexpr const char* kAddedDefault = "mv.photosLibrary.added";
inline constexpr const char* kListTitle = "Photos Library";

[[nodiscard]] inline bool is_key(std::string_view path) noexcept {
  return path.size() > kKeyPrefix.size() && path.substr(0, kKeyPrefix.size()) == kKeyPrefix;
}

// Settings' "added" flag (NSUserDefaults, shared with the pack's chrome).
[[nodiscard]] bool added() noexcept;
void set_added(bool on) noexcept;
// PhotoKit access is authorized or limited. Never prompts. [any-thread]
[[nodiscard]] bool readable() noexcept;
// added() && readable(): the folder, the menu item and the backup show.
[[nodiscard]] bool available() noexcept;

struct item {
  std::string key;        // "photos:" + localIdentifier
  std::string name;       // the original file name (IMG_0412.HEIC); kind by extension
  std::int64_t mtime = 0; // modificationDate (an edit in Photos re-stamps it)
  std::uint64_t size = 0; // pixelWidth * pixelHeight (the pack's stamp; no byte size without a read)
  std::int64_t created = 0;
  bool video = false;
};

// [worker] The user's own library: iCloud Photos and imports, not shared
// albums, not the Hidden album, one per burst and per Live Photo. Oldest
// first (the Photos app's Library order). permission_denied without access.
[[nodiscard]] result<std::vector<item>> enumerate();

// [worker] The items for `keys`, in the keys' order; nullopt for an asset
// that is gone. One PhotoKit fetch for the lot.
[[nodiscard]] result<std::vector<std::optional<item>>> describe(std::span<const std::string> keys);

// [worker] A JPEG tile (<= max_edge on the long side, image/thumb.h's
// encoder) from PhotoKit's own cached rendition. Network off: an asset only
// iCloud has still has a small picture here, which is returned; not_found
// when the asset is gone, status::io when nothing at all is on this Mac.
[[nodiscard]] result<std::vector<std::uint8_t>> thumb_jpeg(std::string_view key, std::uint32_t max_edge);

struct resolved {
  std::string path;      // what the viewer opens
  bool preview = false;  // a cache JPEG standing in for an iCloud-only original
  bool downloaded = false;  // the original, fetched from iCloud into the cache
  // The file's stamp (one stat, on the worker): the lab's still cache is keyed
  // by it, so a revisit is served from VRAM like any file's.
  std::int64_t mtime_unix = 0;
  std::uint64_t size = 0;
};

// [worker] The file for `key`. The current rendition where Photos keeps it
// (in place) when this Mac has it. Otherwise, with want_original the
// original is fetched from iCloud (the one network request; `cancel` stops
// it, leaving nothing); without it the best local picture is written once
// as "<name> (preview).jpg" and returned as a preview. not_found: gone.
[[nodiscard]] result<resolved> resolve(std::string_view key, bool want_original,
                                       const std::atomic<bool>* cancel = nullptr);

// ---- For the backup (shell/photos_backup.h; plan/26 "Backup"), all [worker].
// `kind`: 0 the original photo or video as shot, 1 a Live Photo's paired
// video, 2 a RAW+JPEG pair's RAW (backup::file_kind).
struct library_file {
  std::string key;
  int kind = 0;
  std::string name;  // the original file name
  std::int64_t created = 0;
};
// Every original file of every asset the user owns, the Hidden album
// included (a backup keeps what the user hid, too). Oldest first.
[[nodiscard]] result<std::vector<library_file>> enumerate_files(const std::atomic<bool>* cancel);
// The original's file on this Mac, in place, when it is here; "" when not.
[[nodiscard]] result<std::string> local_original(std::string_view key, int kind);
// Writes the original's bytes to `dest_utf8`, from this Mac or from iCloud
// (network allowed: the user asked for a copy). A cancel leaves nothing.
[[nodiscard]] expected fetch_file(std::string_view key, int kind, std::string_view dest_utf8,
                                  const std::atomic<bool>* cancel);

// ~/Library/Caches/MediaViewer/Photos Library: previews and on-view downloads.
// Every file under it is write-protected (the host registers the prefix).
[[nodiscard]] std::string cache_dir();
// [worker] The on-view downloads only ("cleared after").
void clear_downloads() noexcept;
// [worker] Previews and downloads: at launch.
void clear_cache() noexcept;

}  // namespace mv::shell::photos
