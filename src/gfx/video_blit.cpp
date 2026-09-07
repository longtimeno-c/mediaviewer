// SPDX-License-Identifier: GPL-2.0-or-later
// PR 5a - NV12/P010 -> RGB with the stream's real matrix, plus HLG/PQ -> SDR
// tone-mapping. Lives in gfx/ because gfx may not include player.
//
// OWNER: mediaviewer-48 (5a). The draw entry takes raw ID3D11ShaderResourceView*
// plus a gfx::colour_desc, so gfx never sees a player type.
#include "gfx/colour_desc.h"

namespace mv::gfx {
void anchor_video_blit() noexcept {}
}  // namespace mv::gfx
