// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/import/host.h"

#include <algorithm>
#include <cstring>

#include "addons/import/paths.h"

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

namespace {

constexpr char kKeySeparator = '\x1f';  // in neither a volume identity nor a sane folder name
constexpr std::string_view kKeyPrefix = "vol:";
constexpr std::string_view kUncPrefix = "net:\\\\";  // Windows: "net:" + "\\\\server\\share"

char folded(char c) noexcept {
  if (c == '\\') return '/';
#if defined(_WIN32)
  if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
#endif
  return c;
}

std::string_view trimmed(std::string_view p) noexcept {
  while (!p.empty() && (p.back() == '/' || p.back() == '\\')) p.remove_suffix(1);
  return p;
}

// `path` inside the mount at `root`, '/'-separated ("" at the mount itself).
std::optional<std::string> inside_of(std::string_view root, std::string_view path) {
  const std::string_view r = trimmed(root);
  const std::string_view p = trimmed(path);
  if (p.size() < r.size()) return std::nullopt;
  for (std::size_t i = 0; i < r.size(); ++i) {
    if (folded(r[i]) != folded(p[i])) return std::nullopt;
  }
  if (p.size() == r.size()) return std::string();
  if (p[r.size()] != '/' && p[r.size()] != '\\') return std::nullopt;
  std::string out(p.substr(r.size() + 1));
  std::replace(out.begin(), out.end(), '\\', '/');
  return out;
}

bool has_prefix(std::string_view s, std::string_view prefix) noexcept {
  return s.rfind(prefix, 0) == 0;
}

// A local volume's id survives a remount. A share has no such id (macOS's
// fsid is new at each mount), so it goes by what it is a mount of. Windows'
// id is "vsn:<serial>:<capacity>": the capacity tells two cards apart, but a
// resized partition or a changed quota is still this destination.
std::string volume_identity(const mv_addon_volume& v) {
  if (v.network && v.device_key[0]) return share_identity(v.device_key);
  std::string id(v.volume_id);
  if (has_prefix(id, "vsn:")) id.resize(std::min(id.find(':', 4), id.size()));
  return id;
}

bool is_share(std::string_view identity) noexcept {
  return has_prefix(identity, "dev:") || has_prefix(identity, "net:");
}

// Whether an identity will still mean this volume after a remount. Not an
// fsid (new at each mount), a serial of zero (no serial at all), or a share
// Windows could not name, which is left holding its drive letter.
bool survives_remount(std::string_view identity) noexcept {
  if (identity.empty() || has_prefix(identity, "fsid:") || identity == "vsn:00000000") return false;
  return !has_prefix(identity, "net:") || has_prefix(identity, kUncPrefix);
}

}  // namespace

std::string share_identity(std::string_view device_key) {
  std::string out(trimmed(device_key));
  // SMB and UNC names ignore case; an NFS export path ("nas:/Photos") does not.
  if (out.find("//") != std::string::npos || has_prefix(out, "net:")) {
    for (char& c : out) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
  }
  constexpr std::string_view kBonjour = "._smb._tcp";
  if (const auto b = out.find(kBonjour); b != std::string::npos) out.erase(b, kBonjour.size());
  return out;
}

dest_locator::dest_locator(const host& h) : h_(h) {
  if (auto v = h.list_volumes()) mounted_ = std::move(*v);
}

std::vector<std::string> dest_locator::roots_for(const std::string& identity) const {
  std::vector<std::string> out;
  for (const mv_addon_volume& m : mounted_) {
    if (volume_identity(m) == identity) out.emplace_back(m.root_utf8);
  }
  return out;
}

std::string dest_locator::key_of(const std::string& root) const {
  // A folder not made yet is on the volume of its nearest existing parent.
  std::string probe = root;
  auto v = h_.volume_of(probe);
  while (!v) {
    std::string up = parent_native(probe);
    if (up.empty() || up == probe) return root;
    probe = std::move(up);
    v = h_.volume_of(probe);
  }
  const std::string identity = volume_identity(*v);
  const auto inside = inside_of(v->root_utf8, root);
  if (!survives_remount(identity) || !inside) return root;
  std::vector<std::string> roots = roots_for(identity);
  if (roots.size() > 1 && !is_share(identity)) return root;  // two disks, one id: a clone
  // Windows lists drive letters only; a share used by UNC name is its own root.
  if (identity.rfind(kUncPrefix, 0) == 0) roots.push_back(identity.substr(4));
  for (const std::string& r : roots) {
    if (inside_of(r, root) == inside) {
      std::string key(kKeyPrefix);
      key += identity;
      key += kKeySeparator;
      key += *inside;
      return key;
    }
  }
  return root;
}

std::optional<std::string> dest_locator::live_root(const std::string& stored) const {
  if (!is_volume_key(stored)) return stored;
  const auto [it, added] = resolved_.try_emplace(stored);
  if (added) it->second = resolve(stored);
  return it->second;
}

std::optional<std::string> dest_locator::resolve(const std::string& key) const {
  const auto sep = key.find(kKeySeparator);
  if (sep == std::string::npos) return std::nullopt;
  const std::string identity = key.substr(kKeyPrefix.size(), sep - kKeyPrefix.size());
  const std::string inside = key.substr(sep + 1);
  std::vector<std::string> roots = roots_for(identity);
  // One share mounted twice is the same files. Two disks with one id are not,
  // and neither can be told to be the one this key meant.
  if (roots.size() > 1 && !is_share(identity)) return std::nullopt;
  if (roots.empty() && identity.rfind(kUncPrefix, 0) == 0) {
    std::string unc = identity.substr(4);
    if (auto st = h_.stat(unc); st && st->is_directory) roots.push_back(std::move(unc));
  }
  if (roots.empty()) return std::nullopt;
  return inside.empty() ? roots.front() : join_native(roots.front(), inside);
}

std::optional<std::string> dest_locator::volume_root(const std::string& volume_id) const {
  for (const mv_addon_volume& m : mounted_) {
    if (volume_id == m.volume_id) return std::string(m.root_utf8);
  }
  return std::nullopt;
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

// The add-on's copy of the table is zeroed past an older host's struct_size
// (addon_entry.cpp), so a NULL here is "this app has no bin call".
bool host::can_recycle() const noexcept { return api_->recycle_file != nullptr; }

result<bool> host::recycle(const std::string& path) const {
  if (!api_->recycle_file) return err(status::unsupported_format);
  uint32_t refused = 0;
  const mv_status s = api_->recycle_file(api_->host, path.c_str(), &refused);
  if (s != MV_OK) return err(to_status(s));
  return refused == 0;
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
