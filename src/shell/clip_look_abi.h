// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Video Editor's look as the C ABI carries it (mv_clip_look, ABI 0.36),
// for both hosts' clip-job submits. clip_session::to_request reads it back.
#pragma once

#include <mediaviewer/mediaviewer_clip.h>

#include <cstddef>

#include "edit/clip_look.h"

namespace mv::shell {

[[nodiscard]] inline mv_clip_look abi_look(const edit::clip::look& l) noexcept {
  mv_clip_look q{};
  q.struct_size = sizeof(q);
  q.turns = l.turns & 3u;
  q.flags = (l.flip_h ? MV_CLIP_LOOK_FLIP_H : 0u) | (l.flip_v ? MV_CLIP_LOOK_FLIP_V : 0u) |
            (l.mute ? MV_CLIP_LOOK_MUTE : 0u);
  q.crop = static_cast<std::uint32_t>(l.crop);
  static_assert(sizeof(q.light) / sizeof(q.light[0]) == edit::kAdjustParamCount);
  for (std::size_t i = 0; i < l.light.v.size(); ++i) q.light[i] = l.light.v[i];
  q.volume = l.volume;
  q.fade_in_ms = l.fade_in_ms;
  q.fade_out_ms = l.fade_out_ms;
  q.speed = l.speed;
  return q;
}

}  // namespace mv::shell
