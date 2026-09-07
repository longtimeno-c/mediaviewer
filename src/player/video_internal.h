// SPDX-License-Identifier: GPL-2.0-or-later
// Player-private plumbing shared by the six 5a translation units: the packet
// queue, the frame ring, the decoder context, and the two entry points
// hwdecode_win.cpp exports.
//
// OWNER: mediaviewer-48 (5a). Approved as a header because the alternative —
// repeating extern declarations in each consumer — is an ODR trap.
//
// This header is player-PRIVATE and must stay that way: nothing above player/
// may include it. It may name FFmpeg types (player/ is where they are allowed);
// the contract headers next to it may not.
//
// It must NOT include <libavutil/hwcontext_d3d11va.h> or any other header that
// drags in d3d11.h. That one lives in hwdecode_win.cpp, which is the sole D9
// port boundary. tools/check-hostable-core.ps1 now enforces that specifically.
#pragma once

namespace mv::player {

// 5a fills this in.

}  // namespace mv::player
