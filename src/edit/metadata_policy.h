// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Export metadata preservation policy (plan/07 "Export": all / minus GPS /
// none). The ICC profile is colour, not metadata: every policy keeps it, or
// the exported pixels would change meaning.
#pragma once

#include <cstdint>

namespace mv::edit {

enum class metadata_policy : std::uint8_t {
  all = 0,        // EXIF (incl. maker notes), XMP, IPTC, comments
  minus_gps = 1,  // as `all`, with the EXIF GPS IFD zeroed and any XMP packet
                  // that names exif:GPS* dropped
  none = 2,       // pixels + ICC only
};

}  // namespace mv::edit
