// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote/cache.h"

#include <sodium.h>

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <vector>

namespace mv::remote {

namespace fs = std::filesystem;

namespace {

fs::path fs_path(std::string_view utf8) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string utf8_of(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

std::string leaf_of(std::string_view rel) {
  const std::size_t slash = rel.rfind('/');
  return std::string(slash == std::string_view::npos ? rel : rel.substr(slash + 1));
}

constexpr std::string_view kTiles = "tiles";

}  // namespace

disk_cache::disk_cache(std::string dir, std::uint64_t cap_bytes) : dir_(std::move(dir)), cap_(cap_bytes) {}

std::string disk_cache::hash_of(const identity& id) {
  // BLAKE2b-128 over the parts, each length-prefixed so no two identities share bytes.
  crypto_generichash_state st;
  crypto_generichash_init(&st, nullptr, 0, 16);
  const auto feed = [&](std::string_view s) {
    const std::uint64_t n = s.size();
    crypto_generichash_update(&st, reinterpret_cast<const unsigned char*>(&n), sizeof n);
    crypto_generichash_update(&st, reinterpret_cast<const unsigned char*>(s.data()), s.size());
  };
  feed(id.root);
  feed(id.rel);
  crypto_generichash_update(&st, reinterpret_cast<const unsigned char*>(&id.size), sizeof id.size);
  crypto_generichash_update(&st, reinterpret_cast<const unsigned char*>(&id.mtime), sizeof id.mtime);
  unsigned char h[16];
  crypto_generichash_final(&st, h, sizeof h);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(32, '0');
  for (std::size_t i = 0; i < sizeof h; ++i) {
    out[2 * i] = kHex[h[i] >> 4];
    out[2 * i + 1] = kHex[h[i] & 15];
  }
  return out;
}

std::string disk_cache::original_path(const identity& id) const {
  return utf8_of(fs_path(dir_) / fs_path(id.server) / fs_path(hash_of(id)) / fs_path(leaf_of(id.rel)));
}

std::string disk_cache::tile_path(const identity& id) const {
  return utf8_of(fs_path(dir_) / fs_path(id.server) / fs_path(kTiles) / fs_path(hash_of(id) + ".jpg"));
}

void disk_cache::scan_locked() {
  if (scanned_) return;
  scanned_ = true;
  std::error_code ec;
  // <dir>/<server>/<h>/<name>: two levels down, tiles and part files left out.
  std::vector<std::pair<fs::file_time_type, std::pair<std::string, std::uint64_t>>> found;
  for (fs::directory_iterator s(fs_path(dir_), ec), end; !ec && s != end; s.increment(ec)) {
    if (!s->is_directory(ec)) continue;
    for (fs::directory_iterator h(s->path(), ec), hend; !ec && h != hend; h.increment(ec)) {
      if (!h->is_directory(ec) || h->path().filename() == fs_path(kTiles)) continue;
      for (fs::directory_iterator f(h->path(), ec), fend; !ec && f != fend; f.increment(ec)) {
        if (!f->is_regular_file(ec)) continue;
        if (f->path().extension() == fs_path(".part")) {
          fs::remove(f->path(), ec);  // a fetch cut short by a quit
          continue;
        }
        const auto bytes = f->file_size(ec);
        if (ec) continue;
        found.push_back({f->last_write_time(ec), {utf8_of(f->path()), bytes}});
      }
      ec.clear();
    }
    ec.clear();
  }
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  for (auto& [t, entry] : found) {
    (void)t;
    lru_.push_back(entry);
    index_[lru_.back().first] = std::prev(lru_.end());
    used_ += entry.second;
  }
}

std::optional<std::string> disk_cache::find(const identity& id) {
  const std::string path = original_path(id);
  std::error_code ec;
  const auto size = fs::file_size(fs_path(path), ec);
  if (ec || size != id.size) return std::nullopt;
  std::lock_guard lock(m_);
  scan_locked();
  if (auto it = index_.find(path); it != index_.end()) {
    lru_.splice(lru_.begin(), lru_, it->second);
  } else {
    lru_.emplace_front(path, size);
    index_[path] = lru_.begin();
    used_ += size;
  }
  return path;
}

bool disk_cache::admit(const identity& id, const std::string& part_path) {
  const std::string path = original_path(id);
  std::error_code ec;
  const auto size = fs::file_size(fs_path(part_path), ec);
  if (ec || size != id.size) {
    fs::remove(fs_path(part_path), ec);
    return false;
  }
  fs::rename(fs_path(part_path), fs_path(path), ec);
  if (ec) {
    fs::remove(fs_path(part_path), ec);
    return false;
  }
  std::lock_guard lock(m_);
  scan_locked();
  if (auto it = index_.find(path); it != index_.end()) {
    used_ -= it->second->second;
    lru_.erase(it->second);
    index_.erase(it);
  }
  lru_.emplace_front(path, size);
  index_[path] = lru_.begin();
  used_ += size;
  trim_locked(path);
  return true;
}

void disk_cache::trim_locked(const std::string& keep) {
  std::error_code ec;
  auto it = lru_.end();
  while (used_ > cap_ && it != lru_.begin()) {
    --it;
    if (it->first == keep) continue;
    // A file another process holds open on Windows cannot go: it stays listed,
    // and the next admit tries again.
    fs::remove(fs_path(it->first), ec);
    if (ec && fs::exists(fs_path(it->first))) continue;
    fs::remove(fs_path(it->first).parent_path(), ec);  // its <h> folder, now empty
    used_ -= it->second;
    index_.erase(it->first);
    it = lru_.erase(it);
  }
}

void disk_cache::remove_server(std::string_view server) {
  std::lock_guard lock(m_);
  const std::string prefix = utf8_of(fs_path(dir_) / fs_path(server));
  for (auto it = lru_.begin(); it != lru_.end();) {
    if (it->first.rfind(prefix, 0) == 0) {
      used_ -= it->second;
      index_.erase(it->first);
      it = lru_.erase(it);
    } else {
      ++it;
    }
  }
  std::error_code ec;
  fs::remove_all(fs_path(prefix), ec);
}

std::uint64_t disk_cache::bytes_used() {
  std::lock_guard lock(m_);
  scan_locked();
  return used_;
}

}  // namespace mv::remote
