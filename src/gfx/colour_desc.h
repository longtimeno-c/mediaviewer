// SPDX-License-Identifier: GPL-2.0-or-later
// Colour description carried by a decoded video frame, consumed by the YUV->RGB
// + tone-map shader.
//
// This lives in gfx/, not player/, for a module-graph reason: the shader that
// consumes it sits next to blit.cpp, and gfx may not include player (nor image,
// so image/colour.h — which is the ICC path for stills — is also not the place).
// player -> gfx is legal, so player fills this in and passes it down.
//
// Values mirror the AVCOL_* enums numerically where the ITU-T H.273 code points
// agree, but this header deliberately names no FFmpeg type: nothing outside
// player/ may see one (plan/02 module graph, CLAUDE.md).
#pragma once

#include <cstdint>

namespace mv::gfx {

// H.273 matrix coefficients. Do NOT default to bt709 and hope.
enum class colour_matrix : std::uint8_t {
  unspecified = 0,
  bt709,
  bt601,
  bt2020_ncl,
  smpte240m,
};

enum class colour_primaries : std::uint8_t {
  unspecified = 0,
  bt709,
  bt601_525,
  bt601_625,
  bt2020,
};

// The transfer function the samples are encoded with. hlg and pq are the two
// that MUST be tone-mapped to SDR in v1 (plan/03, plan/05). A camera JPEG is
// display-referred sRGB and is never tone-mapped — that is the still path and
// this enum does not reach it.
enum class colour_transfer : std::uint8_t {
  unspecified = 0,
  bt709,        // ~gamma 2.4 scene-referred, the SDR video default
  srgb,
  smpte2084,    // PQ
  arib_std_b67, // HLG
};

// Limited (studio/TV, 16-235 at 8-bit) vs full (PC, 0-255). Phone video is not
// reliably one or the other; carry what the stream says.
enum class colour_range : std::uint8_t {
  unspecified = 0,
  limited,
  full,
};

struct colour_desc {
  colour_matrix    matrix    = colour_matrix::unspecified;
  colour_primaries primaries = colour_primaries::unspecified;
  colour_transfer  transfer  = colour_transfer::unspecified;
  colour_range     range     = colour_range::unspecified;
  // 8 for NV12, 10 for P010. P010 stores its 10 bits in the HIGH bits of a
  // 16-bit word: the shader shifts down, or every value is 64x too large.
  std::uint8_t     bit_depth = 8;
  std::uint8_t     reserved0 = 0;
  std::uint8_t     reserved1 = 0;
  std::uint8_t     reserved2 = 0;
};

// Resolution for `unspecified`, applied once at decode time so the shader never
// guesses. The rule of thumb the spec actually supports: SD -> BT.601,
// HD -> BT.709, UHD -> BT.2020. Range defaults to limited for YUV, which is the
// common case, but an explicit tag always wins over this.
[[nodiscard]] colour_desc resolve_unspecified(colour_desc desc, std::uint32_t width,
                                              std::uint32_t height) noexcept;

// True when the transfer function requires HDR->SDR tone-mapping in v1.
[[nodiscard]] constexpr bool needs_tone_map(const colour_desc& d) noexcept {
  return d.transfer == colour_transfer::smpte2084 ||
         d.transfer == colour_transfer::arib_std_b67;
}

}  // namespace mv::gfx
