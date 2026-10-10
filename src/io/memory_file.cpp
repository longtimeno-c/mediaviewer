// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "io/memory_file.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <new>

namespace mv::io {
namespace {

struct held {
  std::string key;
  std::shared_ptr<const std::vector<std::uint8_t>> bytes;
  std::int64_t mtime_ns = 0;
};

std::mutex g_m;
held g_file;
std::uint64_t g_count = 0;

}  // namespace

std::string put_memory_file(std::vector<std::uint8_t> bytes, std::string_view name) {
  auto shared = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
  const std::int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
  std::shared_ptr<const std::vector<std::uint8_t>> released;
  std::lock_guard lock(g_m);
  std::string key(kMemoryScheme);
  key += std::to_string(++g_count);
  key.push_back('/');
  key.append(name);
  // The old bytes are freed outside the lock (a 60 MB vector is not instant).
  released = std::move(g_file.bytes);
  g_file = held{key, std::move(shared), now};
  return key;
}

std::shared_ptr<const std::vector<std::uint8_t>> memory_file(std::string_view key) noexcept {
  std::lock_guard lock(g_m);
  if (g_file.bytes == nullptr || key != g_file.key) return nullptr;
  return g_file.bytes;
}

bool memory_file_stat(std::string_view key, memory_file_stamp& out) noexcept {
  std::lock_guard lock(g_m);
  if (g_file.bytes == nullptr || key != g_file.key) return false;
  out.size = g_file.bytes->size();
  out.mtime_ns = g_file.mtime_ns;
  return true;
}

result<std::vector<std::uint8_t>> read_memory_prefix(std::string_view key, std::size_t max_bytes) {
  const std::shared_ptr<const std::vector<std::uint8_t>> bytes = memory_file(key);
  if (bytes == nullptr) return err(status::not_found);
  if (bytes->empty()) return err(status::corrupt);
  const std::size_t n = max_bytes > 0 ? std::min(max_bytes, bytes->size()) : bytes->size();
  std::vector<std::uint8_t> out;
  try {
    out.assign(bytes->begin(), bytes->begin() + static_cast<std::ptrdiff_t>(n));
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
  return out;
}

void clear_memory_files() noexcept {
  std::shared_ptr<const std::vector<std::uint8_t>> released;
  std::lock_guard lock(g_m);
  released = std::move(g_file.bytes);
  g_file = held{};
}

}  // namespace mv::io
