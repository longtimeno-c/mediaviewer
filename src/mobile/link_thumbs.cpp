// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mobile/link_thumbs.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace mv::mobile {

namespace fs = std::filesystem;

namespace {
constexpr std::uint64_t kMergeGap = 32 * 1024;    // a gap worth reading through
constexpr std::uint64_t kMaxMerged = 2 * 1024 * 1024;
constexpr std::uint32_t kMaxTile = 8u * 1024 * 1024;  // a JPEG-512 is tens of kilobytes
constexpr std::size_t kMaxPackName = 64;

// thumbs.idx comes from the signed catalogue, but a name still becomes a path.
bool plausible(const ai::catalog::thumb_location& w) {
  if (w.length == 0 || w.length > kMaxTile || w.pack.empty() || w.pack.size() > kMaxPackName || w.pack.front() == '.') {
    return false;
  }
  return std::all_of(w.pack.begin(), w.pack.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
  });
}

// Pack name, offset and length together are what the pack says is this tile;
// a pack is named by its hash, so this key can never name other bytes.
std::string key_of(const ai::catalog::thumb_location& w) {
  return w.pack + "-" + std::to_string(w.offset) + "-" + std::to_string(w.length);
}

std::vector<std::uint8_t> read_whole(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return {};
  const auto size = f.tellg();
  if (size <= 0) return {};
  std::vector<std::uint8_t> out(static_cast<std::size_t>(size));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(out.data()), size);
  if (!f) return {};
  return out;
}
}  // namespace

struct thumb_cache::want {
  ai::catalog::thumb_location where;
  std::string key;
  int lane = 1;
  std::vector<std::pair<std::shared_ptr<cancel_flag>, done_fn>> waiters;
  bool in_flight = false;

  [[nodiscard]] bool abandoned() const {
    return std::all_of(waiters.begin(), waiters.end(), [](const auto& w) { return w.first->stopped(); });
  }
};

thumb_cache::thumb_cache(std::string dir, std::uint64_t capacity_bytes, remote_file& remote)
    : dir_(std::move(dir)), capacity_(capacity_bytes), remote_(remote) {
  std::error_code ec;
  fs::create_directories(dir_, ec);
  worker_a_ = std::thread([this] { worker(); });
  worker_b_ = std::thread([this] { worker(); });
}

thumb_cache::~thumb_cache() {
  {
    std::lock_guard lock(m_);
    stop_ = true;
  }
  cv_.notify_all();
  worker_a_.join();
  worker_b_.join();
  // Whatever was still queued hears that it will not come.
  for (auto& w : queue_) {
    for (auto& [flag, done] : w->waiters) done(err(status::cancelled));
  }
}

void thumb_cache::set_catalogue_root(std::string root) {
  {
    std::lock_guard lock(m_);
    root_ = std::move(root);
  }
  cv_.notify_all();
}

std::string thumb_cache::path_of(const std::string& name) const { return dir_ + "/" + name; }

std::shared_ptr<cancel_flag> thumb_cache::request(const ai::catalog::thumb_location& where, int lane, done_fn done) {
  auto flag = std::make_shared<cancel_flag>();
  if (!plausible(where)) {
    done(err(status::corrupt));  // this want fails; the worker and the others go on
    return flag;
  }
  const std::string key = key_of(where);
  // A disk hit needs no queue and no network, on the caller's own thread.
  auto cached = read_whole(path_of(key));
  if (!cached.empty()) {
    {
      std::lock_guard lock(m_);
      if (auto it = index_.find(key); it != index_.end()) lru_.splice(lru_.begin(), lru_, it->second);
    }
    done(std::move(cached));
    return flag;
  }
  {
    std::lock_guard lock(m_);
    if (auto it = by_key_.find(key); it != by_key_.end()) {
      it->second->lane = std::min(it->second->lane, lane);
      it->second->waiters.emplace_back(flag, std::move(done));
      return flag;
    }
    auto w = std::make_shared<want>();
    w->where = where;
    w->key = key;
    w->lane = lane;
    w->waiters.emplace_back(flag, std::move(done));
    by_key_[key] = w;
    queue_.push_back(std::move(w));
  }
  cv_.notify_one();
  return flag;
}

void thumb_cache::worker() {
  scan();
  for (;;) {
    std::vector<std::shared_ptr<want>> batch;
    std::vector<done_fn> dropped;  // told "cancelled" outside the lock: every request hears once
    std::string root;
    {
      std::unique_lock lock(m_);
      cv_.wait(lock, [this] { return stop_ || (!queue_.empty() && !root_.empty()); });
      if (stop_) break;
      root = root_;
      // Abandoned wants go first, so a fast scroll leaves nothing behind.
      std::erase_if(queue_, [this, &dropped](const std::shared_ptr<want>& w) {
        if (!w->abandoned()) return false;
        for (auto& [flag, done] : w->waiters) dropped.push_back(std::move(done));
        w->waiters.clear();
        by_key_.erase(w->key);
        return true;
      });
    }
    for (auto& done : dropped) done(err(status::cancelled));
    {
      std::unique_lock lock(m_);
      if (stop_) break;
      if (queue_.empty()) continue;
      // The lead: what is on screen before anything else, then the oldest.
      auto lead = std::min_element(queue_.begin(), queue_.end(), [](const auto& a, const auto& b) { return a->lane < b->lane; });
      const std::string pack = (*lead)->where.pack;
      // Same-pack wants, nearest first from the lead, merged while the gaps are small.
      std::vector<std::shared_ptr<want>> same;
      for (const auto& w : queue_) {
        if (w->where.pack == pack) same.push_back(w);
      }
      std::sort(same.begin(), same.end(), [](const auto& a, const auto& b) { return a->where.offset < b->where.offset; });
      auto at = std::find(same.begin(), same.end(), *lead);
      std::size_t lo = static_cast<std::size_t>(at - same.begin()), hi = lo + 1;
      std::uint64_t start = (*at)->where.offset, end = start + (*at)->where.length;
      // Grow to both sides while the merge stays small.
      for (bool grew = true; grew;) {
        grew = false;
        if (hi < same.size() && same[hi]->where.offset <= end + kMergeGap &&
            std::max(end, same[hi]->where.offset + same[hi]->where.length) - start <= kMaxMerged) {
          end = std::max(end, same[hi]->where.offset + same[hi]->where.length);
          ++hi;
          grew = true;
        }
        if (lo > 0 && same[lo - 1]->where.offset + same[lo - 1]->where.length + kMergeGap >= start &&
            end - same[lo - 1]->where.offset <= kMaxMerged) {
          start = same[lo - 1]->where.offset;
          --lo;
          grew = true;
        }
      }
      batch.assign(same.begin() + static_cast<std::ptrdiff_t>(lo), same.begin() + static_cast<std::ptrdiff_t>(hi));
      for (const auto& w : batch) {
        w->in_flight = true;
        std::erase(queue_, w);
      }
    }
    // One read, cut into the wanted pieces as it arrives.
    const std::uint64_t first = batch.front()->where.offset;
    std::uint64_t last = 0;
    for (const auto& w : batch) last = std::max(last, w->where.offset + w->where.length);
    std::vector<std::uint8_t> got(static_cast<std::size_t>(last - first));
    std::uint64_t filled = 0;
    auto r = remote_.read_range(
        root, "thumbs/" + batch.front()->where.pack, first, last - first,
        [&](std::uint64_t offset, std::span<const std::uint8_t> bytes) {
          if (offset != first + filled || filled + bytes.size() > got.size()) return false;
          std::copy(bytes.begin(), bytes.end(), got.begin() + static_cast<std::ptrdiff_t>(filled));
          filled += bytes.size();
          return true;
        },
        nullptr, false);
    const bool whole = r && filled == got.size();
    std::vector<std::pair<std::string, std::uint64_t>> stored;
    for (const auto& w : batch) {
      result<std::vector<std::uint8_t>> out = err(whole ? status::corrupt : (r ? status::io : r.error()));
      if (whole) {
        const auto at = static_cast<std::ptrdiff_t>(w->where.offset - first);
        std::vector<std::uint8_t> piece(got.begin() + at, got.begin() + at + static_cast<std::ptrdiff_t>(w->where.length));
        const std::string path = path_of(w->key);
        std::ofstream f(path + ".part", std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(piece.data()), static_cast<std::streamsize>(piece.size()));
        f.close();
        std::error_code ec;
        if (f) {
          fs::rename(path + ".part", path, ec);
          if (!ec) stored.emplace_back(w->key, piece.size());
        } else {
          fs::remove(path + ".part", ec);
        }
        out = std::move(piece);
      }
      std::vector<std::pair<std::shared_ptr<cancel_flag>, done_fn>> waiters;
      {
        std::lock_guard lock(m_);
        by_key_.erase(w->key);
        waiters = std::move(w->waiters);
      }
      for (auto& [flag, done] : waiters) {
        if (flag->stopped()) {
          done(err(status::cancelled));
        } else {
          done(out.has_value() ? result<std::vector<std::uint8_t>>(*out) : result<std::vector<std::uint8_t>>(err(out.error())));
        }
      }
    }
    if (!stored.empty()) {
      std::lock_guard lock(m_);
      for (const auto& [name, size] : stored) admit(name, size);
      trim_locked();
    }
  }
}

void thumb_cache::admit(const std::string& name, std::uint64_t size) {
  if (auto it = index_.find(name); it != index_.end()) {
    lru_.splice(lru_.begin(), lru_, it->second);
    return;
  }
  lru_.emplace_front(name, size);
  index_[name] = lru_.begin();
  used_ += size;
}

void thumb_cache::trim_locked() {
  if (used_ <= capacity_) return;
  const std::uint64_t target = capacity_ - capacity_ / 10;
  std::error_code ec;
  while (used_ > target && !lru_.empty()) {
    const auto [name, size] = lru_.back();
    fs::remove(path_of(name), ec);
    index_.erase(name);
    lru_.pop_back();
    used_ -= size;
  }
}

void thumb_cache::scan() {
  std::vector<std::pair<fs::file_time_type, std::pair<std::string, std::uint64_t>>> found;
  std::error_code ec;
  for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().extension() == ".part") {
      std::error_code e2;
      fs::remove(it->path(), e2);
      continue;
    }
    std::error_code e2;
    const auto when = it->last_write_time(e2);
    const auto size = it->file_size(e2);
    if (!e2) found.push_back({when, {it->path().filename().string(), size}});
  }
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::lock_guard lock(m_);
  if (scanned_) return;
  scanned_ = true;
  for (const auto& [when, entry] : found) {
    if (index_.count(entry.first)) continue;
    lru_.push_back(entry);
    index_[entry.first] = std::prev(lru_.end());
    used_ += entry.second;
  }
  trim_locked();
}

std::uint64_t thumb_cache::bytes_used() const {
  std::lock_guard lock(m_);
  return used_;
}

void thumb_cache::clear() {
  std::lock_guard lock(m_);
  std::error_code ec;
  fs::remove_all(dir_, ec);
  fs::create_directories(dir_, ec);
  lru_.clear();
  index_.clear();
  used_ = 0;
}

}  // namespace mv::mobile
