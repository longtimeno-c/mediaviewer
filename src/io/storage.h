// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #612, Settings -> Storage: what MediaViewer keeps on disk, with sizes
// (docs/design/04 "Thumbnail cache upkeep"). Portable (D9); both hosts call
// it, Windows through mv_storage_report_json (docs/design/14), the Mac from
// main_mac.mm. Measuring walks folders: a worker thread only, never the UI or
// render thread (rule 1). Reads sizes only; never deletes, never opens a file.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mv::io {

// One thing the page lists. `id` is stable and what the hosts key their
// wording on:
//   thumbnails           the JPEG-512 cache and its index (Clear empties it)
//   search_cache         meta_cache.db, the file search tags (#291) beside it
//   clipboard            flattened clipboard copies (PR 15)
//   metadata_snapshots   "revert metadata" snapshots (PR 12); never purged
//   addon_settings       what people chose on add-ons' settings pages
//   addon:<folder>       one installed add-on with its data (the AI index,
//                        the Places database), by its folder name
//   open_addon:<folder>  one add-on from another maker
//   anything a host adds (crash_reports, telemetry)
struct storage_row {
  std::string id;
  std::string path;  // where it is (the host may offer "Show in folder")
  std::uint64_t bytes = 0;
  std::uint64_t files = 0;
};

// A place only a host knows (the crash database, the telemetry spool).
struct storage_place {
  std::string id;
  std::string path;
};

// Every place the core knows (io/paths.h), then `extra`, measured. A place
// that does not exist is a row of zero. [worker]
[[nodiscard]] std::vector<storage_row> measure_storage(const std::vector<storage_place>& extra = {});

// The rows as UTF-8 JSON: {"rows":[{"id":"...","path":"...","bytes":n,"files":n},...],
// "total":n}. Paths go only to the local chrome, never anywhere else (rule 6).
[[nodiscard]] std::string storage_json(const std::vector<storage_row>& rows);

// Splits the thumbnail directory's files into the two rows above: names
// starting "meta_cache.db" are the search cache, the rest thumbnails.
[[nodiscard]] bool is_search_cache_file(std::string_view name) noexcept;

}  // namespace mv::io
