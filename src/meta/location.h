// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A file's coordinates alone, from a bounded read (docs/design/25 PR 59: the host
// table's location_of / locations_in, so a search provider such as Places can
// ask for a whole folder without a full metadata read per file).
//
// Cheap enough to run for every file of a folder on a worker, like
// read_date_taken: a still's first 256 KB (8 MB for TIFF and RAW, whose IFDs
// sit anywhere), EXIF GPS first, then XMP exif:GPS* (DJI and other drones); a
// clip's ISO 6709 tag through libavformat. Never throws. Read-only (rule 5).
#pragma once

#include <optional>
#include <string_view>

#include "meta/meta.h"

namespace mv::meta {

// meta::summary::where's shape (#290): lat / lon in degrees, north and east
// positive; altitude in metres above sea level when the file says.
using gps_fix = geo_point;

[[nodiscard]] std::optional<geo_point> read_location(std::string_view utf8_path);

// Pure helpers, exposed for tests.
// "38,47.886N" / "38,47,53.16N" (XMP exif:GPSLatitude) -> 38.7981.
[[nodiscard]] std::optional<double> xmp_coordinate_degrees(std::string_view text) noexcept;
// "48.85837° N, 2.29448° E" (format_coordinate's pair, a clip's summary) -> the fix.
[[nodiscard]] std::optional<gps_fix> parse_coordinate_pair(std::string_view text) noexcept;

}  // namespace mv::meta
