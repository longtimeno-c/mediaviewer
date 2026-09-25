// SPDX-License-Identifier: GPL-2.0-or-later
// mv_import's one export, mv_addon_get, and the mv.import.1 table behind it.
//
// Every thunk is the ABI boundary: no exception crosses it (plan/14), a
// short output buffer reports the size it needs, and nothing here logs a
// path (rule 6).
#include <mediaviewer/mediaviewer_import.h>

#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "addons/import/engine.h"
#include "core/json.h"

#ifndef MV_IMPORT_VERSION
#define MV_IMPORT_VERSION "0.0.0-dev"
#endif

namespace {

using mv::import::engine;

// The range of host tables this build understands (the manifest says the same).
constexpr uint32_t kHostApiMin = 1;
constexpr uint32_t kHostApiMax = 1;

struct addon_state {
  mv_host_api host{};
  std::unique_ptr<engine> eng;
  mv_import_api api{};
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

bool parse_paths(const char* json_text, std::vector<std::string>& out) {
  if (!json_text) return false;
  const auto doc = mv::json::parse(json_text);
  if (!doc || doc->k != mv::json::kind::array) return false;
  for (const auto& v : doc->a) {
    if (v.k != mv::json::kind::string) return false;
    out.push_back(v.s);
  }
  return true;
}

template <typename T>
mv_status id_out(const mv::result<T>& r, uint64_t* out) {
  if (!r) return to_mv(r.error());
  if (out) *out = r.value();
  return MV_OK;
}

mv_status MV_CALL sources_json(void* ctx, char* out, uint32_t cap, uint32_t* needed) {
  return guard([&] { return write_out(eng(ctx).sources_json(), out, cap, needed); });
}
mv_status MV_CALL add_folder_source(void* ctx, const char* dir) {
  return guard([&] {
    if (!dir) return MV_ERR_INVALID_ARG;
    auto r = eng(ctx).add_folder_source(dir);
    return r ? MV_OK : to_mv(r.error());
  });
}
mv_status MV_CALL remove_folder_source(void* ctx, const char* dir) {
  return guard([&] {
    if (!dir) return MV_ERR_INVALID_ARG;
    auto r = eng(ctx).remove_folder_source(dir);
    return r ? MV_OK : to_mv(r.error());
  });
}
mv_status MV_CALL arrival_root(void* ctx, uint64_t seq, char* out, uint32_t cap) {
  return guard([&] {
    auto r = eng(ctx).arrival_root(seq);
    return r ? write_out(*r, out, cap, nullptr) : to_mv(r.error());
  });
}
mv_status MV_CALL scan(void* ctx, const char* root, uint64_t* out_id) {
  return guard([&] { return root ? id_out(eng(ctx).scan(root), out_id) : MV_ERR_INVALID_ARG; });
}
mv_status MV_CALL scan_files(void* ctx, const char* paths_json, uint64_t* out_id) {
  return guard([&] {
    std::vector<std::string> paths;
    if (!parse_paths(paths_json, paths)) return MV_ERR_INVALID_ARG;
    return id_out(eng(ctx).scan_files(paths), out_id);
  });
}
mv_status MV_CALL plan(void* ctx, uint64_t scan_id, const char* preset_json,
                       const char* marked_json, uint64_t* out_id) {
  return guard([&] {
    std::vector<std::string> marked;
    if (marked_json && !parse_paths(marked_json, marked)) return MV_ERR_INVALID_ARG;
    const std::string preset = preset_json ? preset_json : "";
    return id_out(eng(ctx).plan(scan_id, preset_json ? &preset : nullptr,
                                marked_json ? &marked : nullptr),
                  out_id);
  });
}
mv_status MV_CALL plan_json(void* ctx, uint64_t id, char* out, uint32_t cap, uint32_t* needed) {
  return guard([&] {
    auto r = eng(ctx).plan_json(id);
    return r ? write_out(*r, out, cap, needed) : to_mv(r.error());
  });
}
mv_status MV_CALL select(void* ctx, uint64_t id, int32_t unit, const char* day, uint32_t selected) {
  return guard([&] {
    const std::string d = day ? day : "";
    auto r = eng(ctx).select(id, unit, day ? &d : nullptr, selected != 0);
    return r ? MV_OK : to_mv(r.error());
  });
}
mv_status MV_CALL thumbnail(void* ctx, uint64_t id, uint32_t unit, char* out, uint32_t cap) {
  return guard([&] {
    auto r = eng(ctx).thumbnail(id, unit);
    return r ? write_out(*r, out, cap, nullptr) : to_mv(r.error());
  });
}
mv_status MV_CALL start(void* ctx, uint64_t plan_id, uint64_t* out_id) {
  return guard([&] { return id_out(eng(ctx).start_job(plan_id), out_id); });
}
mv_status MV_CALL import_now(void* ctx, const char* paths_json, uint64_t* out_id) {
  return guard([&] {
    std::vector<std::string> paths;
    if (!parse_paths(paths_json, paths)) return MV_ERR_INVALID_ARG;
    return id_out(eng(ctx).import_now(paths), out_id);
  });
}
mv_status MV_CALL pause(void* ctx, uint64_t id, uint32_t paused) {
  return guard([&] {
    auto r = eng(ctx).pause(id, paused != 0);
    return r ? MV_OK : to_mv(r.error());
  });
}
mv_status MV_CALL cancel(void* ctx, uint64_t id) {
  return guard([&] {
    auto r = eng(ctx).cancel(id);
    return r ? MV_OK : to_mv(r.error());
  });
}
mv_status MV_CALL set_priority(void* ctx, uint64_t id, uint32_t fast) {
  return guard([&] {
    auto r = eng(ctx).set_priority(id, fast != 0);
    return r ? MV_OK : to_mv(r.error());
  });
}
mv_status MV_CALL progress(void* ctx, uint64_t id, mv_import_progress* out) {
  return guard([&] {
    if (!out) return MV_ERR_INVALID_ARG;
    auto r = eng(ctx).progress(id, *out);
    return r ? MV_OK : to_mv(r.error());
  });
}
mv_status MV_CALL summary_json(void* ctx, uint64_t id, char* out, uint32_t cap, uint32_t* needed) {
  return guard([&] {
    auto r = eng(ctx).summary_json(id);
    return r ? write_out(*r, out, cap, needed) : to_mv(r.error());
  });
}
mv_status MV_CALL retry_failed(void* ctx, uint64_t id, uint64_t* out_id) {
  return guard([&] { return id_out(eng(ctx).retry_failed(id), out_id); });
}
mv_status MV_CALL unfinished_json(void* ctx, char* out, uint32_t cap, uint32_t* needed) {
  return guard([&] { return write_out(eng(ctx).unfinished_json(), out, cap, needed); });
}
mv_status MV_CALL resume(void* ctx, uint64_t id) {
  return guard([&] {
    auto r = eng(ctx).resume(id);
    return r ? MV_OK : to_mv(r.error());
  });
}
mv_status MV_CALL report_path(void* ctx, uint64_t id, char* out, uint32_t cap) {
  return guard([&] {
    auto r = eng(ctx).report_path(id);
    return r ? write_out(*r, out, cap, nullptr) : to_mv(r.error());
  });
}
mv_status MV_CALL eject(void* ctx, const char* root) {
  return guard([&] {
    if (!root) return MV_ERR_INVALID_ARG;
    auto r = eng(ctx).eject(root);
    return r ? MV_OK : to_mv(r.error());
  });
}
mv_status MV_CALL presets_json(void* ctx, char* out, uint32_t cap, uint32_t* needed) {
  return guard([&] { return write_out(eng(ctx).presets_json(), out, cap, needed); });
}
mv_status MV_CALL save_preset(void* ctx, const char* json_text) {
  return guard([&] {
    if (!json_text) return MV_ERR_INVALID_ARG;
    auto r = eng(ctx).save_preset(json_text);
    return r ? MV_OK : to_mv(r.error());
  });
}
mv_status MV_CALL delete_preset(void* ctx, const char* name) {
  return guard([&] {
    if (!name) return MV_ERR_INVALID_ARG;
    auto r = eng(ctx).delete_preset(name);
    return r ? MV_OK : to_mv(r.error());
  });
}
mv_status MV_CALL bind_card(void* ctx, const char* volume_id, const char* preset_name,
                            uint32_t auto_import) {
  return guard([&] {
    if (!volume_id) return MV_ERR_INVALID_ARG;
    auto r = eng(ctx).bind_card(volume_id, preset_name ? preset_name : "", auto_import != 0);
    return r ? MV_OK : to_mv(r.error());
  });
}
mv_status MV_CALL preview_names_json(void* ctx, const char* preset_json, char* out, uint32_t cap,
                                     uint32_t* needed) {
  return guard([&] {
    if (!preset_json) return MV_ERR_INVALID_ARG;
    auto r = eng(ctx).preview_names_json(preset_json);
    return r ? write_out(*r, out, cap, needed) : to_mv(r.error());
  });
}
mv_status MV_CALL history_json(void* ctx, char* out, uint32_t cap, uint32_t* needed) {
  return guard([&] { return write_out(eng(ctx).history_json(), out, cap, needed); });
}
mv_status MV_CALL verify_folder(void* ctx, const char* dir, uint64_t* out_id) {
  return guard([&] { return dir ? id_out(eng(ctx).verify_folder(dir), out_id) : MV_ERR_INVALID_ARG; });
}

void MV_CALL shutdown(void* addon) {
  auto* state = static_cast<addon_state*>(addon);
  try {
    if (state->eng) state->eng->shutdown();
  } catch (...) {
  }
  delete state;
}

const void* MV_CALL query(void* addon, const char* interface_id) {
  if (!addon || !interface_id) return nullptr;
  if (std::strcmp(interface_id, MV_IMPORT_INTERFACE) != 0) return nullptr;
  return &static_cast<addon_state*>(addon)->api;
}

}  // namespace

extern "C" MV_ADDON_EXPORT mv_status MV_CALL mv_addon_get(uint32_t host_api,
                                                          const mv_host_api* host,
                                                          mv_addon_api* out) {
  return guard([&] {
    if (!host || !out) return MV_ERR_INVALID_ARG;
    // Outside the range: the host says "Import needs an update", it does not
    // load a table it cannot read.
    if (host_api < kHostApiMin || host_api > kHostApiMax || host->host_api < kHostApiMin) {
      return MV_ERR_UNSUPPORTED_FORMAT;
    }
    if (host->struct_size < sizeof(mv_host_api)) return MV_ERR_UNSUPPORTED_FORMAT;
    auto state = std::make_unique<addon_state>();
    state->host = *host;
    state->eng = std::make_unique<engine>(&state->host);
    if (auto started = state->eng->start(); !started) return to_mv(started.error());

    mv_import_api& a = state->api;
    a.struct_size = sizeof(mv_import_api);
    a.ctx = state.get();
    a.sources_json = &sources_json;
    a.add_folder_source = &add_folder_source;
    a.remove_folder_source = &remove_folder_source;
    a.arrival_root = &arrival_root;
    a.scan = &scan;
    a.scan_files = &scan_files;
    a.plan = &plan;
    a.plan_json = &plan_json;
    a.select = &select;
    a.thumbnail = &thumbnail;
    a.start = &start;
    a.import_now = &import_now;
    a.pause = &pause;
    a.cancel = &cancel;
    a.set_priority = &set_priority;
    a.progress = &progress;
    a.summary_json = &summary_json;
    a.retry_failed = &retry_failed;
    a.unfinished_json = &unfinished_json;
    a.resume = &resume;
    a.report_path = &report_path;
    a.eject = &eject;
    a.presets_json = &presets_json;
    a.save_preset = &save_preset;
    a.delete_preset = &delete_preset;
    a.bind_card = &bind_card;
    a.preview_names_json = &preview_names_json;
    a.history_json = &history_json;
    a.verify_folder = &verify_folder;

    *out = mv_addon_api{};
    out->struct_size = sizeof(mv_addon_api);
    out->id = "import";
    out->version = MV_IMPORT_VERSION;
    out->addon = state.release();
    out->shutdown = &shutdown;
    out->query = &query;
    return MV_OK;
  });
}
