// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Open add-on packages (docs/design/25 "The package"): one `.mvaddon` file anyone
// can make, share, or host at a link.
//
// A package is a ZIP holding manifest.json, manifest.json.sig and the files
// the manifest lists, and nothing else. It comes from a stranger, so the
// reader takes the narrowest ZIP there is: every entry STORED (no
// compression, so nothing can inflate past the bytes on disk), no ZIP64, no
// encryption, no data descriptors, no extra fields, no comments, entries
// laid end to end from byte 0 to the central directory with no gap. Anything
// else is refused, never repaired. tools/addon-sdk/mvaddon.py writes exactly
// this; any unzip tool can still open one to look inside.
//
// The reader never touches the disk: it is handed the package's bytes (read
// once, so what was inspected is what is installed) and hands back views
// into them. Portable (D9). Worker threads only.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "addon/manifest.h"

namespace mv::addon {

// A package is themes, settings and scripts, not models: 64 MB until a slice
// needs more (docs/design/25). The first-party packs keep their own channel.
inline constexpr std::uint64_t kPackageMaxBytes = 64ull << 20;
inline constexpr std::size_t kPackageMaxEntries = 2048;
inline constexpr std::string_view kPackageExtension = ".mvaddon";

struct package_entry {
  std::string path;                     // safe_relative_path, '/'-separated
  std::span<const std::uint8_t> bytes;  // a view into the package
};

struct package_listing {
  rejection why = rejection::bad_package;
  std::vector<package_entry> entries;   // in file order
  [[nodiscard]] bool ok() const noexcept { return why == rejection::none; }
  [[nodiscard]] const package_entry* find(std::string_view path) const noexcept;
};

// Every entry of a strict package, or why it is not one. `package` must
// outlive the listing.
[[nodiscard]] package_listing read_package(std::span<const std::uint8_t> package);

}  // namespace mv::addon
