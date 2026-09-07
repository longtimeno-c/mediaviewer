// SPDX-License-Identifier: GPL-2.0-or-later
// PR 5a - libavformat demux into bounded packet queues (~2 s).
//
// OWNER: mediaviewer-48 (5a). Listed in CMakeLists.txt already, so you do not need to touch the
// build to fill this in. If you need an ADDITIONAL file, message mediaviewer-56
// rather than editing CMakeLists.txt — three sessions share this checkout.
#include "player/media_source.h"

namespace mv::player {

// Anchor so the translation unit has a symbol before it is implemented.
void anchor_demux() noexcept {}

}  // namespace mv::player
