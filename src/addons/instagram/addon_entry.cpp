// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// mv_instagram's one export, mv_addon_get, and its mv.slots.1 interface
// (docs/design/30). The add-on links nothing of the core: the host function
// table is its only way in. Every thunk is the ABI boundary: no exception
// crosses it, and nothing here logs a path or a file name (rule 6).
#include <mediaviewer/mediaviewer_addon.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "addons/instagram/engine.h"
#include "core/json.h"

#ifndef MV_INSTAGRAM_VERSION
#define MV_INSTAGRAM_VERSION "0.0.0-dev"
#endif

namespace {

using namespace mv::instagram;

constexpr uint32_t kHostApi = 3;

struct addon_state {
  const mv_host_api* host = nullptr;
  std::unique_ptr<engine> eng;
};

addon_state* g = nullptr;

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

// A row this host may be too old to have.
#define MV_HAS(h, row) ((h)->struct_size >= offsetof(mv_host_api, row) + sizeof((h)->row) && (h)->row)

class host_reader final : public clip_reader {
 public:
  host_reader(const mv_host_api* h, void* frames, const mv_addon_frames_info& info) : h_(h), frames_(frames), info_(info) {}
  ~host_reader() override { h_->frames_close(h_->host, frames_); }
  std::uint32_t width() const noexcept override { return info_.width; }
  std::uint32_t height() const noexcept override { return info_.height; }
  std::uint32_t rotation() const noexcept override { return info_.rotation; }
  std::int64_t frame_count() const noexcept override { return info_.frame_count; }
  bool next(std::vector<std::uint8_t>& rgb, std::int64_t& pts_us, bool& failed) override {
    rgb.resize(static_cast<std::size_t>(info_.width) * info_.height * 3);
    uint32_t got = 0;
    if (h_->frames_read(h_->host, frames_, rgb.data(), rgb.size(), &pts_us, &got) != MV_OK) {
      failed = true;
      return false;
    }
    return got != 0;
  }

 private:
  const mv_host_api* h_;
  void* frames_;
  mv_addon_frames_info info_;
};

class host_writer final : public clip_writer {
 public:
  host_writer(const mv_host_api* h, void* w) : h_(h), w_(w) {}
  ~host_writer() override { h_->clip_write_close(h_->host, w_); }
  bool frame(const std::uint8_t* rgb, std::uint32_t stride, std::int64_t pts_us) override {
    return h_->clip_write_frame(h_->host, w_, rgb, stride, pts_us) == MV_OK;
  }
  std::string finish() override {
    char out[4096] = {};
    return h_->clip_write_finish(h_->host, w_, out, sizeof out) == MV_OK ? std::string(out) : std::string();
  }

 private:
  const mv_host_api* h_;
  void* w_;
};

std::string library_dir(const mv_host_api* h) {
  char buf[4096] = {};
  return h->default_library_dir && h->default_library_dir(h->host, buf, sizeof buf) == MV_OK ? std::string(buf) : std::string();
}

std::string local_stamp() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  (void)localtime_s(&tm, &now);
#else
  (void)localtime_r(&now, &tm);
#endif
  char buf[32] = {};
  (void)std::strftime(buf, sizeof buf, "%Y-%m-%d %H%M%S", &tm);
  return buf;
}


struct walk_ctx {
  const std::function<void(gallery_row)>* add;
};

int32_t MV_CALL on_file(void* user, const mv_addon_file_entry* e) {
  try {
    gallery_row r;
    r.path = e->path_utf8;
    r.name = e->name_utf8;
    r.when = e->mtime_unix;
    (*static_cast<walk_ctx*>(user)->add)(std::move(r));
    return 1;
  } catch (...) {
    return 0;
  }
}

int32_t MV_CALL on_library(void* user, const mv_addon_library_item* i) {
  try {
    gallery_row r;
    r.path = i->key_utf8;
    r.name = i->name_utf8;
    r.video = i->is_video != 0;
    r.when = i->created_unix;
    (*static_cast<walk_ctx*>(user)->add)(std::move(r));
    return 1;
  } catch (...) {
    return 0;
  }
}

int32_t MV_CALL on_hit(void* user, const char* path) {
  try {
    if (path) static_cast<std::vector<std::string>*>(user)->emplace_back(path);
    return 1;
  } catch (...) {
    return 0;
  }
}

engine_port make_port(const mv_host_api* h) {
  engine_port p;
  const std::string screen(kScreen);
  p.post = [h, screen](std::uint64_t gen, const std::string& tree) {
    return h->screen_post(h->host, screen.c_str(), gen, tree.c_str()) == MV_OK;
  };
  p.diff = [h, screen](std::uint64_t gen, const std::string& diff) {
    return h->screen_diff(h->host, screen.c_str(), gen, diff.c_str()) == MV_OK;
  };
  p.surface = [h, screen](std::uint64_t gen, const std::string& id, const std::vector<std::uint8_t>& bgra, std::uint32_t w,
                          std::uint32_t ht) {
    (void)h->surface_post(h->host, screen.c_str(), id.c_str(), gen, bgra.data(), w, ht, w * 4);
  };
  p.show = [h, screen] { (void)h->screen_show(h->host, screen.c_str(), 1); };
  p.walk = [h](const std::string& folder, const std::function<void(gallery_row)>& add) {
    walk_ctx c{&add};
    return h->walk_files(h->host, folder.c_str(), 0, &on_file, &c) == MV_OK;
  };
  if (MV_HAS(h, library_items)) {
    p.library = [h](const std::function<void(gallery_row)>& add) {
      walk_ctx c{&add};
      return h->library_items(h->host, &on_library, &c) == MV_OK;
    };
  }
  if (MV_HAS(h, item_preview_rgb)) {
    p.preview = [h](const std::string& path, std::uint32_t edge, image& out) {
      // Sized for the long edge once, so a picture is never decoded twice.
      out.rgb.resize(static_cast<std::size_t>(edge) * edge * 3);
      if (h->item_preview_rgb(h->host, path.c_str(), edge, out.rgb.data(), out.rgb.size(), &out.w, &out.h) != MV_OK) {
        return false;
      }
      out.rgb.resize(static_cast<std::size_t>(out.w) * out.h * 3);
      return true;
    };
  }
  if (MV_HAS(h, index_search)) {
    p.search = [h](const std::string& query, const std::string& scope, std::vector<std::string>& out) {
      return h->index_search(h->host, query.c_str(), scope.c_str(), &on_hit, &out) == MV_OK;
    };
  }
  if (MV_HAS(h, still_open)) {
    p.still = [h](const std::string& path, image& out) {
      mv_addon_still_info info{};
      info.struct_size = sizeof info;
      void* still = nullptr;
      if (h->still_open(h->host, path.c_str(), &info, &still) != MV_OK || !still) return false;
      out.w = info.width;
      out.h = info.height;
      out.rgb.resize(static_cast<std::size_t>(info.width) * info.height * 3);
      const bool ok = h->still_pixels(h->host, still, out.rgb.data(), out.rgb.size()) == MV_OK;
      h->still_close(h->host, still);
      return ok;
    };
  }
  if (MV_HAS(h, still_write_into)) {
    p.write_still = [h](const image& pic, const std::string& dir, const std::string& stem) {
      char out[4096] = {};
      return h->still_write_into(h->host, pic.rgb.data(), pic.w, pic.h, pic.w * 3, dir.c_str(), stem.c_str(), out, sizeof out) == MV_OK
                 ? std::string(out)
                 : std::string();
    };
  }
  if (MV_HAS(h, library_file)) {
    p.library_file = [h](const std::string& path) {
      char out[4096] = {};
      return h->library_file(h->host, path.c_str(), out, sizeof out) == MV_OK ? std::string(out) : std::string();
    };
  }
  if (MV_HAS(h, frames_open) && MV_HAS(h, clip_write_open)) {
    p.open_clip = [h](const std::string& file) -> std::unique_ptr<clip_reader> {
      mv_addon_frames_info info{};
      info.struct_size = sizeof info;
      void* frames = nullptr;
      if (h->frames_open(h->host, file.c_str(), &info, &frames) != MV_OK || !frames) return nullptr;
      return std::make_unique<host_reader>(h, frames, info);
    };
    p.write_clip = [h](const std::string& file, std::uint32_t w, std::uint32_t ht, const std::string& dir,
                       const std::string& stem) -> std::unique_ptr<clip_writer> {
      mv_addon_clip_out o{};
      o.struct_size = sizeof o;
      o.width = w;
      o.height = ht;
      o.suffix_ascii = "post";
      o.out_dir_utf8 = dir.c_str();
      o.out_stem_utf8 = stem.c_str();
      o.rotation = 0;  // the frames are turned before they are written
      void* writer = nullptr;
      if (h->clip_write_open(h->host, file.c_str(), &o, &writer) != MV_OK || !writer) return nullptr;
      return std::make_unique<host_writer>(h, writer);
    };
  }
  p.make_dirs = [h](const std::string& dir) { return h->make_directories(h->host, dir.c_str()) == MV_OK; };
  if (MV_HAS(h, open_list)) {
    p.open_list = [h, screen](std::uint64_t gen, const std::vector<std::string>& paths) {
      std::vector<const char*> c;
      c.reserve(paths.size());
      for (const std::string& s : paths) c.push_back(s.c_str());
      (void)h->open_list(h->host, screen.c_str(), gen, "Instagram post", c.data(), static_cast<uint32_t>(c.size()), 1);
    };
  }
  if (MV_HAS(h, adjust_auto)) {
    p.adjust = [h](std::uint8_t* rgb, std::uint32_t w, std::uint32_t ht, const adjustments& values) {
      return h->adjust_rgb(h->host, rgb, w, ht, values.data(), static_cast<uint32_t>(values.size())) == MV_OK;
    };
    p.auto_adjust = [h](const std::uint8_t* rgb, std::uint32_t w, std::uint32_t ht, adjustments& out) {
      return h->adjust_auto(h->host, rgb, w, ht, out.data(), static_cast<uint32_t>(out.size())) == MV_OK;
    };
  }
  if (MV_HAS(h, embed_close)) {
    p.embed_open = [h, screen](std::uint64_t gen, const std::string& path) {
      return h->embed_open(h->host, screen.c_str(), gen, "enhance", "panel", path.c_str(), 480) == MV_OK;
    };
    p.embed_close = [h] { (void)h->embed_close(h->host, "enhance", "panel"); };
  }
  if (MV_HAS(h, busy_post)) {
    p.busy = [h](const char* what) { (void)h->busy_post(h->host, what); };
  }
  p.log = [h](int level, const char* message) {
    if (h->log) h->log(h->host, level, message);
  };
  p.library_dir = library_dir(h);
  p.stamp = &local_stamp;
  return p;
}

std::vector<std::string> paths_of(const char* value_json) {
  std::vector<std::string> out;
  const auto doc = mv::json::parse(value_json ? value_json : "", 4);
  if (!doc || doc->k != mv::json::kind::array) return out;
  for (const auto& v : doc->a) {
    if (v.k == mv::json::kind::string && !v.s.empty()) out.push_back(v.s);
  }
  return out;
}

double number_of(const mv::json::value& doc, const char* key, double fallback) {
  const mv::json::value* v = doc.find(key);
  if (!v || v->k != mv::json::kind::number) return fallback;
  return v->is_integer ? static_cast<double>(v->i) : v->d;
}

mv_status MV_CALL screen_open(void*, const mv_slot_screen_request* r) {
  return guard([&] {
    if (!r || !r->screen_id || kScreen != r->screen_id) return MV_ERR_INVALID_ARG;
    screen_facts f;
    f.generation = r->generation;
    f.path = r->path_utf8 ? r->path_utf8 : "";
    f.accent = r->theme.accent;
    f.scale = r->scale;
    g->eng->opened(f);
    return MV_OK;
  });
}

void MV_CALL on_event(void*, const mv_slot_event* e) {
  (void)guard([&] {
    if (!e || !e->node_id) return MV_OK;
    const std::string node = e->node_id;
    const char* value = e->value_json ? e->value_json : "";
    switch (e->kind) {
      case MV_SLOT_EVENT_CLOSE:
        g->eng->closed();
        break;
      case MV_SLOT_EVENT_COMMAND:
        if (node == "open") {
          g->eng->open_command(paths_of(value));
        } else {
          g->eng->add_and_show(paths_of(value));
        }
        break;
      case MV_SLOT_EVENT_ITEM_SAVED: {
        const auto doc = mv::json::parse(value, 3);
        if (!doc || doc->k != mv::json::kind::object) break;
        const std::string* source = doc->str("source");
        const std::string* result = doc->str("result");
        if (source && result) g->eng->item_saved(*source, *result);
        break;
      }
      case MV_SLOT_EVENT_CLICK:
        g->eng->click(node);
        break;
      case MV_SLOT_EVENT_CHANGE:
      case MV_SLOT_EVENT_SUBMIT:
        g->eng->change(node, value);
        break;
      case MV_SLOT_EVENT_SELECT:
        if (node == "grid") g->eng->select(value);
        break;
      case MV_SLOT_EVENT_TAP:
        g->eng->tap(node);
        break;
      case MV_SLOT_EVENT_DOUBLE:
        g->eng->reset_frame(node);
        break;
      case MV_SLOT_EVENT_PAN:
      case MV_SLOT_EVENT_ZOOM: {
        const auto doc = mv::json::parse(value, 3);
        if (!doc || doc->k != mv::json::kind::object) break;
        if (e->kind == MV_SLOT_EVENT_PAN) {
          g->eng->pan(node, number_of(*doc, "dx", 0), number_of(*doc, "dy", 0));
        } else {
          g->eng->zoom(node, number_of(*doc, "scale", 1));
        }
        break;
      }
      default:
        break;
    }
    return MV_OK;
  });
}

const mv_addon_slots kSlots{sizeof(mv_addon_slots), 0, nullptr, nullptr, &screen_open, nullptr, &on_event};

const void* MV_CALL query(void*, const char* id) {
  if (id && std::strcmp(id, MV_SLOTS_INTERFACE) == 0) return &kSlots;
  return nullptr;
}

void MV_CALL shutdown(void*) {
  delete g;
  g = nullptr;
}

}  // namespace

extern "C" MV_ADDON_EXPORT mv_status MV_CALL mv_addon_get(uint32_t host_api, const mv_host_api* host,
                                                          mv_addon_api* out) {
  return guard([&] {
    if (host_api != kHostApi || !host || !out || !MV_HAS(host, screen_show) || !host->screen_post || !host->screen_diff ||
        !host->surface_post) {
      return MV_ERR_UNSUPPORTED_FORMAT;
    }
    if (g) return MV_ERR_INVALID_ARG;  // one instance per process
    auto state = std::make_unique<addon_state>();
    state->host = host;
    state->eng = std::make_unique<engine>(make_port(host));
    g = state.release();
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    out->id = "instagram";
    out->version = MV_INSTAGRAM_VERSION;
    out->addon = g;
    out->shutdown = &shutdown;
    out->query = &query;
    return MV_OK;
  });
}
