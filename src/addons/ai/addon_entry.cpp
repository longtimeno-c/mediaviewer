// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// mv_ai's one export, mv_addon_get, and the mv.ai.1 table behind it
// (mediaviewer_ai.h). Every thunk is the ABI boundary: no exception crosses
// it (plan/14), a short output buffer reports the size it needs, and nothing
// here logs a path, a query or a name (rule 6).
#include <mediaviewer/mediaviewer_ai.h>

#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "addons/ai/engine.h"
#include "addons/ai/pack.h"
#include "addons/ai/platform.h"

#ifndef MV_AI_VERSION
#define MV_AI_VERSION "0.0.0-dev"
#endif

namespace {

using mv::ai::engine;

// Host table 2 carries the pixels (mediaviewer_addon.h "Negotiation").
constexpr uint32_t kHostApiMin = 2;
constexpr uint32_t kHostApiMax = 2;

struct addon_state {
  mv_host_api host{};
  std::unique_ptr<mv::ai::host> h;
  std::unique_ptr<engine> eng;
  mv_ai_api api{};
};

template <typename Fn>
mv_status guard(Fn&& fn) noexcept {
  try {
    return fn();
  } catch (const std::bad_alloc&) {
    return MV_ERR_OUT_OF_MEMORY;
  } catch (...) {
    return MV_ERR_INTERNAL;
  }
}

mv_status to_mv(mv::status s) noexcept { return static_cast<mv_status>(s); }

mv_status write_out(const std::string& text, char* out, uint32_t cap, uint32_t* needed) {
  const auto need = static_cast<uint32_t>(text.size() + 1);
  if (needed) *needed = need;
  if (!out || cap < need) return MV_ERR_INVALID_ARG;
  std::memcpy(out, text.data(), text.size());
  out[text.size()] = '\0';
  return MV_OK;
}

engine& eng(void* ctx) { return *static_cast<addon_state*>(ctx)->eng; }
std::string str(const char* s) { return s ? std::string(s) : std::string(); }

mv_status MV_CALL t_status(void* ctx, mv_ai_status* out) {
  return guard([&] {
    if (!out) return MV_ERR_INVALID_ARG;
    eng(ctx).status(*out);
    return MV_OK;
  });
}
mv_status MV_CALL t_settings_json(void* ctx, char* out, uint32_t cap, uint32_t* needed) {
  return guard([&] { return write_out(eng(ctx).settings_json(), out, cap, needed); });
}
mv_status MV_CALL t_set_setting(void* ctx, const char* key, const char* value) {
  return guard([&] {
    if (!key || !value) return MV_ERR_INVALID_ARG;
    auto r = eng(ctx).set_setting(key, value);
    return r ? MV_OK : to_mv(r.error());
  });
}
mv_status MV_CALL t_pause(void* ctx, uint32_t paused) {
  return guard([&] {
    eng(ctx).pause(paused != 0);
    return MV_OK;
  });
}
mv_status MV_CALL t_roots_json(void* ctx, char* out, uint32_t cap, uint32_t* needed) {
  return guard([&] { return write_out(eng(ctx).roots_json(), out, cap, needed); });
}
mv_status MV_CALL t_index_folder(void* ctx, const char* dir, uint32_t recursive, uint64_t* out_id) {
  return guard([&] {
    if (!dir) return MV_ERR_INVALID_ARG;
    auto r = eng(ctx).index_folder(dir, recursive != 0);
    if (!r) return to_mv(r.error());
    if (out_id) *out_id = static_cast<uint64_t>(*r);
    return MV_OK;
  });
}
mv_status MV_CALL t_root_enabled(void* ctx, uint64_t id, uint32_t enabled) {
  return guard([&] {
    auto r = eng(ctx).root_set_enabled(static_cast<std::int64_t>(id), enabled != 0);
    return r ? MV_OK : to_mv(r.error());
  });
}
mv_status MV_CALL t_root_rescan(void* ctx, uint64_t id) {
  return guard([&] {
    auto r = eng(ctx).root_rescan(static_cast<std::int64_t>(id));
    return r ? MV_OK : to_mv(r.error());
  });
}
mv_status MV_CALL t_root_remove(void* ctx, uint64_t id) {
  return guard([&] {
    auto r = eng(ctx).root_remove(static_cast<std::int64_t>(id));
    return r ? MV_OK : to_mv(r.error());
  });
}
mv_status MV_CALL t_coverage(void* ctx, const char* dir, uint32_t* out) {
  return guard([&] {
    if (!dir || !out) return MV_ERR_INVALID_ARG;
    *out = eng(ctx).folder_coverage(dir);
    return MV_OK;
  });
}
mv_status MV_CALL t_note_folder(void* ctx, const char* dir) {
  return guard([&] {
    if (!dir) return MV_ERR_INVALID_ARG;
    eng(ctx).note_folder_opened(dir);
    return MV_OK;
  });
}
mv_status MV_CALL t_clear(void* ctx) {
  return guard([&] {
    auto r = eng(ctx).clear_index();
    return r ? MV_OK : to_mv(r.error());
  });
}
mv_status MV_CALL t_search_text(void* ctx, const char* query, const char* scope_dir, uint32_t scope,
                                uint32_t kinds, uint64_t* out_id) {
  return guard([&] {
    if (!query || !out_id) return MV_ERR_INVALID_ARG;
    *out_id = eng(ctx).search_text(query, str(scope_dir), scope, kinds);
    return MV_OK;
  });
}
mv_status MV_CALL t_search_similar(void* ctx, const char* path, int64_t pts_ms, const char* scope_dir,
                                   uint32_t scope, uint32_t kinds, uint64_t* out_id) {
  return guard([&] {
    if (!path || !out_id) return MV_ERR_INVALID_ARG;
    *out_id = eng(ctx).search_similar(path, pts_ms, str(scope_dir), scope, kinds);
    return MV_OK;
  });
}
mv_status MV_CALL t_result_count(void* ctx, uint64_t id, uint32_t* out) {
  return guard([&] {
    if (!out) return MV_ERR_INVALID_ARG;
    auto r = eng(ctx).result_count(id);
    if (!r) return to_mv(r.error());
    *out = *r;
    return MV_OK;
  });
}
mv_status MV_CALL t_result_at(void* ctx, uint64_t id, uint32_t index, mv_ai_result* out) {
  return guard([&] {
    if (!out) return MV_ERR_INVALID_ARG;
    auto r = eng(ctx).result_at(id, index);
    if (!r) return to_mv(r.error());
    *out = *r;
    return MV_OK;
  });
}
mv_status MV_CALL t_result_path(void* ctx, uint64_t id, uint32_t index, char* out, uint32_t cap) {
  return guard([&] {
    auto r = eng(ctx).result_path(id, index);
    return r ? write_out(*r, out, cap, nullptr) : to_mv(r.error());
  });
}
mv_status MV_CALL t_result_thumb(void* ctx, uint64_t id, uint32_t index, char* out, uint32_t cap) {
  return guard([&] {
    auto r = eng(ctx).result_thumb(id, index);
    return r ? write_out(*r, out, cap, nullptr) : to_mv(r.error());
  });
}
mv_status MV_CALL t_clip_matches(void* ctx, uint64_t id, const char* path, int64_t* out_ms,
                                 float* out_scores, uint32_t cap, uint32_t* out_count) {
  return guard([&] {
    if (!path || !out_count) return MV_ERR_INVALID_ARG;
    auto r = eng(ctx).clip_matches(id, path);
    if (!r) return to_mv(r.error());
    *out_count = static_cast<uint32_t>(r->size());
    if (!out_ms) return MV_OK;
    if (cap < r->size()) return MV_ERR_INVALID_ARG;
    for (std::size_t i = 0; i < r->size(); ++i) {
      out_ms[i] = (*r)[i].first;
      if (out_scores) out_scores[i] = (*r)[i].second;
    }
    return MV_OK;
  });
}
mv_status MV_CALL t_search_release(void* ctx, uint64_t id) {
  return guard([&] {
    eng(ctx).search_release(id);
    return MV_OK;
  });
}
mv_status MV_CALL t_faces_enable(void* ctx, uint32_t enable) {
  return guard([&] {
    auto r = eng(ctx).faces_enable(enable != 0);
    return r ? MV_OK : to_mv(r.error());
  });
}
mv_status MV_CALL t_people_json(void* ctx, char* out, uint32_t cap, uint32_t* needed) {
  return guard([&] { return write_out(eng(ctx).people_json(), out, cap, needed); });
}
mv_status MV_CALL t_person_faces_json(void* ctx, uint64_t person, char* out, uint32_t cap, uint32_t* needed) {
  return guard([&] {
    return write_out(eng(ctx).person_faces_json(static_cast<std::int64_t>(person)), out, cap, needed);
  });
}
mv_status MV_CALL t_person_rename(void* ctx, uint64_t person, const char* name) {
  return guard([&] {
    if (!name) return MV_ERR_INVALID_ARG;
    auto r = eng(ctx).person_rename(static_cast<std::int64_t>(person), name);
    return r ? MV_OK : to_mv(r.error());
  });
}
mv_status MV_CALL t_person_merge(void* ctx, uint64_t into, uint64_t from) {
  return guard([&] {
    auto r = eng(ctx).person_merge(static_cast<std::int64_t>(into), static_cast<std::int64_t>(from));
    return r ? MV_OK : to_mv(r.error());
  });
}
mv_status MV_CALL t_face_reject(void* ctx, uint64_t face) {
  return guard([&] {
    auto r = eng(ctx).face_reject(static_cast<std::int64_t>(face));
    return r ? MV_OK : to_mv(r.error());
  });
}
mv_status MV_CALL t_face_split(void* ctx, const uint64_t* faces, uint32_t count, uint64_t* out_person) {
  return guard([&] {
    if (!faces || count == 0) return MV_ERR_INVALID_ARG;
    std::vector<std::int64_t> ids(faces, faces + count);
    auto r = eng(ctx).face_split(ids);
    if (!r) return to_mv(r.error());
    if (out_person) *out_person = static_cast<uint64_t>(*r);
    return MV_OK;
  });
}
mv_status MV_CALL t_person_refine(void* ctx, uint64_t person, uint32_t* out_removed) {
  return guard([&] {
    auto r = eng(ctx).person_refine(static_cast<std::int64_t>(person));
    if (!r) return to_mv(r.error());
    if (out_removed) *out_removed = *r;
    return MV_OK;
  });
}
mv_status MV_CALL t_search_person(void* ctx, uint64_t person, const char* scope_dir, uint32_t scope,
                                  uint64_t* out_id) {
  return guard([&] {
    if (!out_id) return MV_ERR_INVALID_ARG;
    *out_id = eng(ctx).search_person(static_cast<std::int64_t>(person), str(scope_dir), scope);
    return MV_OK;
  });
}
mv_status MV_CALL t_search_this_person(void* ctx, const char* path, int64_t pts_ms, uint64_t* out_id) {
  return guard([&] {
    if (!path || !out_id) return MV_ERR_INVALID_ARG;
    *out_id = eng(ctx).search_this_person(path, pts_ms);
    return MV_OK;
  });
}

mv_status MV_CALL t_face_thumb(void* ctx, uint64_t face, char* out, uint32_t cap) {
  return guard([&] {
    auto r = eng(ctx).face_thumb(static_cast<std::int64_t>(face));
    return r ? write_out(*r, out, cap, nullptr) : to_mv(r.error());
  });
}

mv_status MV_CALL t_root_media(void* ctx, uint64_t root, uint32_t media) {
  return guard([&] {
    auto r = eng(ctx).root_set_media(static_cast<std::int64_t>(root), media);
    return r ? MV_OK : to_mv(r.error());
  });
}
mv_status MV_CALL t_result_snippet(void* ctx, uint64_t id, uint32_t index, char* out, uint32_t cap) {
  return guard([&] {
    auto r = eng(ctx).result_snippet(id, index);
    return r ? write_out(*r, out, cap, nullptr) : to_mv(r.error());
  });
}
mv_status MV_CALL t_result_duration(void* ctx, uint64_t id, uint32_t index, int64_t* out) {
  return guard([&] {
    if (!out) return MV_ERR_INVALID_ARG;
    auto r = eng(ctx).result_duration(id, index);
    if (!r) return to_mv(r.error());
    *out = *r;
    return MV_OK;
  });
}
mv_status MV_CALL t_suggest_json(void* ctx, const char* query, char* out, uint32_t cap, uint32_t* needed) {
  return guard([&] {
    if (!query) return MV_ERR_INVALID_ARG;
    return write_out(eng(ctx).suggest_json(query), out, cap, needed);
  });
}

void MV_CALL shutdown(void* addon) {
  try {
    std::unique_ptr<addon_state> state(static_cast<addon_state*>(addon));
    if (state && state->eng) state->eng->stop();
  } catch (...) {
  }
}

const void* MV_CALL query(void* addon, const char* interface_id) {
  if (!addon || !interface_id) return nullptr;
  if (std::strcmp(interface_id, MV_AI_INTERFACE) != 0) return nullptr;
  return &static_cast<addon_state*>(addon)->api;
}

mv_status make(uint32_t host_api, const mv_host_api* host, mv_addon_api* out, bool read_only) {
  return guard([&] {
    if (!host || !out) return MV_ERR_INVALID_ARG;
    // Outside the range: the host says "Local search needs an update".
    if (host_api < kHostApiMin || host_api > kHostApiMax || host->host_api < kHostApiMin) {
      return MV_ERR_UNSUPPORTED_FORMAT;
    }
    if (host->struct_size < sizeof(mv_host_api)) return MV_ERR_UNSUPPORTED_FORMAT;
    auto state = std::make_unique<addon_state>();
    state->host = *host;
    state->h = std::make_unique<mv::ai::host>(&state->host);
    auto data = state->h->data_dir();
    if (!data) return to_mv(data.error());
    state->eng = std::make_unique<engine>(
        &state->host, mv::ai::pack_deps(*state->h, mv::ai::platform::self_dir(), *data),
        mv::ai::engine_options{.read_only = read_only});
    if (auto started = state->eng->start(); !started) return to_mv(started.error());

    mv_ai_api& a = state->api;
    a.struct_size = sizeof(mv_ai_api);
    a.ctx = state.get();
    a.status = &t_status;
    a.settings_json = &t_settings_json;
    a.set_setting = &t_set_setting;
    a.pause = &t_pause;
    a.roots_json = &t_roots_json;
    a.index_folder = &t_index_folder;
    a.root_set_enabled = &t_root_enabled;
    a.root_rescan = &t_root_rescan;
    a.root_remove = &t_root_remove;
    a.folder_coverage = &t_coverage;
    a.note_folder_opened = &t_note_folder;
    a.clear_index = &t_clear;
    a.search_text = &t_search_text;
    a.search_similar = &t_search_similar;
    a.result_count = &t_result_count;
    a.result_at = &t_result_at;
    a.result_path = &t_result_path;
    a.result_thumb = &t_result_thumb;
    a.clip_matches = &t_clip_matches;
    a.search_release = &t_search_release;
    a.faces_enable = &t_faces_enable;
    a.people_json = &t_people_json;
    a.person_faces_json = &t_person_faces_json;
    a.person_rename = &t_person_rename;
    a.person_merge = &t_person_merge;
    a.face_reject = &t_face_reject;
    a.face_split = &t_face_split;
    a.search_person = &t_search_person;
    a.search_this_person = &t_search_this_person;
    a.face_thumb = &t_face_thumb;
    a.root_set_media = &t_root_media;
    a.result_snippet = &t_result_snippet;
    a.suggest_json = &t_suggest_json;
    a.person_refine = &t_person_refine;
    a.result_duration = &t_result_duration;

    *out = mv_addon_api{};
    out->struct_size = sizeof(mv_addon_api);
    out->id = "ai";
    out->version = MV_AI_VERSION;
    out->addon = state.release();
    out->shutdown = &shutdown;
    out->query = &query;
    return MV_OK;
  });
}

}  // namespace

extern "C" MV_ADDON_EXPORT mv_status MV_CALL mv_addon_get(uint32_t host_api, const mv_host_api* host,
                                                          mv_addon_api* out) {
  return make(host_api, host, out, false);
}

// The search agent's door (plan/23): the same engine, read-only. A pack
// without this export predates the reader, and the agent says "Local search
// needs an update" rather than loading it through mv_addon_get, which would
// start a second indexer on the app's files.
extern "C" MV_ADDON_EXPORT mv_status MV_CALL mv_ai_reader_get(uint32_t host_api, const mv_host_api* host,
                                                              mv_addon_api* out) {
  return make(host_api, host, out, true);
}
