// SPDX-License-Identifier: GPL-2.0-or-later
#include "addons/import/host.h"

#include <algorithm>
#include <cstring>

namespace mv::import {
namespace {

int32_t MV_CALL cb_cancelled(void* user) {
  const auto* cb = static_cast<const copy_callbacks*>(user);
  return cb->cancelled && cb->cancelled() ? 1 : 0;
}
void MV_CALL cb_progress(void* user, uint64_t delta) {
  const auto* cb = static_cast<const copy_callbacks*>(user);
  if (cb->progress) cb->progress(delta);
}
void MV_CALL cb_yield(void* user) {
  const auto* cb = static_cast<const copy_callbacks*>(user);
  if (cb->yield) cb->yield();
}

int32_t MV_CALL walk_thunk(void* user, const mv_addon_file_entry* e) {
  const auto* fn = static_cast<const std::function<bool(const mv_addon_file_entry&)>*>(user);
  return (*fn)(*e) ? 1 : 0;
}

result<std::string> read_string(mv_status (MV_CALL* fn)(void*, char*, uint32_t), void* h) {
  if (!fn) return err(status::unsupported_format);
  std::string buf(4096, '\0');
  const mv_status s = fn(h, buf.data(), static_cast<uint32_t>(buf.size()));
  if (s != MV_OK) return err(to_status(s));
  buf.resize(std::strlen(buf.c_str()));
  return buf;
}

}  // namespace

expected host::walk(const std::string& root, int max_depth,
                    const std::function<bool(const mv_addon_file_entry&)>& visit) const {
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

result<digest> host::hash(const std::string& path, bool uncached, const copy_callbacks& cb) const {
  digest d{};
  const mv_status s =
      api_->hash_file(api_->host, path.c_str(), uncached ? 1u : 0u, &cb_cancelled, &cb_yield,
                      const_cast<void*>(static_cast<const void*>(&cb)), d.data());
  if (s != MV_OK) return err(to_status(s));
  return d;
}

expected host::copy(const std::string& src, const std::vector<std::string>& targets,
                    bool read_back, const copy_callbacks& cb, const copy_fault_request& fault,
                    mv_addon_copy_result& out) const {
  if (targets.empty() || targets.size() > MV_ADDON_COPY_MAX_TARGETS) return err(status::invalid_arg);
  const char* paths[MV_ADDON_COPY_MAX_TARGETS] = {};
  for (std::size_t i = 0; i < targets.size(); ++i) paths[i] = targets[i].c_str();
  mv_addon_copy_request req{};
  req.source_utf8 = src.c_str();
  req.targets_utf8 = paths;
  req.target_count = static_cast<uint32_t>(targets.size());
  req.read_back = read_back ? 1u : 0u;
  req.retries = 1;
  req.is_cancelled = &cb_cancelled;
  req.on_progress = &cb_progress;
  req.yield = &cb_yield;
  req.user = const_cast<void*>(static_cast<const void*>(&cb));
  req.fault_target = fault.target;
  req.fault_times = fault.times;
  req.fault_offset = fault.offset;
  out = mv_addon_copy_result{};
  const mv_status s = api_->copy_verified(api_->host, &req, &out);
  return s == MV_OK ? expected{} : err(to_status(s));
}

expected host::write_new_file(const std::string& path, std::string_view bytes) const {
  const mv_status s = api_->write_new_file(api_->host, path.c_str(), bytes.data(), bytes.size());
  return s == MV_OK ? expected{} : err(to_status(s));
}

result<mv_addon_volume> host::volume_of(const std::string& path) const {
  mv_addon_volume v{};
  const mv_status s = api_->volume_of(api_->host, path.c_str(), &v);
  if (s != MV_OK) return err(to_status(s));
  return v;
}

result<std::vector<mv_addon_volume>> host::list_volumes() const {
  std::vector<mv_addon_volume> v(64);
  uint32_t n = 0;
  const mv_status s = api_->list_volumes(api_->host, v.data(), static_cast<uint32_t>(v.size()), &n);
  if (s != MV_OK) return err(to_status(s));
  v.resize(std::min<std::size_t>(n, v.size()));
  return v;
}

expected host::eject(const std::string& root) const {
  const mv_status s = api_->eject_volume(api_->host, root.c_str());
  return s == MV_OK ? expected{} : err(to_status(s));
}

expected host::watch_volumes(void(MV_CALL* cb)(void*, std::uint32_t, const char*),
                             void* user) const {
  const mv_status s = api_->watch_volumes(api_->host, cb, user);
  return s == MV_OK ? expected{} : err(to_status(s));
}

bool host::capture(const std::string& path, mv_addon_capture& out) const {
  out = mv_addon_capture{};
  return api_->capture_info && api_->capture_info(api_->host, path.c_str(), &out) == MV_OK;
}

expected host::pair(const std::vector<std::string>& names, std::vector<std::uint32_t>& partner,
                    std::vector<std::uint32_t>& kind) const {
  std::vector<const char*> ptrs;
  ptrs.reserve(names.size());
  for (const auto& n : names) ptrs.push_back(n.c_str());
  partner.assign(names.size(), UINT32_MAX);
  kind.assign(names.size(), 0);
  if (names.empty()) return {};
  const mv_status s = api_->pair_names(api_->host, ptrs.data(), static_cast<uint32_t>(ptrs.size()),
                                       partner.data(), kind.data());
  return s == MV_OK ? expected{} : err(to_status(s));
}

result<std::string> host::thumbnail(const std::string& path) const {
  if (!api_->thumbnail_path) return err(status::unsupported_format);
  std::string buf(4096, '\0');
  const mv_status s =
      api_->thumbnail_path(api_->host, path.c_str(), buf.data(), static_cast<uint32_t>(buf.size()));
  if (s != MV_OK) return err(to_status(s));
  buf.resize(std::strlen(buf.c_str()));
  return buf;
}

bool host::should_yield() const noexcept {
  return api_->should_yield && api_->should_yield(api_->host) != 0;
}

void host::post(mv_addon_event_kind kind, mv_status s, std::uint64_t id,
                std::int64_t payload) const noexcept {
  if (!api_->post_event) return;
  mv_addon_event e{};
  e.kind = static_cast<uint32_t>(kind);
  e.status = static_cast<uint32_t>(s);
  e.id = id;
  e.payload = payload;
  api_->post_event(api_->host, &e);
}

result<std::string> host::data_dir() const { return read_string(api_->data_dir, api_->host); }

result<std::string> host::default_library_dir() const {
  return read_string(api_->default_library_dir, api_->host);
}

void host::log(int level, const char* ascii) const noexcept {
  if (api_->log) api_->log(api_->host, level, ascii);
}

}  // namespace mv::import
