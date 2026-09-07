// SPDX-License-Identifier: GPL-2.0-or-later
// CPU-side decode product, still in the file's colour space.
#pragma once

#include <cstdint>
#include <vector>

#include "codec/format.h"

namespace mv::codec {

enum class transfer_intent : std::uint32_t {
  display_referred = 0,  // JPEG/PNG/BMP: ICC → linear → sRGB, no tone map (D6)
  scene_referred = 1,    // RAW/HDR: tone-mapped. Not produced in PR 2.
};

struct raster {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  format_family format = format_family::unknown;
  transfer_intent intent = transfer_intent::display_referred;
  // Packed RGBA8, source-encoded (not yet colour-managed). Row stride = width * 4.
  std::vector<std::uint8_t> rgba;
  // Empty means untagged. A tagged sRGB profile still goes in here; the colour
  // stage decides whether the transform is an identity.
  std::vector<std::uint8_t> icc;
  // PNG sRGB chunk (or equivalent). Distinct from "has an ICC profile": an
  // untagged file and a file that declared sRGB without embedding a profile
  // are both treated as sRGB, but only the latter is *tagged*.
  bool tagged_srgb = false;
};

}  // namespace mv::codec
