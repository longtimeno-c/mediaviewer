// SPDX-License-Identifier: GPL-2.0-or-later
// PR 5b - WASAPI shared-mode render client. Windows port (D9 / plan/15).
//
// OWNER: mediaviewer-08 (5b). This is the ONLY file in player/ permitted to
// include <audioclient.h> / <mmdeviceapi.h> / <audiopolicy.h>:
// tools/check-hostable-core.ps1 exempts *_win.cpp by name and now bans those
// headers everywhere else. A Core Audio host replaces this file and nothing else.
#include "player/audio_sink.h"

namespace mv::player {
void anchor_audio_win() noexcept {}
}  // namespace mv::player
