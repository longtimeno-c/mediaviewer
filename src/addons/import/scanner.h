// SPDX-License-Identifier: GPL-2.0-or-later
// Scanning a source: walk, units (the viewer's pairing plus camera sidecars),
// capture dates, and the card memory (plan/18 "Units", "Per-card memory").
// I/O threads only.
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "addons/import/host.h"
#include "addons/import/library_index.h"
#include "addons/import/model.h"
#include "core/result.h"

namespace mv::import {

// Every importable file under `root`.
[[nodiscard]] result<scan_result> scan_folder(const host& h, library_index& idx,
                                              const std::string& root,
                                              const std::atomic<bool>& cancel);

// The units that contain any of `paths` (the viewer's marks): each file's
// folder is listed so a marked JPEG brings its RAW and its sidecars along.
[[nodiscard]] result<scan_result> scan_files(const host& h, library_index& idx,
                                             const std::vector<std::string>& paths,
                                             const std::atomic<bool>& cancel);

// Groups one folder's files into units (exposed for tests). `files` indices
// are into `all`; sidecars attach to the unit whose stem they carry.
void group_units(const host& h, std::vector<source_file>& all,
                 const std::vector<std::uint32_t>& folder_files, std::vector<unit>& out);

}  // namespace mv::import
