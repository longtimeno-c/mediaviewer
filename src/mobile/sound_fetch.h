// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The sound text tower's download and the question whether a catalogue needs it
// (docs/plans/mobile.md §9, "Sound search"; the pin is sound_tower.h). mv_mobile_link.h's
// mv_sound_* calls are the C face of these.
#pragma once

#include <cstdint>
#include <string>

#include "mobile/link.h"
#include "mobile/mv_mobile_link.h"
#include "mobile/sound_tower.h"

namespace mv::mobile::sound {

// BLOCKS. Every pinned file into folder(library_dir), resumed, each one hashed whole and kept
// only if it is the pinned file; verified.txt last. An mv_link_status: OK (ready), WAITING
// (`allowed` said no), CANCELLED, UNREACHABLE, NOT_FOUND (the release no longer has a pinned
// file), REJECTED (a file that is not the pinned one: deleted), NO_SPACE, INTERNAL.
// `files`: the pin (kFiles; a test gives its own).
int32_t fetch_all(const std::string& library_dir, const std::string& base, mv_fetch_fn fetch, void* fetch_ctx,
                  const cancel_flag* cancel, mv_sync_allowed_fn allowed, mv_sync_progress_fn progress, void* ctx,
                  pin files = kFiles);

// True when this generation's catalogue has clip sounds under kSpec: one indexed probe.
bool wanted(const std::string& generation_dir);

}  // namespace mv::mobile::sound
