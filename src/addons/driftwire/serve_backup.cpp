// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/serve_backup.h"

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

book::verdict book::reserve(const root_config& root, std::uint64_t bytes) {
  const std::uint64_t free = free_of(root.path);
  std::unique_lock lock(m_);
  auto& c = count(root, lock);
  if (c.used + c.held + bytes > root.limit) return verdict::limit;
  // Other writes' held bytes are not on the disk yet either.
  if (sub(free, c.held) < bytes + root.reserve) return verdict::full;
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

}  // namespace mv::driftwire::serve::backup
