// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/photos_backup_library.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/json.h"

namespace mv::shell::backup {
namespace {

// One gate for every call into Import's table (Import is loaded once per
// process). Closed just before Import unloads: no call enters after that, and
// the closer waits, bounded, for those inside. The running check job is
// remembered so closing can cancel it.
struct gate {
  std::mutex m;
  std::condition_variable cv;
  bool closed = false;
  int inside = 0;
  const mv_import_api* api = nullptr;
  std::uint64_t job = 0;  // a check job in flight, 0 when none
};
gate& the_gate() {
  static gate g;
  return g;
}

// Holds the gate open for one call; false (do not call) once it is closed.
class pass {
 public:
  pass() {
    gate& g = the_gate();
    std::lock_guard lock(g.m);
    ok_ = !g.closed;
    if (ok_) ++g.inside;
  }
  ~pass() {
    if (!ok_) return;
    gate& g = the_gate();
    {
      std::lock_guard lock(g.m);
      --g.inside;
    }
    g.cv.notify_all();
  }
  pass(const pass&) = delete;
  pass& operator=(const pass&) = delete;
  explicit operator bool() const noexcept { return ok_; }

 private:
  bool ok_ = false;
};

const char* scope_word(library_scope s) { return s == library_scope::library ? "library" : "destination"; }

// A JSON answer into a growing buffer (the table's cap / needed rule).
template <typename Call>
result<std::string> read_json(Call&& call) {
  std::string buf(4096, '\0');
  for (int attempt = 0; attempt < 4; ++attempt) {
    std::uint32_t needed = 0;
    const mv_status s = call(buf.data(), static_cast<std::uint32_t>(buf.size()), &needed);
    if (s == MV_OK) {
      buf.resize(std::char_traits<char>::length(buf.c_str()));
      return buf;
    }
    if (s != MV_ERR_INVALID_ARG || needed <= buf.size()) return err(static_cast<status>(s));
    buf.assign(needed, '\0');
  }
  return err(status::io);
}

std::vector<std::string> strings_of(const json::value* v) {
  std::vector<std::string> out;
  if (!v || v->k != json::kind::array) return out;
  for (const json::value& s : v->a) {
    if (s.k == json::kind::string) out.push_back(s.s);
  }
  return out;
}

std::uint64_t u64(const json::value& doc, std::string_view key) {
  const auto v = doc.integer(key);
  return v && *v > 0 ? static_cast<std::uint64_t>(*v) : 0;
}

class import_library final : public library {
 public:
  explicit import_library(const mv_import_api* api) : api_(api) {}

  result<library_check_output> check(const library_check_input& in, const std::atomic<bool>* cancel,
                                     const std::function<void(const library_progress&)>& on_progress) override {
    json::writer w;
    w.begin_object();
    w.key("destination").string(in.destination);
    w.key("scope").string(scope_word(in.scope));
    w.key("history").boolean(false);
    w.key("source_sizes").begin_array();
    for (const std::uint64_t s : in.sizes) w.integer(static_cast<std::int64_t>(s));
    w.end_array();
    w.key("expected").begin_array();
    for (const library_expected& e : in.expected) {
      w.begin_object();
      w.key("rel").string(e.rel);
      w.key("size").integer(static_cast<std::int64_t>(e.size));
      if (e.hash_hex.size() == 64) w.key("hash").string(e.hash_hex);
      w.end_object();
    }
    w.end_array();
    w.end_object();
    const std::string request = w.take();
    std::uint64_t job = 0;
    {
      pass entered;
      if (!entered) return err(status::cancelled);
      if (const mv_status s = api_->check_library(api_->ctx, request.c_str(), &job); s != MV_OK) {
        return err(static_cast<status>(s));
      }
      gate& g = the_gate();
      std::lock_guard lock(g.m);
      g.job = job;
    }
    const auto forget_job = [] {
      gate& g = the_gate();
      std::lock_guard lock(g.m);
      g.job = 0;
    };
    // The job runs on the add-on's thread; this is the backup's worker, so it
    // waits here (never the UI thread). 10 Hz is plenty for a progress line.
    bool cancelled = false;
    for (;;) {
      mv_import_progress p{};
      {
        pass entered;
        if (!entered) {
          forget_job();
          return err(status::cancelled);
        }
        if (api_->progress(api_->ctx, job, &p) != MV_OK) {
          forget_job();
          return err(status::io);
        }
        if (!cancelled && cancel && cancel->load(std::memory_order_acquire)) {
          (void)api_->cancel(api_->ctx, job);
          cancelled = true;
        }
      }
      if (on_progress) on_progress(library_progress{p.units_done, p.bytes_read, p.bytes_total});
      if (p.state >= MV_IMPORT_JOB_DONE && p.state != MV_IMPORT_JOB_PAUSED) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    forget_job();
    if (cancelled) return err(status::cancelled);
    pass entered;
    if (!entered) return err(status::cancelled);
    MV_TRY(std::string text, read_json([&](char* out, std::uint32_t cap, std::uint32_t* needed) {
             return api_->summary_json(api_->ctx, job, out, cap, needed);
           }));
    const auto doc = json::parse(text, 8);
    if (!doc || doc->k != json::kind::object) return err(status::corrupt);
    library_check_output out;
    out.reachable = doc->boolean("reachable").value_or(false);
    out.walk_complete = doc->boolean("walk_complete").value_or(false);
    if (doc->boolean("cancelled").value_or(false)) return err(status::cancelled);
    if (!out.reachable) return err(status::not_found);
    out.library_files = u64(*doc, "library_files");
    out.hashed = static_cast<std::uint32_t>(u64(*doc, "hashed"));
    out.remembered = static_cast<std::uint32_t>(u64(*doc, "remembered"));
    out.hashed_bytes = u64(*doc, "hashed_bytes");
    if (const json::value* v = doc->find("matched_sizes"); v && v->k == json::kind::array) {
      for (const json::value& n : v->a) {
        if (n.k == json::kind::number && n.is_integer && n.i > 0) out.matched_sizes.push_back(static_cast<std::uint64_t>(n.i));
      }
    }
    if (const json::value* v = doc->find("gone"); v && v->k == json::kind::array) {
      for (const json::value& g : v->a) {
        const std::string* rel = g.str("rel");
        const std::string* to = g.str("moved_to");
        if (rel) out.gone.push_back({*rel, to ? *to : std::string()});
      }
    }
    if (const json::value* v = doc->find("duplicates"); v && v->k == json::kind::array) {
      for (const json::value& g : v->a) out.duplicates.push_back({u64(g, "size"), strings_of(g.find("paths"))});
    }
    return out;
  }

  result<std::vector<std::string>> find(std::string_view destination, library_scope scope, std::uint64_t size,
                                        const io::content_hash& hash) override {
    json::writer w;
    w.begin_object();
    w.key("destination").string(destination);
    w.key("scope").string(scope_word(scope));
    w.key("size").integer(static_cast<std::int64_t>(size));
    w.key("hash").string(hash.hex());
    w.end_object();
    const std::string request = w.take();
    pass entered;
    if (!entered) return err(status::cancelled);
    MV_TRY(std::string text, read_json([&](char* out, std::uint32_t cap, std::uint32_t* needed) {
             return api_->library_find(api_->ctx, request.c_str(), out, cap, needed);
           }));
    const auto doc = json::parse(text);
    if (!doc || doc->k != json::kind::object) return err(status::corrupt);
    return strings_of(doc->find("paths"));
  }

  void record(std::string_view destination, const std::string& path, std::uint64_t size, std::int64_t mtime,
              const io::content_hash& hash) override {
    json::writer w;
    w.begin_object();
    w.key("destination").string(destination);
    w.key("path").string(path);
    w.key("size").integer(static_cast<std::int64_t>(size));
    w.key("mtime").integer(mtime);
    w.key("hash").string(hash.hex());
    w.end_object();
    // Only a cache: a refused record means the next check reads that file.
    pass entered;
    if (entered) (void)api_->library_record(api_->ctx, w.take().c_str());
  }

 private:
  const mv_import_api* api_;
};

}  // namespace

bool import_has_library_check(const mv_import_api* api) noexcept {
  return api && api->struct_size >= offsetof(mv_import_api, library_record) + sizeof(api->library_record) &&
         api->check_library && api->library_find && api->library_record;
}

std::shared_ptr<library> make_import_library(const mv_import_api* api) {
  if (!import_has_library_check(api)) return nullptr;
  {
    gate& g = the_gate();
    std::lock_guard lock(g.m);
    g.closed = false;
    g.api = api;
  }
  return std::make_shared<import_library>(api);
}

bool close_import_library(double seconds) {
  gate& g = the_gate();
  std::unique_lock lock(g.m);
  g.closed = true;
  // The check job keeps reading the library on the add-on's own thread after
  // its caller has gone: stop it too ([no-block] in the engine).
  if (g.job != 0 && g.api && g.api->cancel) (void)g.api->cancel(g.api->ctx, g.job);
  g.job = 0;
  return g.cv.wait_for(lock, std::chrono::duration<double>(seconds), [&g] { return g.inside == 0; });
}

}  // namespace mv::shell::backup
