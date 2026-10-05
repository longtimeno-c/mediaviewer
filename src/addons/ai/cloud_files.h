// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Files a cloud provider keeps online-only inside an indexed folder
// (docs/plans/document-search.md slice 0, 2026-10-05): OneDrive Files
// On-Demand on Windows, iCloud Drive's evicted files on the Mac. The folder
// walk lists them flagged (host walk_files2) and the indexer never opens one,
// since reading it is a download. With the opt-in "cloud_files" setting the
// engine brings a couple down at a time, in place, indexes them, and hands the
// space back, like the Photos library's iCloud videos (photos_source.h).
//
// A port, like photos_source: the Cloud Files API (cldapi) on Windows,
// NSFileManager's ubiquitous-item calls on the Mac, nothing elsewhere. It
// lives in the pack, not the base app.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "core/result.h"

namespace mv::ai {

class cloud_files {
 public:
  virtual ~cloud_files() = default;
  // Brings the file's bytes onto this machine, where it is (the provider keeps
  // it in sync as before). `progress` gets 0..1 and returning false cancels
  // (status::cancelled). status::unsupported_format: not a file this provider
  // can fetch on request (another sync client's placeholder on the Mac).
  [[nodiscard]] virtual expected hydrate(const std::string& path,
                                         const std::function<bool(double)>& progress) = 0;
  // Makes it online-only again, freeing the space. Best effort: a file the
  // user opened or pinned meanwhile may stay.
  [[nodiscard]] virtual expected dehydrate(const std::string& path) = 0;
  // Online, and not on a metered / expensive / Low Data connection.
  [[nodiscard]] virtual bool network_unmetered() = 0;
};

// The platform's, or nullptr where there is none.
[[nodiscard]] std::unique_ptr<cloud_files> make_cloud_files();

}  // namespace mv::ai
