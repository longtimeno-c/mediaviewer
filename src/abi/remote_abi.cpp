// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// 0.44: another computer's folders (mediaviewer.h, docs/plans/desktop-client.md):
// the process's remote library for the Windows core, the tile makers wired to
// the image pipeline and the poster, and the mv_remote_* calls the C# chrome
// makes. Portable C++ (no Win32 here); only built into mediaviewer_core.
#include <mediaviewer/mediaviewer.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "abi/guard.h"
#include "abi/remote_source.h"
#include "core/json.h"
#include "image/thumb.h"
#include "io/file.h"
#include "io/paths.h"
#include "player/poster.h"
#include "remote/key.h"
#include "remote/library.h"

namespace mv::abi::remote_source {

namespace fs = std::filesystem;

namespace {

std::once_flag g_once;
std::atomic<remote::library*> g_lib{nullptr};

class clip_source final : public player::byte_source {
 public:
  explicit clip_source(remote::range_reader& r) : r_(r) {}
  result<std::size_t> read_at(std::uint64_t offset, std::span<std::uint8_t> out) noexcept override {
    return r_.read_at(offset, out);
  }
  std::uint64_t size() const noexcept override { return r_.size(); }

 private:
  remote::range_reader& r_;
};

std::string utf8_of(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

fs::path fs_path(const std::string& s) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

void make() {
  // Beside the thumbnail cache: %LocalAppData%\MediaViewer\remote (servers.json,
  // no key in it) and ...\remote-cache (originals, tiles, last listings).
  auto thumbs = io::thumb_cache_dir();
  if (!thumbs) return;
  const fs::path app = fs_path(thumbs.value()).parent_path();
  remote::config c;
  c.data_dir = utf8_of(app / "remote");
  c.cache_dir = utf8_of(app / "remote-cache");
  if (const char* name = std::getenv("COMPUTERNAME"); name && *name) c.device_name = name;
  c.still_tile = [](std::span<const std::uint8_t> bytes, const job_context* ctx) {
    return image::make_thumb_jpeg(bytes, ctx);
  };
  c.clip_tile = [](remote::range_reader& r, const job_context* ctx) -> result<std::vector<std::uint8_t>> {
    clip_source src(r);
    auto poster = player::poster_frame(src, image::kThumbLongEdge, ctx);
    if (!poster) return err(poster.error());
    return image::encode_thumb_rgba(poster->rgba, poster->width, poster->height);
  };
  auto lib = std::make_unique<remote::library>(std::move(c));
  (void)lib->load();  // a damaged servers.json lists nothing; pairing again rewrites it
  remote::library* raw = lib.get();
  remote::library::install(std::move(lib));
  g_lib.store(raw, std::memory_order_release);
}

}  // namespace

remote::library* library() {
  std::call_once(g_once, make);
  return g_lib.load(std::memory_order_acquire);
}

void start() {
  std::thread([] { (void)library(); }).detach();
}

std::optional<std::pair<std::uint64_t, std::int64_t>> stamp(std::string_view file_key) {
  remote::library* lib = library();
  return lib ? lib->stamp(file_key) : std::nullopt;
}

result<remote::listing> list(std::string_view folder_key) {
  remote::library* lib = library();
  if (!lib) return err(status::internal);
  return lib->list(folder_key, /*person=*/true);
}

result<std::vector<std::uint8_t>> tile(std::string_view file_key, const job_context* ctx) {
  remote::library* lib = library();
  if (!lib) return err(status::internal);
  return lib->tile(file_key, ctx);
}

std::optional<std::vector<std::uint8_t>> cached_tile(std::string_view file_key) {
  remote::library* lib = library();
  if (!lib) return std::nullopt;
  auto path = lib->cached_tile(file_key);
  if (!path) return std::nullopt;
  auto bytes = io::read_all(*path);
  if (!bytes) return std::nullopt;
  return std::move(bytes).value();
}

result<std::string> fetch(std::string_view file_key, const job_context* ctx) {
  remote::library* lib = library();
  if (!lib) return err(status::internal);
  if (auto hit = lib->cached(file_key)) return *hit;
  // The job's cancel (a navigation) reaches the fetch through its progress.
  mobile::cancel_flag flag;
  return lib->fetch(file_key, &flag, [&](std::uint64_t, std::uint64_t) {
    if (ctx && ctx->cancelled()) flag.stop.store(true);
  });
}

}  // namespace mv::abi::remote_source

namespace {

using mv::status;
namespace rs = mv::abi::remote_source;

status write_out(const std::string& text, char* out, uint32_t cap, uint32_t* needed) {
  const auto need = static_cast<uint32_t>(text.size() + 1);
  if (needed) *needed = need;
  if (!out || cap < need) return status::invalid_arg;
  std::memcpy(out, text.c_str(), need);
  return status::ok;
}

// The one pairing in flight and how the last one ended, for the chrome's poll.
// 0.45's changing calls (add, make_folder, move) and fetch: the work is done, so a short
// buffer is not a failure -- what fits, NUL-ended, and `needed` the whole size. A caller that
// retried on a refusal here would upload or move a second time.
status write_done(const std::string& text, char* out, uint32_t cap, uint32_t* needed) {
  const auto need = static_cast<uint32_t>(text.size() + 1);
  if (needed) *needed = need;
  if (out && cap > 0) {
    const uint32_t n = std::min(cap - 1, need - 1);
    std::memcpy(out, text.data(), n);
    out[n] = '\0';
  }
  return status::ok;
}

std::mutex g_pair_m;
bool g_pair_busy = false;
bool g_pair_ok = false;
std::string g_pair_words;
std::string g_pair_key;       // the server the last pairing that worked added
std::uint64_t g_pair_seq = 0;  // pairings that worked this run

}  // namespace

extern "C" {

MV_API mv_status MV_CALL mv_remote_servers_json(char* utf8, uint32_t cap, uint32_t* out_bytes) {
  return static_cast<mv_status>(mv::abi::guard("mv_remote_servers_json", [&]() -> status {
    mv::json::writer w;
    w.begin_array();
    if (mv::remote::library* lib = mv::remote::library::shared()) {
      for (const auto& s : lib->servers()) {
        w.begin_object()
            .key("name").string(s.name)
            .key("key").string(mv::remote::server_key(s.id))
            .key("state").string(mv::remote::reach_name(s.state))
            .key("role").string(s.role)
            .end_object();
      }
    }
    w.end_array();
    return write_out(w.str(), utf8, cap, out_bytes);
  }));
}

MV_API mv_status MV_CALL mv_remote_generation(uint64_t* out_generation) {
  return static_cast<mv_status>(mv::abi::guard("mv_remote_generation", [&]() -> status {
    MV_REQUIRE(out_generation, "out_generation is required");
    mv::remote::library* lib = mv::remote::library::shared();
    *out_generation = lib ? lib->generation() : 0;
    return status::ok;
  }));
}

MV_API mv_status MV_CALL mv_remote_preview_json(const char* code_utf8, char* utf8, uint32_t cap, uint32_t* out_bytes) {
  return static_cast<mv_status>(mv::abi::guard("mv_remote_preview_json", [&]() -> status {
    MV_REQUIRE(code_utf8, "code_utf8 is required");
    mv::json::writer w;
    w.begin_object();
    mv::remote::library* lib = mv::remote::library::shared();
    // Not started yet: said as "can't reach" (status::internal is a store that would not keep the keys).
    auto p = lib ? lib->preview(code_utf8, static_cast<std::int64_t>(std::time(nullptr)))
                 : mv::result<mv::remote::code_preview>(mv::err(status::io));
    if (p) {
      w.key("name").string(p->name).key("fingerprint").string(p->fingerprint).key("paired").boolean(p->already_paired);
    } else {
      w.key("error").string(mv::remote::pair_problem_words(p.error(), {}));
    }
    w.end_object();
    return write_out(w.str(), utf8, cap, out_bytes);
  }));
}

MV_API mv_status MV_CALL mv_remote_pair_start(const char* code_utf8) {
  return static_cast<mv_status>(mv::abi::guard("mv_remote_pair_start", [&]() -> status {
    MV_REQUIRE(code_utf8, "code_utf8 is required");
    {
      std::lock_guard lock(g_pair_m);
      if (g_pair_busy) return status::busy;
      g_pair_busy = true;
      g_pair_ok = false;
      g_pair_words.clear();
    }
    std::thread([code = std::string(code_utf8)] {
      mv::remote::library* lib = rs::library();
      const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
      std::string name;
      if (lib) {
        if (auto p = lib->preview(code, now)) name = p->name;
      }
      auto paired = lib ? lib->pair(code, now) : mv::result<std::string>(mv::err(status::io));
      std::lock_guard lock(g_pair_m);
      g_pair_busy = false;
      g_pair_ok = paired.has_value();
      if (paired) {
        g_pair_key = mv::remote::server_key(*paired);
        ++g_pair_seq;
      }
      g_pair_words = paired ? "Connected to " + name + ". Its folders are in the folder list."
                            : mv::remote::pair_problem_words(paired.error(), name);
    }).detach();
    return status::ok;
  }));
}

MV_API mv_status MV_CALL mv_remote_pair_state_json(char* utf8, uint32_t cap, uint32_t* out_bytes) {
  return static_cast<mv_status>(mv::abi::guard("mv_remote_pair_state_json", [&]() -> status {
    mv::json::writer w;
    {
      std::lock_guard lock(g_pair_m);
      w.begin_object().key("busy").boolean(g_pair_busy).key("ok").boolean(g_pair_ok).key("words").string(g_pair_words)
          .key("key").string(g_pair_key).key("seq").integer(static_cast<std::int64_t>(g_pair_seq))
          .end_object();
    }
    return write_out(w.str(), utf8, cap, out_bytes);
  }));
}

MV_API mv_status MV_CALL mv_remote_forget(const char* server_key_utf8) {
  return static_cast<mv_status>(mv::abi::guard("mv_remote_forget", [&]() -> status {
    MV_REQUIRE(server_key_utf8, "server_key_utf8 is required");
    auto k = mv::remote::parse_key(server_key_utf8);
    MV_REQUIRE(k.has_value(), "not a server key");
    const std::string id = k->server;
    std::thread([id] {
      if (mv::remote::library* lib = rs::library()) (void)lib->forget(id);
    }).detach();
    return status::ok;
  }));
}

// ---- 0.45: changing a server's folders (plan §6) -------------------------------------------

namespace {
// The bytes so far, and the caller's stop, as the library's cancel flag.
struct progress_bridge {
  mv_remote_progress_fn fn;
  void* user;
  mv::mobile::cancel_flag flag;
  void operator()(std::uint64_t bytes) {
    if (fn && fn(bytes, user) != 0) flag.stop.store(true);
  }
};
}  // namespace

MV_API mv_status MV_CALL mv_remote_can_write(const char* key_utf8, uint8_t* out_can) {
  return static_cast<mv_status>(mv::abi::guard("mv_remote_can_write", [&]() -> status {
    MV_REQUIRE(key_utf8 && out_can, "key_utf8 and out_can are required");
    mv::remote::library* lib = mv::remote::library::shared();
    *out_can = lib && lib->can_write(key_utf8) ? 1 : 0;
    return status::ok;
  }));
}

MV_API mv_status MV_CALL mv_remote_write_problem(const char* key_utf8, char* utf8, uint32_t cap, uint32_t* out_bytes) {
  return static_cast<mv_status>(mv::abi::guard("mv_remote_write_problem", [&]() -> status {
    MV_REQUIRE(key_utf8, "key_utf8 is required");
    mv::remote::library* lib = mv::remote::library::shared();
    return write_out(lib ? lib->write_problem(key_utf8) : std::string(), utf8, cap, out_bytes);
  }));
}

MV_API mv_status MV_CALL mv_remote_exists(const char* key_utf8, uint8_t* out_exists) {
  return static_cast<mv_status>(mv::abi::guard("mv_remote_exists", [&]() -> status {
    MV_REQUIRE(key_utf8 && out_exists, "key_utf8 and out_exists are required");
    mv::remote::library* lib = rs::library();
    if (!lib) return status::internal;
    auto r = lib->exists(key_utf8);
    if (!r) return r.error();
    *out_exists = r.value() ? 1 : 0;
    return status::ok;
  }));
}

MV_API mv_status MV_CALL mv_remote_make_folder(const char* parent_key_utf8, const char* name_utf8, char* out_key,
                                               uint32_t cap, uint32_t* out_bytes) {
  return static_cast<mv_status>(mv::abi::guard("mv_remote_make_folder", [&]() -> status {
    MV_REQUIRE(parent_key_utf8 && name_utf8, "parent_key_utf8 and name_utf8 are required");
    mv::remote::library* lib = rs::library();
    if (!lib) return status::internal;
    auto r = lib->make_folder(parent_key_utf8, name_utf8);
    if (!r) return r.error();
    return write_done(r.value(), out_key, cap, out_bytes);
  }));
}

MV_API mv_status MV_CALL mv_remote_add(const char* parent_key_utf8, const char* local_path_utf8, const char* name_utf8,
                                       mv_remote_progress_fn progress, void* user, char* out_key, uint32_t cap,
                                       uint32_t* out_bytes) {
  return static_cast<mv_status>(mv::abi::guard("mv_remote_add", [&]() -> status {
    MV_REQUIRE(parent_key_utf8 && local_path_utf8 && name_utf8, "parent, local path and name are required");
    mv::remote::library* lib = rs::library();
    if (!lib) return status::internal;
    progress_bridge p{progress, user, {}};
    auto r = lib->add_file(parent_key_utf8, local_path_utf8, name_utf8, &p.flag, [&](std::uint64_t n) { p(n); });
    if (!r) return r.error();
    return write_done(r.value(), out_key, cap, out_bytes);
  }));
}

MV_API mv_status MV_CALL mv_remote_move(const char* key_utf8, const char* to_parent_key_utf8, const char* name_utf8,
                                        uint8_t* out_taken, char* out_key, uint32_t cap, uint32_t* out_bytes) {
  return static_cast<mv_status>(mv::abi::guard("mv_remote_move", [&]() -> status {
    MV_REQUIRE(key_utf8 && to_parent_key_utf8 && name_utf8, "key, destination and name are required");
    mv::remote::library* lib = rs::library();
    if (!lib) return status::internal;
    mv::remote::library::change_refused why;
    auto r = lib->move(key_utf8, to_parent_key_utf8, name_utf8, &why);
    if (out_taken) *out_taken = !r && why.why == mv::driftwire::serve::client::change_why::exists ? 1 : 0;
    if (!r) return r.error();
    return write_done(r.value(), out_key, cap, out_bytes);
  }));
}

MV_API mv_status MV_CALL mv_remote_remove(const char* key_utf8) {
  return static_cast<mv_status>(mv::abi::guard("mv_remote_remove", [&]() -> status {
    MV_REQUIRE(key_utf8, "key_utf8 is required");
    mv::remote::library* lib = rs::library();
    if (!lib) return status::internal;
    auto r = lib->remove(key_utf8);
    return r ? status::ok : r.error();
  }));
}

MV_API mv_status MV_CALL mv_remote_fetch(const char* key_utf8, mv_remote_progress_fn progress, void* user,
                                         char* out_path, uint32_t cap, uint32_t* out_bytes) {
  return static_cast<mv_status>(mv::abi::guard("mv_remote_fetch", [&]() -> status {
    MV_REQUIRE(key_utf8, "key_utf8 is required");
    mv::remote::library* lib = rs::library();
    if (!lib) return status::internal;
    progress_bridge p{progress, user, {}};
    auto r = lib->fetch(key_utf8, &p.flag, [&](std::uint64_t got, std::uint64_t) { p(got); });
    if (!r) return r.error();
    return write_done(r.value(), out_path, cap, out_bytes);
  }));
}

}  // extern "C"
