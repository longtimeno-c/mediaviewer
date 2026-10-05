// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The system Photos library as an index source (issue #72, docs/design/17 "Photos
// library source"). macOS only: PhotoKit reads the library the Photos app
// shows, iCloud Photos included, with the user's Photos permission. Windows
// has no PhotoKit (iCloud for Windows syncs to a folder, which is an ordinary
// folder root), so there make_photos_source() returns null and nothing about
// this source appears (D9: a platform-specific source behind a portable
// interface; the index and search stay shared).
//
// An asset has no path. The index keys it as "photos:" + its PhotoKit
// localIdentifier (photos_key), under one root whose path is "photos:"
// (kPhotosRoot); (mtime, size) is (modificationDate, pixel count), so an edit
// in Photos re-queues it like an edited file.
//
// Rules this holds:
//   5  read-only: nothing here writes to the library (no albums, keywords,
//      favourites, edits).
//   6  identifiers are paths: never logged, never in a crash report.
//      Local only: every read has network access off. An iCloud-only
//      original with no local derivative is `unavailable`. The one exception
//      is fetch_video, the opt-in "Download iCloud videos to index them"
//      (docs/design/12 2026-10-05): it copies a clip's original to a file the
//      engine owns and deletes once the clip is indexed.
//   1  every call here may block for milliseconds (PhotoKit's own caches, a
//      decode); worker / control threads only, never the UI or render thread.
//
// Nothing here ever asks for permission: that prompt belongs to a click in the
// chrome (Settings -> Local search -> Add Photos Library). Until access is
// granted every call answers permission_denied without touching PhotoKit's
// library (a fetch before then would itself raise the system prompt).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "addons/ai/host.h"
#include "addons/ai/index_db.h"
#include "core/result.h"

namespace mv::ai {

inline constexpr std::string_view kPhotosRoot = "photos:";

[[nodiscard]] inline bool is_photos_key(std::string_view path) noexcept {
  return path.size() >= kPhotosRoot.size() && path.substr(0, kPhotosRoot.size()) == kPhotosRoot;
}
[[nodiscard]] inline std::string photos_key(std::string_view local_identifier) {
  std::string k(kPhotosRoot);
  k.append(local_identifier);
  return k;
}
[[nodiscard]] inline std::string_view photos_id(std::string_view key) noexcept {
  return is_photos_key(key) ? key.substr(kPhotosRoot.size()) : std::string_view();
}

// PHAuthorizationStatus, in an order where >= limited means readable.
enum class photos_access : std::uint32_t {
  unsupported = 0,     // no PhotoKit (Windows, an old macOS)
  not_determined = 1,  // never asked
  denied = 2,
  restricted = 3,      // parental controls / MDM
  limited = 4,         // only the photos the user picked
  full = 5,
};

[[nodiscard]] constexpr bool readable(photos_access a) noexcept { return a >= photos_access::limited; }
[[nodiscard]] constexpr const char* access_name(photos_access a) noexcept {
  switch (a) {
    case photos_access::not_determined: return "not_determined";
    case photos_access::denied:         return "denied";
    case photos_access::restricted:     return "restricted";
    case photos_access::limited:        return "limited";
    case photos_access::full:           return "full";
    case photos_access::unsupported:    break;
  }
  return "unsupported";
}

struct photos_item {
  std::string id;             // localIdentifier
  std::int64_t mtime = 0;     // modificationDate, unix seconds
  std::uint64_t size = 0;     // pixelWidth * pixelHeight
  asset_kind kind = asset_kind::photo;
};

class photos_source {
 public:
  virtual ~photos_source() = default;

  [[nodiscard]] virtual photos_access access() const = 0;

  // Every asset the Photos app shows in its library: the user's own library
  // (iCloud Photos included), not hidden assets, not shared albums, one per
  // burst and one per Live Photo (its still). `visit` returns false to stop.
  [[nodiscard]] virtual expected enumerate(const std::function<bool(const photos_item&)>& visit) = 0;

  // A still (a photo, or a Live Photo's still) at <= max_edge on its long
  // side, as the Photos app would show it (its current edit), sRGB, from what
  // is already on this Mac: PhotoKit's own derivatives or the local original,
  // never the network. not_found: the asset is gone; unavailable (status::io)
  // when only iCloud has it.
  [[nodiscard]] virtual result<rgb_frame> still(std::string_view id, std::uint32_t max_edge) = 0;

  // A local file for a video (its current edit when rendered, else the
  // original) that the host's sampler and audio reader can open read-only.
  // status::io when only iCloud has it.
  [[nodiscard]] virtual result<std::string> video_file(std::string_view id) = 0;

  // The opt-in iCloud fetch (2026-10-05): downloads a video's file (its
  // current edit when Photos keeps one, else the original) from iCloud into
  // `dest_stem` + its extension (".mov", ".mp4"), and answers that path.
  // Blocks for as long as the download takes; `progress` (0..1) is called as
  // it goes, and returning false from it cancels (status::cancelled). Nothing
  // in the library changes. unsupported_format where there is no PhotoKit.
  [[nodiscard]] virtual result<std::string> fetch_video(std::string_view id, const std::string& dest_stem,
                                                        const std::function<bool(double)>& progress) {
    (void)id;
    (void)dest_stem;
    (void)progress;
    return err(status::unsupported_format);
  }

  // Whether the network suits a bulk download now: up, and not marked
  // expensive or constrained (a phone's hotspot, Low Data Mode).
  [[nodiscard]] virtual bool network_unmetered() const { return false; }

  // `changed` runs on a PhotoKit thread when the library changes (an import,
  // an edit, a delete, an iCloud download). It must return quickly. Replaces
  // any earlier callback; null stops observing. A no-op until access is
  // readable; call again after it is granted.
  virtual void observe(std::function<void()> changed) = 0;
};

// The platform's source: PhotoKit on macOS, null elsewhere.
[[nodiscard]] std::unique_ptr<photos_source> make_photos_source();

}  // namespace mv::ai
