// SPDX-License-Identifier: GPL-2.0-or-later
// PR 5b - the A/V clock: audio master, host-clock fallback, drift.
//
// OWNER: mediaviewer-08 (5b). Listed in CMakeLists.txt already, so you do not need to touch the
// build to fill this in. If you need an ADDITIONAL file, message mediaviewer-56
// rather than editing CMakeLists.txt — three sessions share this checkout.
#include "player/media_source.h"

namespace mv::player {

// Anchor so the translation unit has a symbol before it is implemented.
void anchor_av_clock() noexcept {}

}  // namespace mv::player
