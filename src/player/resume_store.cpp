// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Per-file video resume points: bounded, no plain-text paths (issue #235).
#include "player/resume_store.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "player/transport.h"

namespace mv::player {
namespace {

constexpr std::string_view kExt = ".pos";
constexpr std::string_view kLegacyExt = ".txt";
constexpr std::string_view kMagic = "mvr1";
// Trim again after this many writes, so a long session stays near the cap
// without scanning the folder on every clip close.
constexpr unsigned kPruneEvery = 64;

// The file name: FNV-1a 64 of the UTF-8 path (the pre-#235 name, kept stable).
std::uint64_t name_hash(std::string_view path) noexcept {
  std::uint64_t h = 14695981039346656037ull;
  for (const unsigned char c : path) { h ^= c; h *= 1099511628211ull; }
  return h;
}

// The check stored inside: a different mix, so two paths sharing a file name
// do not also share a check.
std::uint64_t check_hash(std::string_view path) noexcept {
  std::uint64_t h = 0x9E3779B97F4A7C15ull ^ path.size();
  for (const unsigned char c : path) {
    h = (h ^ c) * 0xBF58476D1CE4E5B9ull;
    h ^= h >> 31;
  }
  h ^= h >> 33; h *= 0x94D049BB133111EBull; h ^= h >> 29;
  return h;
}

std::filesystem::path entry_path(const std::filesystem::path& dir, std::string_view path) {
  char name[32];
  std::snprintf(name, sizeof name, "%016llx",
                static_cast<unsigned long long>(name_hash(path)));
  return dir / (std::string(name) + std::string(kExt));
}

std::atomic<unsigned> g_writes{0};
std::mutex g_prune_mutex;

}  // namespace

time_ns resume_load(const std::filesystem::path& dir, const resume_key& key,
                    time_ns duration_ns) noexcept {
  try {
    if (dir.empty() || key.path.empty()) return 0;
    std::ifstream input(entry_path(dir, key.path));
    if (!input) return 0;
    std::string magic;
    unsigned long long check = 0, size = 0;
    long long stamp = 0, position = 0;
    if (!(input >> magic >> std::hex >> check >> std::dec >> size >> stamp >> position)) return 0;
    if (magic != kMagic || check != check_hash(key.path) || size != key.size ||
        stamp != key.mtime) {
      return 0;
    }
    return resume_start_position(position, duration_ns);
  } catch (...) {
    return 0;
  }
}

void resume_save(const std::filesystem::path& dir, const resume_key& key,
                 time_ns position_ns, time_ns duration_ns) noexcept {
  try {
    if (dir.empty() || key.path.empty()) return;
    const auto file = entry_path(dir, key.path);
    std::error_code ec;
    if (!should_store_resume(position_ns, duration_ns)) {
      // Nothing worth resuming (the start, the credits, no duration): drop any
      // older entry rather than storing a zero.
      std::filesystem::remove(file, ec);
      return;
    }
    std::filesystem::create_directories(dir, ec);
    if (ec) return;
    {
      std::ofstream output(file, std::ios::trunc);
      if (!output) return;
      char line[128];
      std::snprintf(line, sizeof line, "%s %016llx %llu %lld %lld\n", kMagic.data(),
                    static_cast<unsigned long long>(check_hash(key.path)),
                    static_cast<unsigned long long>(key.size),
                    static_cast<long long>(key.mtime), static_cast<long long>(position_ns));
      output << line;
    }
    if (g_writes.fetch_add(1, std::memory_order_relaxed) % kPruneEvery == 0) {
      resume_prune(dir, resume_store_cap);
    }
  } catch (...) {
  }
}

void resume_prune(const std::filesystem::path& dir, std::size_t cap) noexcept {
  try {
    std::lock_guard lock(g_prune_mutex);
    std::error_code ec;
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> entries;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      const auto& path = it->path();
      const auto ext = path.extension().string();
      if (ext == kLegacyExt) {
        std::error_code rm; std::filesystem::remove(path, rm);
        continue;
      }
      if (ext != kExt) continue;
      std::error_code tec;
      const auto when = it->last_write_time(tec);
      if (!tec) entries.emplace_back(when, path);
    }
    if (entries.size() <= cap) return;
    const auto keep_from = entries.end() - static_cast<std::ptrdiff_t>(cap);
    std::nth_element(entries.begin(), keep_from, entries.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto it = entries.begin(); it != keep_from; ++it) {
      std::error_code rm; std::filesystem::remove(it->second, rm);
    }
  } catch (...) {
  }
}

}  // namespace mv::player
