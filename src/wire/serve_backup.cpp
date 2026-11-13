// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "wire/serve_backup.h"

#include <algorithm>
#include <filesystem>
#include <vector>

#include "io/content_hash.h"
#include "io/file_port.h"

namespace mv::driftwire::serve::backup {

namespace {

std::filesystem::path fs_path(const std::string& s) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::uint64_t sub(std::uint64_t a, std::uint64_t b) noexcept { return a > b ? a - b : 0; }

}  // namespace

bool book::allowed(const root_config& root, const std::array<std::uint8_t, 32>& device) const {
  return root.backup && may_write && may_write(device);
}

std::uint64_t book::free_of(const std::string& path) const {
  if (free_bytes) return free_bytes(path);
  std::error_code ec;
  const auto s = std::filesystem::space(fs_path(path), ec);
  return ec ? 0 : static_cast<std::uint64_t>(s.available);
}

book::counted& book::count(const root_config& root, std::unique_lock<std::mutex>& lock) {
  auto& c = roots_[root.id];
  if (!c.known) {
    // Walked once per run, with no lock held: a big folder on a slow NAS
    // takes a while, and other folders' writes go on meanwhile.
    lock.unlock();
    const auto usage = io::tree_size(root.path);
    lock.lock();
    auto& again = roots_[root.id];
    if (!again.known) {
      again.known = true;
      // Bytes written during the walk were added already and may be in it
      // too: counted twice until the next start, which errs toward the limit.
      again.used += usage.bytes;
    }
    return again;
  }
  return c;
}

void book::space(const root_config& root, stat_info& out) {
  const std::uint64_t free = free_of(root.path);
  std::unique_lock lock(m_);
  auto& c = count(root, lock);
  out.has_space = true;
  out.limit = root.limit;
  out.used = c.used;
  out.room = std::min(sub(root.limit, c.used + c.held), sub(free, root.reserve));
}

book::verdict book::reserve(const root_config& root, std::uint64_t bytes, bool meta) {
  const std::uint64_t free = free_of(root.path);
  std::unique_lock lock(m_);
  auto& c = count(root, lock);
  if (!meta && c.used + c.held + std::max(bytes, kMinFileCost) > root.limit) return verdict::limit;
  // Held bytes are not on the disk yet: every folder's, since folders may share a volume.
  std::uint64_t held_all = 0;
  for (const auto& [id, other] : roots_) held_all += other.held;
  if (sub(free, held_all) < bytes + root.reserve) return verdict::full;
  c.held += bytes;
  return verdict::ok;
}

void book::wrote(const std::string& root_id, std::uint64_t bytes) {
  std::lock_guard lock(m_);
  auto& c = roots_[root_id];
  c.held = sub(c.held, bytes);
  c.used += bytes;
}

void book::release(const std::string& root_id, std::uint64_t bytes) {
  std::lock_guard lock(m_);
  auto& c = roots_[root_id];
  c.held = sub(c.held, bytes);
}

void book::removed(const std::string& root_id, std::uint64_t bytes) {
  std::lock_guard lock(m_);
  auto& c = roots_[root_id];
  c.used = sub(c.used, bytes);
}

void book::added(const std::string& root_id, std::uint64_t bytes) {
  std::lock_guard lock(m_);
  roots_[root_id].used += bytes;
}

bool book::claim(const std::string& part) {
  std::lock_guard lock(m_);
  return parts_.insert(part).second;
}

void book::drop(const std::string& part) {
  std::lock_guard lock(m_);
  parts_.erase(part);
}

std::unique_lock<std::mutex> book::lock_folder(const std::string& dir) {
  std::mutex* m = nullptr;
  {
    std::lock_guard lock(m_);
    auto& slot = folders_[dir];
    if (!slot) slot = std::make_unique<std::mutex>();
    m = slot.get();
  }
  return std::unique_lock<std::mutex>(*m);
}

std::optional<std::string> book::cached_hash(const std::string& path, std::uint64_t size, std::int64_t mtime_ns) {
  std::lock_guard lock(m_);
  const auto it = hashes_.find(path);
  if (it == hashes_.end() || it->second.size != size || it->second.mtime_ns != mtime_ns) return std::nullopt;
  return it->second.hash;
}

void book::remember_hash(const std::string& path, std::uint64_t size, std::int64_t mtime_ns, std::string hash) {
  std::lock_guard lock(m_);
  if (hashes_.size() >= kHashCache && hashes_.find(path) == hashes_.end()) hashes_.erase(hashes_.begin());
  hashes_[path] = {size, mtime_ns, std::move(hash)};
}

bool book::same_file(const std::string& path, std::uint64_t size, std::string_view hash) {
  const auto st = io::stat_beneath(std::string(io::parent_of(path)), std::string(io::file_name_of(path)));
  if (!st || st->is_directory || st->size != size) return false;
  if (const auto known = cached_hash(path, size, st->mtime_ns)) return *known == hash;
  const auto h = hash_file(path);
  if (!h) return false;
  remember_hash(path, size, st->mtime_ns, h.value());
  return h.value() == hash;
}

std::string numbered(std::string_view name, int n) {
  if (n <= 1) return std::string(name);
  const auto dot = name.rfind('.');
  // A leading dot is not an extension; neither is a name with no dot.
  const bool ext = dot != std::string_view::npos && dot > 0;
  std::string out(ext ? name.substr(0, dot) : name);
  out += " (" + std::to_string(n) + ")";
  if (ext) out += name.substr(dot);
  return out;
}

std::string part_name(std::string_view name, std::string_view device_id) {
  std::string out = ".";
  out += name;
  out += '.';
  out += device_id;
  out += ".mvpart";
  return out;
}

result<std::string> hash_file(const std::string& path) {
  io::file_reader r;
  if (auto o = r.open(path, io::read_mode::sequential); !o) return err(o.error());
  io::hasher h;
  std::vector<std::uint8_t> buf(1u << 20);
  for (;;) {
    auto got = r.read(buf);
    if (!got) return err(got.error());
    if (*got == 0) break;
    h.update(std::span<const std::uint8_t>(buf.data(), *got));
  }
  return h.finish().hex();
}

bool same_file(const std::string& path, std::uint64_t size, std::string_view hash) {
  std::error_code ec;
  const auto p = fs_path(path);
  const auto st = std::filesystem::symlink_status(p, ec);
  if (ec || !std::filesystem::is_regular_file(st)) return false;
  const auto bytes = std::filesystem::file_size(p, ec);
  if (ec || bytes != size) return false;
  const auto h = hash_file(path);
  return h && h.value() == hash;
}

std::optional<std::string> find_existing(const std::string& dir, std::string_view name, std::uint64_t size,
                                         std::string_view hash, book* cache) {
  for (int n = 1; n <= kMaxNumbered; ++n) {
    const std::string candidate = numbered(name, n);
    auto st = io::stat_beneath(dir, candidate);
    if (!st) return std::nullopt;  // the first free number: nothing past it is ours
    if (st->is_directory || st->size != size) continue;
    const std::string path = io::join_path(dir, candidate);
    if (cache ? cache->same_file(path, size, hash) : same_file(path, size, hash)) return candidate;
  }
  return std::nullopt;
}

commit_result commit_part(const std::string& dir, std::string_view name, const std::string& part, std::uint64_t size,
                          std::string_view hash, bool replace, book* cache) {
  commit_result out;
  std::error_code ec;
  const auto on_disk = hash_file(part);
  if (!on_disk) return out;
  if (on_disk.value() != hash) {
    // Not what was sent: the device's own unfinished copy goes, and it starts again.
    const auto bytes = std::filesystem::file_size(fs_path(part), ec);
    std::filesystem::remove(fs_path(part), ec);
    if (!ec) out.freed = bytes;
    out.k = commit_result::kind::bad_hash;
    return out;
  }
  if constexpr (io::kHasHiddenFlag) (void)io::set_hidden(part, false);
  if (replace) {
    const std::string target = io::join_path(dir, name);
    const auto old = std::filesystem::file_size(fs_path(target), ec);
    const std::uint64_t old_bytes = ec ? 0 : old;
    ec.clear();
    std::filesystem::rename(fs_path(part), fs_path(target), ec);
    if (ec) return out;
    out.k = commit_result::kind::placed;
    out.name = std::string(name);
    out.freed = old_bytes;
    return out;
  }
  for (int n = 1; n <= kMaxNumbered; ++n) {
    const std::string candidate = numbered(name, n);
    const std::string target = io::join_path(dir, candidate);
    auto renamed = io::rename_no_replace(part, target);
    if (!renamed) break;
    if (*renamed == io::rename_outcome::renamed) {
      out.k = commit_result::kind::placed;
      out.name = candidate;
      return out;
    }
    // Taken. By this very file (a commit whose answer was lost)? Then it is done.
    if (cache ? cache->same_file(target, size, hash) : same_file(target, size, hash)) {
      std::filesystem::remove(fs_path(part), ec);
      if (!ec) out.freed = size;
      out.k = commit_result::kind::existing;
      out.name = candidate;
      return out;
    }
  }
  if constexpr (io::kHasHiddenFlag) (void)io::set_hidden(part, true);
  return out;
}

}  // namespace mv::driftwire::serve::backup
