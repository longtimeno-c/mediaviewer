// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/package.h"

#include <algorithm>
#include <set>

namespace mv::addon {
namespace {

constexpr std::uint32_t kLocalSig = 0x04034b50;
constexpr std::uint32_t kCentralSig = 0x02014b50;
constexpr std::uint32_t kEndSig = 0x06054b50;
constexpr std::size_t kLocalSize = 30;
constexpr std::size_t kCentralSize = 46;
constexpr std::size_t kEndSize = 22;
constexpr std::uint16_t kFlagUtf8 = 1u << 11;  // the one flag a package may set

std::uint16_t u16(std::span<const std::uint8_t> b, std::size_t at) noexcept {
  return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}

std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t at) noexcept {
  return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
         (static_cast<std::uint32_t>(b[at + 2]) << 16) |
         (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

std::string folded(std::string_view path) {
  std::string out(path);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

}  // namespace

const package_entry* package_listing::find(std::string_view path) const noexcept {
  for (const package_entry& e : entries) {
    if (e.path == path) return &e;
  }
  return nullptr;
}

package_listing read_package(std::span<const std::uint8_t> p) {
  package_listing out;
  if (p.size() > kPackageMaxBytes) {
    out.why = rejection::too_large;
    return out;
  }
  // The end record is the last 22 bytes: a package carries no comment, so
  // there is one place it can be and nothing can hide after it.
  if (p.size() < kEndSize) return out;
  const std::size_t end = p.size() - kEndSize;
  if (u32(p, end) != kEndSig) return out;
  const std::uint16_t disk = u16(p, end + 4);
  const std::uint16_t cd_disk = u16(p, end + 6);
  const std::uint16_t here = u16(p, end + 8);
  const std::uint16_t total = u16(p, end + 10);
  const std::uint32_t cd_size = u32(p, end + 12);
  const std::uint32_t cd_at = u32(p, end + 16);
  const std::uint16_t comment = u16(p, end + 20);
  if (disk != 0 || cd_disk != 0 || here != total || comment != 0) return out;
  if (total == 0 || total == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_at == 0xFFFFFFFFu) return out;
  if (total > kPackageMaxEntries) {
    out.why = rejection::too_large;
    return out;
  }
  // The central directory ends exactly where the end record begins.
  if (static_cast<std::uint64_t>(cd_at) + cd_size != end) return out;

  std::set<std::string> seen;
  std::size_t at = cd_at;
  std::uint64_t next_local = 0;  // entries are laid end to end from byte 0
  out.entries.reserve(total);
  for (std::uint16_t i = 0; i < total; ++i) {
    if (end - at < kCentralSize || u32(p, at) != kCentralSig) return out;
    const std::uint16_t flags = u16(p, at + 8);
    const std::uint16_t method = u16(p, at + 10);
    const std::uint32_t crc = u32(p, at + 16);
    const std::uint32_t packed = u32(p, at + 20);
    const std::uint32_t size = u32(p, at + 24);
    const std::uint16_t name_len = u16(p, at + 28);
    const std::uint16_t extra_len = u16(p, at + 30);
    const std::uint16_t note_len = u16(p, at + 32);
    const std::uint16_t start_disk = u16(p, at + 34);
    const std::uint32_t local_at = u32(p, at + 42);
    if ((flags & ~kFlagUtf8) != 0 || method != 0 || packed != size || extra_len != 0 ||
        note_len != 0 || start_disk != 0 || name_len == 0) {
      return out;
    }
    if (end - at - kCentralSize < name_len) return out;
    const std::string_view name(reinterpret_cast<const char*>(p.data() + at + kCentralSize),
                                name_len);
    at += kCentralSize + name_len;
    // A package lists files; folders exist because files do. An entry that
    // ends in '/' is a folder entry.
    if (!safe_relative_path(name) || name.back() == '/') {
      out.why = rejection::unsafe_path;
      out.entries.clear();
      return out;
    }
    if (!seen.insert(folded(name)).second) return out;

    // The local header says the same as the central one, and sits where the
    // previous entry's bytes ended.
    if (local_at != next_local) return out;
    if (cd_at - static_cast<std::uint64_t>(local_at) < kLocalSize) return out;
    if (u32(p, local_at) != kLocalSig || u16(p, local_at + 6) != flags ||
        u16(p, local_at + 8) != method || u32(p, local_at + 14) != crc ||
        u32(p, local_at + 18) != packed || u32(p, local_at + 22) != size ||
        u16(p, local_at + 26) != name_len || u16(p, local_at + 28) != 0) {
      return out;
    }
    const std::uint64_t data_at = static_cast<std::uint64_t>(local_at) + kLocalSize + name_len;
    if (data_at > cd_at || cd_at - data_at < size) return out;
    if (!std::equal(name.begin(), name.end(),
                    reinterpret_cast<const char*>(p.data() + local_at + kLocalSize))) {
      return out;
    }
    next_local = data_at + size;
    out.entries.push_back(
        package_entry{std::string(name), p.subspan(static_cast<std::size_t>(data_at), size)});
  }
  // Nothing between the last entry and the central directory, and nothing in
  // the central directory after the last record.
  if (next_local != cd_at || at != end) {
    out.entries.clear();
    return out;
  }
  out.why = rejection::none;
  return out;
}

}  // namespace mv::addon
