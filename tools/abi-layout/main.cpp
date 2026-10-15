// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// abi-layout: the native layout of the structs the C# chrome mirrors by hand,
// as this compiler lays them out. The ABI smoke test
// (src.managed/MediaViewer.AbiSmokeTest) reads this and compares every field's
// offset and size with the C# struct, both ways, so a mirror that drifts from
// mediaviewer_ai.h fails CI instead of the app.
//
//   mv_abi_layout            one line per struct ("mv_ai_status 1640") and per
//                            field ("mv_ai_status.struct_size 0 4")
//
// Exits 1 when a struct's listed fields do not reach its sizeof: a field
// appended to the header but not listed here.

#include <cstddef>
#include <cstdio>

#include "mediaviewer/mediaviewer_ai.h"

namespace {

int g_failures = 0;
std::size_t g_end = 0;  // end of the furthest field listed for the current struct

void begin(const char* s, std::size_t size) {
  std::printf("%s %zu\n", s, size);
  g_end = 0;
}

void field(const char* s, const char* f, std::size_t offset, std::size_t size) {
  std::printf("%s.%s %zu %zu\n", s, f, offset, size);
  if (offset + size > g_end) g_end = offset + size;
}

void end(const char* s, std::size_t size, std::size_t align) {
  const std::size_t padded = (g_end + align - 1) / align * align;
  if (padded != size) {
    std::fprintf(stderr, "%s: listed fields end at %zu, sizeof is %zu: list the new field\n", s, g_end, size);
    ++g_failures;
  }
}

}  // namespace

#define MV_BEGIN(S) begin(#S, sizeof(S))
#define MV_FIELD(S, F) field(#S, #F, offsetof(S, F), sizeof(static_cast<S*>(nullptr)->F))
#define MV_END(S) end(#S, sizeof(S), alignof(S))

int main() {
  MV_BEGIN(mv_ai_status);
  MV_FIELD(mv_ai_status, struct_size);
  MV_FIELD(mv_ai_status, state);
  MV_FIELD(mv_ai_status, yield_reason);
  MV_FIELD(mv_ai_status, backend);
  MV_FIELD(mv_ai_status, provider_fault);
  MV_FIELD(mv_ai_status, quality);
  MV_FIELD(mv_ai_status, assets_total);
  MV_FIELD(mv_ai_status, assets_done);
  MV_FIELD(mv_ai_status, assets_failed);
  MV_FIELD(mv_ai_status, frames_indexed);
  MV_FIELD(mv_ai_status, assets_per_second);
  MV_FIELD(mv_ai_status, frames_per_second);
  MV_FIELD(mv_ai_status, eta_low_seconds);
  MV_FIELD(mv_ai_status, eta_high_seconds);
  MV_FIELD(mv_ai_status, index_bytes);
  MV_FIELD(mv_ai_status, migrate_total);
  MV_FIELD(mv_ai_status, migrate_done);
  MV_FIELD(mv_ai_status, faces_total);
  MV_FIELD(mv_ai_status, people);
  MV_FIELD(mv_ai_status, flags);
  MV_FIELD(mv_ai_status, active_root_utf8);
  MV_FIELD(mv_ai_status, model_utf8);
  MV_FIELD(mv_ai_status, sound_total);
  MV_FIELD(mv_ai_status, sound_done);
  MV_FIELD(mv_ai_status, speech_total);
  MV_FIELD(mv_ai_status, speech_done);
  MV_FIELD(mv_ai_status, assets_unavailable);
  MV_FIELD(mv_ai_status, people_scan_total);
  MV_FIELD(mv_ai_status, people_scan_done);
  MV_FIELD(mv_ai_status, people_model_utf8);
  MV_FIELD(mv_ai_status, icloud_videos_left);
  MV_FIELD(mv_ai_status, icloud_videos_fetched);
  MV_FIELD(mv_ai_status, icloud_fetch);
  MV_FIELD(mv_ai_status, icloud_fetch_progress);
  MV_FIELD(mv_ai_status, provider_detail_utf8);
  MV_FIELD(mv_ai_status, cloud_files_left);
  MV_FIELD(mv_ai_status, cloud_files_fetched);
  MV_FIELD(mv_ai_status, cloud_fetch);
  MV_FIELD(mv_ai_status, cloud_fetch_progress);
  MV_FIELD(mv_ai_status, cues_total);
  MV_FIELD(mv_ai_status, cues_done);
  MV_FIELD(mv_ai_status, text_total);
  MV_FIELD(mv_ai_status, text_done);
  MV_END(mv_ai_status);

  MV_BEGIN(mv_ai_result);
  MV_FIELD(mv_ai_result, asset_id);
  MV_FIELD(mv_ai_result, pts_ms);
  MV_FIELD(mv_ai_result, score);
  MV_FIELD(mv_ai_result, kind);
  MV_FIELD(mv_ai_result, more_in_clip);
  MV_FIELD(mv_ai_result, match);
  MV_END(mv_ai_result);

  return g_failures == 0 ? 0 : 1;
}
