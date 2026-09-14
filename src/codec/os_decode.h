// SPDX-License-Identifier: GPL-2.0-or-later
// OS-codec probe (D3). try_os_decode is declared on decode.h; this header holds
// what the Windows TU needs from the portable decoders to decide whether the
// OS codec may take a file at all. Do not include windows.h here (D9).
//
// POLICY (plan/12 row pending, PR 7):
//   * Only HEIC stills are offered to the OS codec. JPEG/PNG/BMP/GIF/WebP/TIFF/
//     ICO/AVIF/RAW always use the bundled decoder: their colour handling is
//     pinned by the D6 tests, and WIC gains nothing for them.
//   * A HEIC is offered only when the OS path can match the bundled result:
//     one HEVC-coded (`hvc1` or `grid`) still, 8-bit, no alpha, not HDR
//     (PQ/HLG), colour either an embedded ICC (which WIC must return through
//     GetColorContexts) or sRGB-in-effect nclx. Anything the OS would have to
//     guess — Display P3 nclx, 10-bit, sequences — is bundled.
//   * The OS path runs only if WIC has a HEIF decoder AND an HEVC decoder MFT
//     is registered (the Store HEVC Video Extension). WIC's HEIF container
//     decoder exists on a clean VM and fails there; that failure is silent.
//   * Any OS failure other than cancellation falls through to libheif, which
//     then judges the file (a corrupt HEIC is reported by the bundled decoder).
//   * MV_OS_CODEC=0 in the environment forces the bundled path (tests use it to
//     stand in for a clean VM).
#pragma once

#include <cstdint>
#include <span>

#include "codec/decode.h"

namespace mv::codec {

struct heic_still_info {
  std::uint32_t width = 0;   // displayed (after irot)
  std::uint32_t height = 0;
  std::uint32_t luma_bits = 0;
  bool hevc = false;          // primary item is hvc1, or a grid (tiles are checked by the decoder)
  bool has_alpha = false;
  bool has_icc = false;       // prof / rICC
  bool srgb_in_effect = true; // no colour box, or nclx that tag_sdr leaves untagged
  bool hdr = false;           // nclx PQ / HLG
  bool sequence = false;      // msf1 track present
};

// Header-only parse through libheif: no pixels are decoded. `unsupported_format`
// if the bytes are not a HEIC still libheif can open.
[[nodiscard]] result<heic_still_info> inspect_heic(std::span<const std::uint8_t> bytes);

// True unless MV_OS_CODEC is set to "0" (read on every call; tests flip it).
[[nodiscard]] bool os_codec_enabled() noexcept;

}  // namespace mv::codec
