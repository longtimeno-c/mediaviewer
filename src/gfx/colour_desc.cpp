// SPDX-License-Identifier: GPL-2.0-or-later
#include "gfx/colour_desc.h"

namespace mv::gfx {

colour_desc resolve_unspecified(colour_desc desc, std::uint32_t width,
                                std::uint32_t height) noexcept {
  // plan/05: "Do not assume BT.709 limited range; phone video is frequently
  // BT.2020, and getting this wrong is the classic 'why is my video washed
  // out' bug." An explicit tag always wins; this only fills real gaps, and it
  // resolves them by resolution, which is what the specs actually imply.
  const bool sd = height > 0 && height <= 576;
  const bool uhd = width >= 3840 || height >= 2160;

  if (desc.matrix == colour_matrix::unspecified) {
    desc.matrix = sd    ? colour_matrix::bt601
                : uhd   ? colour_matrix::bt2020_ncl
                        : colour_matrix::bt709;
  }
  if (desc.primaries == colour_primaries::unspecified) {
    desc.primaries = sd  ? colour_primaries::bt601_625
                   : uhd ? colour_primaries::bt2020
                         : colour_primaries::bt709;
  }
  if (desc.transfer == colour_transfer::unspecified) {
    // Never guess HDR. An untagged clip is SDR: guessing PQ or HLG here would
    // tone-map ordinary video and darken it, which is worse than the bug this
    // function exists to avoid.
    desc.transfer = colour_transfer::bt709;
  }
  if (desc.range == colour_range::unspecified) {
    // YUV video is limited range far more often than not. This is the one
    // genuinely lossy guess in here, which is why an explicit tag wins.
    desc.range = colour_range::limited;
  }
  return desc;
}

}  // namespace mv::gfx
