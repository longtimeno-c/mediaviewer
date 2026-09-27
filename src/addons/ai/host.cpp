// SPDX-License-Identifier: GPL-2.0-or-later
#include "addons/ai/host.h"

#include <cstring>

namespace mv::ai {
namespace {

int32_t MV_CALL walk_thunk(void* user, const mv_addon_file_entry* e) {
  const auto* fn = static_cast<const std::function<bool(const host::entry&)>*>(user);
  host::entry x;
  x.path = e->path_utf8 ? e->path_utf8 : "";
  x.size = e->size;
  x.mtime = e->mtime_unix;
  return (*fn)(x) ? 1 : 0;
}

// A pixel call with the grow-and-retry buffer rule (mediaviewer_addon.h v2).
template <typename Call>
result<rgb_frame> pixels(Call&& call) {
  rgb_frame f;
  f.rgb.resize(512u * 512u * 3u);
  for (int attempt = 0; attempt < 2; ++attempt) {
    uint32_t w = 0, h = 0;
    const mv_status s = call(f.rgb.data(), static_cast<uint64_t>(f.rgb.size()), &w, &h);
    if (s == MV_OK) {
      f.width = w;
      f.height = h;
      f.rgb.resize(static_cast<std::size_t>(w) * h * 3);
      return f;
    }
    if (s != MV_ERR_INVALID_ARG || w == 0 || h == 0) return err(to_status(s));
    f.rgb.resize(static_cast<std::size_t>(w) * h * 3);
  }
  return err(status::internal);
}

result<std::string> path_out(mv_status (*fill)(char*, uint32_t, void*), void* ctx) {
  std::string buf(4096, '\0');
  const mv_status s = fill(buf.data(), static_cast<uint32_t>(buf.size()), ctx);
  if (s != MV_OK) return err(to_status(s));
  buf.resize(std::strlen(buf.c_str()));
  return buf;
}

}  // namespace

bool host::has_pixels() const noexcept {
  return api_ && api_->host_api >= 2 &&
         api_->struct_size >= offsetof(mv_host_api, piece_dir) + sizeof(api_->piece_dir) &&
         api_->decode_still_rgb && api_->sampler_open;
}

expected host::walk(const std::string& root, int max_depth,
                    const std::function<bool(const entry&)>& visit) const {
  const mv_status s = api_->walk_files(api_->host, root.c_str(), max_depth, &walk_thunk,
                                       const_cast<void*>(static_cast<const void*>(&visit)));
  return s == MV_OK ? expected{} : err(to_status(s));
}

result<host::stat_result> host::stat(const std::string& path) const {
  stat_result r;
  uint32_t dir = 0;
  const mv_status s = api_->stat_file(api_->host, path.c_str(), &r.size, &r.mtime, &dir);
  if (s != MV_OK) return err(to_status(s));
  r.is_directory = dir != 0;
  return r;
}

expected host::make_directories(const std::string& dir) const {
  const mv_status s = api_->make_directories(api_->host, dir.c_str());
  return s == MV_OK ? expected{} : err(to_status(s));
}

expected host::remove_file(const std::string& path) const {
  const mv_status s = api_->remove_file(api_->host, path.c_str());
  return s == MV_OK ? expected{} : err(to_status(s));
}

result<rgb_frame> host::decode_still(const std::string& path, std::uint32_t max_edge) const {
  if (!has_pixels()) return err(status::unsupported_format);
  return pixels([&](uint8_t* out, uint64_t cap, uint32_t* w, uint32_t* h) {
    return api_->decode_still_rgb(api_->host, path.c_str(), max_edge, out, cap, w, h);
  });
}

result<rgb_frame> host::video_frame(const std::string& path, std::int64_t pts_ms,
                                    std::uint32_t max_edge) const {
  if (!has_pixels()) return err(status::unsupported_format);
  auto f = pixels([&](uint8_t* out, uint64_t cap, uint32_t* w, uint32_t* h) {
    return api_->video_frame_rgb(api_->host, path.c_str(), pts_ms, max_edge, out, cap, w, h);
  });
  if (f) f->pts_ms = pts_ms;
  return f;
}

host::sampler::~sampler() {
  if (handle_) api_->sampler_close(api_->host, handle_);
}

result<rgb_frame> host::sampler::next() {
  if (buf_.empty()) buf_.resize(512u * 512u * 3u);
  for (int attempt = 0; attempt < 2; ++attempt) {
    mv_addon_sampled_frame f{};
    const mv_status s = api_->sampler_next(api_->host, handle_, buf_.data(),
                                           static_cast<uint64_t>(buf_.size()), &f);
    if (s == MV_OK) {
      rgb_frame out;
      out.flags = f.flags;
      out.pts_ms = f.pts_ms;
      out.pts_tb = f.pts_tb;
      out.tb_num = f.tb_num;
      out.tb_den = f.tb_den;
      if (!(f.flags & MV_ADDON_FRAME_END)) {
        out.width = f.width;
        out.height = f.height;
        out.rgb.assign(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(
                                                        static_cast<std::size_t>(f.width) * f.height * 3));
      }
      return out;
    }
    if (s != MV_ERR_INVALID_ARG || f.width == 0 || f.height == 0) return err(to_status(s));
    buf_.resize(static_cast<std::size_t>(f.width) * f.height * 3);  // the host kept the frame
  }
  return err(status::internal);
}

result<std::unique_ptr<host::sampler>> host::open_sampler(
    const std::string& path, const mv_addon_sampler_options& options) const {
  if (!has_pixels()) return err(status::unsupported_format);
  void* handle = nullptr;
  mv_addon_video_info info{};
  const mv_status s = api_->sampler_open(api_->host, path.c_str(), &options, &info, &handle);
  if (s != MV_OK) return err(to_status(s));
  return std::unique_ptr<sampler>(new sampler(api_, handle, info));
}

result<std::string> host::thumbnail(const std::string& path) const {
  struct ctx_t {
    const mv_host_api* api;
    const std::string* path;
  } ctx{api_, &path};
  return path_out(
      [](char* out, uint32_t cap, void* c) {
        auto* x = static_cast<ctx_t*>(c);
        return x->api->thumbnail_path(x->api->host, x->path->c_str(), out, cap);
      },
      &ctx);
}

result<std::string> host::moment_thumbnail(const std::string& path, std::int64_t pts_ms,
                                           const rgb_frame* frame) const {
  if (!has_pixels()) return err(status::unsupported_format);
  struct ctx_t {
    const mv_host_api* api;
    const std::string* path;
    std::int64_t ms;
    const rgb_frame* frame;
  } ctx{api_, &path, pts_ms, frame};
  return path_out(
      [](char* out, uint32_t cap, void* c) {
        auto* x = static_cast<ctx_t*>(c);
        return x->api->moment_thumbnail(x->api->host, x->path->c_str(), x->ms,
                                        x->frame ? x->frame->rgb.data() : nullptr,
                                        x->frame ? x->frame->width : 0,
                                        x->frame ? x->frame->height : 0, out, cap);
      },
      &ctx);
}

result<std::string> host::piece_dir(const std::string& piece) const {
  if (!has_pixels()) return err(status::io);
  struct ctx_t {
    const mv_host_api* api;
    const std::string* piece;
  } ctx{api_, &piece};
  return path_out(
      [](char* out, uint32_t cap, void* c) {
        auto* x = static_cast<ctx_t*>(c);
        return x->api->piece_dir(x->api->host, x->piece->c_str(), out, cap);
      },
      &ctx);
}

bool host::should_yield() const noexcept {
  return api_->should_yield && api_->should_yield(api_->host) != 0;
}

void host::post(mv_addon_event_kind kind, mv_status st, std::uint64_t id,
                std::int64_t payload) const noexcept {
  if (!api_->post_event) return;
  mv_addon_event e{};
  e.kind = static_cast<uint32_t>(kind);
  e.status = static_cast<uint32_t>(st);
  e.id = id;
  e.payload = payload;
  api_->post_event(api_->host, &e);
}

result<std::string> host::data_dir() const {
  struct ctx_t {
    const mv_host_api* api;
  } ctx{api_};
  return path_out(
      [](char* out, uint32_t cap, void* c) {
        auto* x = static_cast<ctx_t*>(c);
        return x->api->data_dir(x->api->host, out, cap);
      },
      &ctx);
}

}  // namespace mv::ai
