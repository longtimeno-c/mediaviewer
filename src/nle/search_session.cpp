// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nle/search_session.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <vector>

#include "addon/host.h"
#include "addon/store.h"
#include "nle/thumb_reader.h"

namespace mv::nle {
namespace {

using steady = std::chrono::steady_clock;

// A string out of one of the table's (buf, cap) calls, growing once for a
// long path.
template <typename Fn>
std::string read_string(Fn&& fn) {
  std::vector<char> buf(4096);
  for (int attempt = 0; attempt < 2; ++attempt) {
    const mv_status st = fn(buf.data(), static_cast<std::uint32_t>(buf.size()));
    if (st == MV_OK) return std::string(buf.data());
    if (st != MV_ERR_INVALID_ARG) return {};
    buf.resize(64 * 1024);
  }
  return {};
}

bool has_duration(const mv_ai_api* api) {
  return api->struct_size >= offsetof(mv_ai_api, result_duration) + sizeof(api->result_duration) &&
         api->result_duration != nullptr;
}

}  // namespace

search_session::~search_session() {
  // The pack's threads post into on_event until it has shut down.
  addon_.reset();
}

result<std::unique_ptr<search_session>> search_session::open(const addon::store& s, std::string thumbs_dir) {
  std::unique_ptr<search_session> out(new search_session());
  auto thumbs = std::make_shared<thumb_reader>(std::move(thumbs_dir));
  addon::host_services svc;
  search_session* self = out.get();
  svc.post = [self](const mv_addon_event& e) { self->on_event(e); };
  svc.should_yield = [] { return false; };  // a reader does no background work to yield
  svc.thumbnail = [thumbs](const std::string& path) -> result<std::string> {
    MV_TRY(std::string hit, thumbs->lookup(path, -1));
    if (hit.empty()) return err(status::io);
    return hit;
  };
  svc.moment_thumbnail = [thumbs](const std::string& path, std::int64_t pts_ms,
                                  const addon::rgb_image* image) -> result<std::string> {
    if (image) return err(status::unsupported_format);  // never writes the viewer's cache
    MV_TRY(std::string hit, thumbs->lookup(path, pts_ms));
    if (hit.empty()) return err(status::io);
    return hit;
  };
  MV_TRY(auto loaded, addon::loaded_addon::load(s, "ai", std::move(svc), MV_AI_READER_ENTRY_SYMBOL));
  const void* table = loaded->query(MV_AI_INTERFACE);
  if (!table) return err(status::unsupported_format);
  out->api_ = static_cast<const mv_ai_api*>(table);
  out->addon_ = std::move(loaded);
  return out;
}

std::unique_ptr<search_session> search_session::over(const mv_ai_api* api) {
  std::unique_ptr<search_session> out(new search_session());
  out->api_ = api;
  return out;
}

bool search_session::wait_ready(int ms) const {
  if (!api_) return false;
  const auto deadline = steady::now() + std::chrono::milliseconds(ms);
  while (true) {
    mv_ai_status st{};
    st.struct_size = sizeof(st);
    if (api_->status(api_->ctx, &st) != MV_OK) return false;
    if (st.state == MV_AI_STATE_ERROR) return false;
    if (st.state != MV_AI_STATE_LOADING) return true;
    if (steady::now() >= deadline) return false;
    std::unique_lock lock(m_);
    cv_.wait_for(lock, std::chrono::milliseconds(20));
  }
}

void search_session::on_event(const mv_addon_event& e) {
  if (e.kind != MV_ADDON_EVENT_AI_SEARCH_DONE && e.kind != MV_ADDON_EVENT_AI_STATUS) return;
  {
    std::lock_guard lock(m_);
    if (e.kind == MV_ADDON_EVENT_AI_SEARCH_DONE) {
      done_.insert(e.id);
      while (done_.size() > 64) done_.erase(done_.begin());
    }
  }
  cv_.notify_all();
}

result<std::string> search_session::roots_json(int timeout_ms) const {
  if (!wait_ready(timeout_ms)) return err(status::busy);
  std::string json = read_string([&](char* b, std::uint32_t c) { return api_->roots_json(api_->ctx, b, c, nullptr); });
  if (json.empty()) return err(status::io);
  return json;
}

result<std::string> search_session::suggest_json(const std::string& query, int timeout_ms) const {
  if (!wait_ready(timeout_ms)) return err(status::busy);
  std::string json = read_string(
      [&](char* b, std::uint32_t c) { return api_->suggest_json(api_->ctx, query.c_str(), b, c, nullptr); });
  if (json.empty()) return err(status::io);
  return json;
}

reply search_session::run(const request& r, const std::string& text, const std::string& scope_dir, int timeout_ms) {
  reply out;
  out.correlation_id = r.correlation_id;
  if (r.version != kWireVersion) {
    out.code = status::unsupported_format;
    return out;
  }
  const auto deadline = steady::now() + std::chrono::milliseconds(timeout_ms);
  if (!wait_ready(timeout_ms)) {
    mv_ai_status st{};
    st.struct_size = sizeof(st);
    const bool failed = api_ && api_->status(api_->ctx, &st) == MV_OK && st.state == MV_AI_STATE_ERROR;
    out.code = failed ? status::not_found : status::busy;  // nothing indexed / still loading
    return out;
  }
  const char* scope = scope_dir.empty() ? nullptr : scope_dir.c_str();
  std::uint64_t id = 0;
  const mv_status started = r.kind == request_kind::similar
                                ? api_->search_similar(api_->ctx, text.c_str(), r.pts_ms, scope, r.scope, r.kinds, &id)
                                : api_->search_text(api_->ctx, text.c_str(), scope, r.scope, r.kinds, &id);
  if (started != MV_OK) {
    out.code = static_cast<status>(started);
    return out;
  }
  std::uint32_t count = 0;
  bool finished = false;
  while (!finished) {
    {
      std::unique_lock lock(m_);
      // The done event wakes this at once; the table's own answer is the
      // truth (a host without events, or one that arrived before the wait).
      cv_.wait_for(lock, std::chrono::milliseconds(5), [&] { return done_.count(id) != 0; });
      done_.erase(id);
    }
    finished = api_->result_count(api_->ctx, id, &count) == MV_OK;
    if (!finished && steady::now() >= deadline) {
      (void)api_->search_release(api_->ctx, id);
      out.code = status::timeout;
      return out;
    }
  }
  count = std::min({count, r.max_results, kMaxRows});
  out.rows.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    mv_ai_result res{};
    if (api_->result_at(api_->ctx, id, i, &res) != MV_OK) break;
    row x;
    x.pts_ms = res.pts_ms;
    x.score = res.score;
    x.kind = res.kind;
    x.more = res.more_in_clip;
    x.match = res.match;
    x.path = read_string([&](char* b, std::uint32_t c) { return api_->result_path(api_->ctx, id, i, b, c); });
    if (x.path.empty()) continue;
    x.thumb = read_string([&](char* b, std::uint32_t c) { return api_->result_thumb(api_->ctx, id, i, b, c); });
    if (x.kind == MV_AI_KIND_VIDEOS) {
      std::int64_t d = 0;
      if (has_duration(api_) && api_->result_duration(api_->ctx, id, i, &d) == MV_OK) x.duration_ms = d;
      std::uint32_t n = 0;
      if (api_->clip_matches(api_->ctx, id, x.path.c_str(), nullptr, nullptr, 0, &n) == MV_OK && n > 0) {
        std::vector<std::int64_t> ms(n);
        std::vector<float> scores(n);
        if (api_->clip_matches(api_->ctx, id, x.path.c_str(), ms.data(), scores.data(), n, &n) == MV_OK) {
          n = std::min({n, static_cast<std::uint32_t>(ms.size()), kMaxMomentsPerRow});
          for (std::uint32_t k = 0; k < n; ++k) x.moments.push_back(moment{ms[k], scores[k]});
        }
      }
    }
    out.rows.push_back(std::move(x));
  }
  (void)api_->search_release(api_->ctx, id);
  return out;
}

}  // namespace mv::nle
