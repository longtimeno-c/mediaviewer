// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mobile/search.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include <mediaviewer/mediaviewer_addon.h>
#include <mediaviewer/mediaviewer_ai.h>

#include "addons/ai/engine.h"
#include "addons/ai/host.h"
#include "addons/ai/pack.h"
#include "core/json.h"

namespace mv::mobile {
namespace {

namespace fs = std::filesystem;

fs::path fs_path(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

std::string slurp(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

bool write_new(const fs::path& path, std::string_view text) {
  std::ofstream out(path, std::ios::binary);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.close();
  return static_cast<bool>(out);
}

// Hard links, so a catalogue of a gigabyte is not on the phone twice; a symlink
// where the volume will not take one. The generation stays untouched.
bool link_in(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::create_hard_link(from, to, ec);
  if (!ec) return true;
  ec.clear();
  fs::create_symlink(from, to, ec);
  return !ec;
}

// The phone's whole tier: one tower, in the slot the tier's own name says.
std::string tiers_json(const std::string& tier, std::string_view spec, const std::string& folder) {
  json::writer w;
  w.begin_object();
  w.key("default").string(tier);
  w.key("profiles").begin_object();
  w.key(tier).begin_object();
  w.key("name").string("Phone");
  w.key("pictures").begin_object();
  // A reader takes whichever slot's key matches the index, so the slot only
  // has to be a real one; the high slot is the tier's first.
  w.key("high").string(folder);
  w.key("fast").null();
  w.end_object();
  // Speech is rows, not a model; sounds, people and documents need models the
  // phone does not have. The tier says so rather than leaving the slots out.
  w.key("speech").begin_object();
  w.key("high").string("models/no-speech-model");  // a slot the file format requires; never opened
  w.end_object();
  w.key("sounds").null();
  w.key("faces").null();
  w.key("documents").null();
  (void)spec;
  w.end_object();
  w.end_object();
  w.end_object();
  return w.take();
}

result<std::string> tier_of(const catalogue& c) {
  const auto doc = json::parse(slurp(fs_path(c.dir() + "/settings.json")), 4);
  if (!doc || doc->k != json::kind::object) return err(status::corrupt);
  const std::string* tier = doc->str("tier");
  // An older publisher wrote no tier; the engine then reads "" as the file's default.
  return tier && !tier->empty() && tier->size() < 32 ? *tier : std::string("phone");
}

std::atomic<std::uint64_t> g_correlation{1};

// ---- the host table: what a read-only engine asks of its host ------------------------------
struct host_state {
  std::string data_dir;
  std::string tile_dir;
  std::shared_ptr<const catalogue> c;
};

mv_status copy_out(const std::string& text, char* out, std::uint32_t capacity) {
  if (text.size() + 1 > capacity) return MV_ERR_INVALID_ARG;
  std::memcpy(out, text.c_str(), text.size() + 1);
  return MV_OK;
}

mv_status MV_CALL host_data_dir(void* h, char* out, std::uint32_t capacity) {
  return copy_out(static_cast<host_state*>(h)->data_dir, out, capacity);
}
mv_status MV_CALL host_stat(void*, const char* path, std::uint64_t* size, std::int64_t* mtime, std::uint32_t* is_dir) {
  std::error_code ec;
  const fs::path p = fs_path(path);
  const bool dir = fs::is_directory(p, ec);
  if (ec) return MV_ERR_NOT_FOUND;
  *size = dir ? 0 : fs::file_size(p, ec);
  if (ec) return MV_ERR_NOT_FOUND;
  *mtime = 0;
  *is_dir = dir ? 1 : 0;
  return MV_OK;
}
// Nothing here is a piece; the pack's tower is laid out in the Core folder.
mv_status MV_CALL host_piece_dir(void*, const char*, char*, std::uint32_t) { return MV_ERR_IO; }
int32_t MV_CALL host_no_yield(void*) { return 0; }
void MV_CALL host_drop_event(void*, const mv_addon_event*) {}
uint64_t MV_CALL host_correlation(void*) { return g_correlation.fetch_add(1); }
void MV_CALL host_log(void*, int32_t, const char*) {}

// A file on disk for the engine's tile calls, in place of the viewer's cache.
mv_status tile_path(host_state* s, const char* path, char* out, std::uint32_t capacity) {
  const auto id = s->c->id_at(path);
  if (!id) return MV_ERR_NOT_FOUND;
  const auto bytes = s->c->tile(*id);
  if (!bytes) return MV_ERR_IO;
  const fs::path file = fs_path(s->tile_dir) / (std::to_string(*id) + ".jpg");
  std::error_code ec;
  if (!fs::exists(file, ec)) {
    const fs::path part = file.string() + ".part." + std::to_string(g_correlation.fetch_add(1));
    if (!write_new(part, std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()))) return MV_ERR_IO;
    fs::rename(part, file, ec);
    if (ec) return MV_ERR_IO;
  }
  return copy_out(file.string(), out, capacity);
}
mv_status MV_CALL host_thumbnail(void* h, const char* path, char* out, std::uint32_t capacity) {
  return tile_path(static_cast<host_state*>(h), path, out, capacity);
}
// A phone has no tile per moment: a clip's own tile stands for all of them.
mv_status MV_CALL host_moment_thumbnail(void* h, const char* path, std::int64_t, const std::uint8_t*, std::uint32_t,
                                        std::uint32_t, char* out, std::uint32_t capacity) {
  return tile_path(static_cast<host_state*>(h), path, out, capacity);
}

}  // namespace

bool tower_supported(std::string_view spec) {
  return spec.starts_with("clip-vit-l14/") || spec.starts_with("clip-vit-b32/");
}

expected lay_out_work_dir(const catalogue& c, const std::string& work_dir) {
  const fs::path done = fs_path(work_dir) / "core" / "models" / "tiers.json";
  std::error_code ec;
  if (fs::exists(done, ec)) return {};
  const fs::path dir = fs_path(c.dir());
  const fs::path part = fs_path(work_dir + ".part");
  fs::remove_all(part, ec);
  fs::create_directories(part, ec);
  if (ec) return err(status::io);

  // What the engine reads from its data folder, under the names it reads.
  if (!link_in(dir / "catalog.db", part / "index.db")) return err(status::io);
  for (const char* name : {"settings.json", "labels.f32", "faces.db"}) {
    if (fs::exists(dir / name, ec) && !link_in(dir / name, part / name)) return err(status::io);
  }

  // The text tower, as pack.cpp finds it: models/tiers.json, then a folder
  // holding model.json and the file it names, and the tokenizer its sibling.
  const model_file* model_json = nullptr;
  for (const model_file& m : c.info().models) {
    if (m.name == "model.json") model_json = &m;
    if (static_cast<std::int64_t>(fs::file_size(fs_path(m.path), ec)) != static_cast<std::int64_t>(m.size) || ec) {
      return err(ec ? status::not_found : status::corrupt);
    }
  }
  if (!model_json) return err(status::not_found);
  const auto spec = json::parse(slurp(fs_path(model_json->path)), 8);
  const std::string* tokenizer = spec ? spec->str("tokenizer") : nullptr;
  const std::string* text = spec ? spec->str("text") : nullptr;
  if (!tokenizer || !text || tokenizer->find('/') != std::string::npos || text->find('/') != std::string::npos) {
    return err(status::corrupt);
  }
  const fs::path models = part / "core" / "models";
  const std::string folder = "models/tower";
  const fs::path tower = models / "tower";
  const fs::path tokens = models / *tokenizer;
  fs::create_directories(tower, ec);
  fs::create_directories(tokens, ec);
  if (ec) return err(status::io);
  for (const model_file& m : c.info().models) {
    const fs::path to = (m.name == "vocab.json" || m.name == "merges.txt") ? tokens / m.name : tower / m.name;
    if (!link_in(fs_path(m.path), to)) return err(status::io);
  }
  if (!fs::exists(tower / *text, ec)) return err(status::not_found);

  MV_TRY(std::string tier, tier_of(c));
  if (!write_new(models / "tiers.json", tiers_json(tier, c.info().active_spec, folder))) return err(status::io);

  // The commit: a work folder with a tiers.json is a whole one.
  fs::remove_all(fs_path(work_dir), ec);
  fs::create_directories(fs_path(work_dir).parent_path(), ec);
  fs::rename(part, fs_path(work_dir), ec);
  if (ec) return err(status::io);
  return {};
}

struct search_engine::impl {
  host_state state;
  mv_host_api api{};
  std::unique_ptr<ai::host> host;
  std::unique_ptr<ai::engine> eng;
  std::mutex one_at_a_time;
  std::atomic<std::uint64_t> newest{0};
};

search_engine::~search_engine() {
  if (impl_ && impl_->eng) impl_->eng->stop();
}

result<std::unique_ptr<search_engine>> search_engine::start(std::shared_ptr<const catalogue> c,
                                                            const std::string& work_dir) {
  if (!tower_supported(c->info().active_spec)) return err(status::unsupported_format);
  MV_TRY_VOID(lay_out_work_dir(*c, work_dir));
  std::unique_ptr<search_engine> s(new search_engine());
  s->impl_ = std::make_unique<impl>();
  impl& i = *s->impl_;
  i.state.data_dir = work_dir;
  i.state.tile_dir = work_dir + "/tiles";
  i.state.c = c;
  std::error_code ec;
  fs::create_directories(fs_path(i.state.tile_dir), ec);
  if (ec) return err(status::io);

  i.api.struct_size = sizeof i.api;
  i.api.host_api = MV_ADDON_HOST_API;
  i.api.host = &i.state;
  i.api.data_dir = &host_data_dir;
  i.api.stat_file = &host_stat;
  i.api.piece_dir = &host_piece_dir;
  i.api.should_yield = &host_no_yield;
  i.api.post_event = &host_drop_event;
  i.api.next_correlation_id = &host_correlation;
  i.api.log = &host_log;
  i.api.thumbnail_path = &host_thumbnail;
  i.api.moment_thumbnail = &host_moment_thumbnail;
  // place_matches stays empty on purpose: a reader's engine then answers
  // `place:` from the labels the catalogue stores (stored_place_matches), by
  // the same rule the desktop's Places add-on follows.
  i.host = std::make_unique<ai::host>(&i.api);

  ai::engine_deps deps = ai::pack_deps(*i.host, work_dir + "/core", work_dir);
  // Speech needs no model, only the rows: the transcript the catalogue holds
  // is the one to read, whichever Whisper wrote it.
  deps.speech_spec_key = [spec = c->speech_spec()](std::uint32_t) { return spec; };
  ai::engine_options options;
  options.read_only = true;
  options.immutable = true;
  i.eng = std::make_unique<ai::engine>(&i.api, std::move(deps), options);
  MV_TRY_VOID(i.eng->start());
  return s;
}

search_engine::state search_engine::current() const {
  mv_ai_status st{};
  st.struct_size = sizeof st;
  impl_->eng->status(st);
  if (st.state == MV_AI_STATE_LOADING) return state::warming;
  return st.state == MV_AI_STATE_ERROR ? state::failed : state::ready;
}

void search_engine::cancel() noexcept { impl_->newest.fetch_add(1); }

result<std::vector<hit>> search_engine::run(const std::string& query, std::uint32_t kinds, std::uint32_t timeout_ms) {
  impl& i = *impl_;
  const std::uint64_t ticket = i.newest.fetch_add(1) + 1;
  std::lock_guard lock(i.one_at_a_time);
  if (i.newest.load() != ticket) return err(status::cancelled);
  const auto start = std::chrono::steady_clock::now();
  const auto out_of_time = [&] {
    return timeout_ms != 0 &&
           std::chrono::steady_clock::now() - start > std::chrono::milliseconds(timeout_ms);
  };
  while (current() == state::warming) {
    if (i.newest.load() != ticket) return err(status::cancelled);
    if (out_of_time()) return err(status::timeout);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (current() == state::failed) return err(status::io);

  const std::uint64_t id = i.eng->search_text(query, std::string(), MV_AI_SCOPE_ALL,
                                              kinds == 0 ? static_cast<std::uint32_t>(MV_AI_KIND_ALL) : kinds);
  while (!i.eng->wait_search(id, 20)) {
    if (i.newest.load() != ticket) return err(status::cancelled);  // the engine's slot is simply left to age out
    if (out_of_time()) return err(status::timeout);
  }
  std::vector<hit> out;
  auto count = i.eng->result_count(id);
  if (!count) return err(count.error());
  out.reserve(*count);
  for (std::uint32_t n = 0; n < *count; ++n) {
    auto r = i.eng->result_at(id, n);
    if (!r) continue;
    hit h;
    h.asset = static_cast<std::int64_t>(r->asset_id);
    h.pts_ms = r->pts_ms;
    h.score = r->score;
    h.kind = r->kind;
    h.match = r->match;
    h.more_in_clip = r->more_in_clip;
    if (auto p = i.eng->result_path(id, n)) h.path = std::move(*p);
    if (auto t = i.eng->result_snippet(id, n)) h.snippet = std::move(*t);
    out.push_back(std::move(h));
  }
  i.eng->search_release(id);
  return out;
}

}  // namespace mv::mobile
