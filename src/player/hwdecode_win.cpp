// SPDX-License-Identifier: GPL-2.0-or-later
// PR 5a - D3D11VA hardware decode. Windows port (D9 / plan/15-platforms.md).
//
// OWNER: mediaviewer-48 (5a). This is the ONLY file in player/ permitted to
// include <d3d11.h>: tools/check-hostable-core.ps1 exempts *_win.cpp by name.
// A Metal host replaces this file and nothing else.
#include "player/media_source.h"

namespace mv::player {
void anchor_hwdecode_win() noexcept {}
}  // namespace mv::player
