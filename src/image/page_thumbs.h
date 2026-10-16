// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Page thumbnails for the page sidebar (issue #429, docs/design/04 "Page
// sidebar"): small renders of one document's pages, kept in the same disk
// cache as the folder thumbnails under the file's own stamp and its path with
// "#page=<n>" appended. Both hosts call this from a background job; it never
// runs on the UI or render thread (rule 1).
#pragma once

#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/job_system.h"
#include "core/status.h"
#include "image/thumb.h"

namespace mv::image {

// Long edge of a page thumbnail. The sidebar shows a page about 120 points
// wide, so 320 px covers a portrait page at 2x and stays small to render: a
// thumbnail is drawn at this size, never resampled from the full page.
inline constexpr std::uint32_t kPageThumbLongEdge = 320;

// Like moment_thumb_key: no real file name ends "#page=<n>", so a page row
// cannot collide with a file's own row. One spelling, here.
[[nodiscard]] inline thumb_key page_thumb_key(std::string path, std::uint32_t page,
                                              std::int64_t mtime_unix, std::uint64_t size) {
  path += "#page=";
  path += std::to_string(page);
  return thumb_key{std::move(path), mtime_unix, size};
}

// Called once for each page in `pages` that has a thumbnail, cached or just
// made, with the JPEG's path. Pages that fail (a broken page, past the end)
// are not reported. Returning false stops the work (the sidebar moved on to
// another file); make_page_thumbs then returns `cancelled`.
using page_thumb_ready = bool (*)(void* user, std::uint32_t page, std::string_view jpeg_path);

// Looks every page of `pages` up in `store` (reporting the hits at once), then
// renders the misses from one read and one open of the file
// (codec::decode_pages) and stores them, reporting each as it lands. `file`
// is the document's own key (path, mtime, size). Stops with `cancelled` when
// `ctx` is cancelled. A page count the render learned goes to
// `*out_page_count` (0 when every page was a hit and nothing was opened).
[[nodiscard]] status make_page_thumbs(thumb_store& store, const thumb_key& file,
                                      std::span<const std::uint32_t> pages,
                                      const job_context* ctx, void* user, page_thumb_ready ready,
                                      std::uint32_t* out_page_count = nullptr);

// The sidebar's bookkeeping for one host (Windows: the ABI session; Mac: the
// app controller), so both follow the same rule. Thread-safe: requests come
// from the UI thread, landings from pool threads; the lock is never held
// across a render or a cache call.
//
// A request names the stop (`file`) and the window of pages the strip shows.
// Another stop than the last starts a new serial and forgets the old one's
// pages, so a landing for it is refused. A job queued for pages that have
// since scrolled out of the window (plus kSlack either side) drops them
// before rendering, so a fling renders what is on screen, not everything
// flung past.
class page_thumb_book {
 public:
  static constexpr std::uint32_t kSlack = 8;
  static constexpr std::uint32_t kMaxWindow = 256;

  struct request {
    std::uint32_t serial = 0;
    std::vector<std::uint32_t> pages;  // to render: not landed, not in flight
  };
  // Pages `first` .. `first + count - 1` (count capped at kMaxWindow).
  [[nodiscard]] request want(const std::string& file, std::uint32_t first, std::uint32_t count);
  // The pages of a queued job still worth rendering; the rest leave flight.
  [[nodiscard]] std::vector<std::uint32_t> still_wanted(std::uint32_t serial,
                                                        std::span<const std::uint32_t> pages);
  // False when `serial` is stale (nothing is recorded).
  bool landed(std::uint32_t serial, std::uint32_t page, std::string_view jpeg_path);
  // A job ended: whatever of `pages` did not land can be asked for again.
  void finished(std::uint32_t serial, std::span<const std::uint32_t> pages);
  // The landed thumbnail of `page` of the current stop, "" when none.
  [[nodiscard]] std::string path(std::uint32_t page) const;
  [[nodiscard]] std::uint32_t serial() const;
  // Nothing open: forgets the stop (the next request starts a new serial).
  void clear();

 private:
  mutable std::mutex mutex_;
  std::string file_;
  std::uint32_t serial_ = 0;
  std::unordered_map<std::uint32_t, std::string> paths_;
  std::unordered_set<std::uint32_t> pending_;
  std::uint32_t want_first_ = 0;
  std::uint32_t want_last_ = 0;
};

}  // namespace mv::image
