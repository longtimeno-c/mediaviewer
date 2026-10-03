// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "codec/zip.h"

#include <zlib.h>

#include <algorithm>
#include <new>

namespace mv::codec::zip {
namespace {

std::uint16_t u16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t u32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

constexpr std::uint32_t kEndSig = 0x06054b50;
constexpr std::uint32_t kCentralSig = 0x02014b50;
constexpr std::uint32_t kLocalSig = 0x04034b50;
constexpr std::size_t kEndBytes = 22;
constexpr std::size_t kCentralBytes = 46;
constexpr std::size_t kLocalBytes = 30;
constexpr std::size_t kMaxEntries = 65535;

}  // namespace

result<archive> archive::open(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < kEndBytes) return err(status::corrupt);
  // The end record is in the last 22 + 65535 (comment) bytes.
  const std::size_t floor = bytes.size() > kEndBytes + 65535 ? bytes.size() - kEndBytes - 65535 : 0;
  std::size_t end = bytes.size() - kEndBytes;
  for (;; --end) {
    if (u32(bytes.data() + end) == kEndSig) break;
    if (end == floor) return err(status::corrupt);
  }
  const std::uint8_t* e = bytes.data() + end;
  const std::uint16_t count = u16(e + 10);
  const std::uint32_t dir_size = u32(e + 12);
  const std::uint32_t dir_offset = u32(e + 16);
  if (count == 0xFFFF || dir_offset == 0xFFFFFFFFu) return err(status::unsupported_format);  // zip64
  if (static_cast<std::uint64_t>(dir_offset) + dir_size > end) return err(status::corrupt);

  archive out;
  out.bytes_ = bytes;
  try {
    out.entries_.reserve(std::min<std::size_t>(count, kMaxEntries));
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
  std::size_t at = dir_offset;
  const std::size_t dir_end = static_cast<std::size_t>(dir_offset) + dir_size;
  for (std::uint16_t i = 0; i < count; ++i) {
    if (at + kCentralBytes > dir_end) return err(status::corrupt);
    const std::uint8_t* c = bytes.data() + at;
    if (u32(c) != kCentralSig) return err(status::corrupt);
    const std::uint16_t flags = u16(c + 8);
    const std::uint16_t name_len = u16(c + 28);
    const std::uint16_t extra_len = u16(c + 30);
    const std::uint16_t comment_len = u16(c + 32);
    const std::size_t next = at + kCentralBytes + name_len + extra_len + comment_len;
    if (next > dir_end) return err(status::corrupt);
    entry item;
    item.method = u16(c + 10);
    item.compressed = u32(c + 20);
    item.size = u32(c + 24);
    item.local_offset = u32(c + 42);
    if (flags & 1u) item.method = 0xFFFF;  // encrypted: never read
    item.name.assign(reinterpret_cast<const char*>(c + kCentralBytes), name_len);
    out.entries_.push_back(std::move(item));
    at = next;
  }
  return out;
}

const entry* archive::find(std::string_view name) const noexcept {
  for (const entry& e : entries_) {
    if (e.name == name) return &e;
  }
  // Part names are case-insensitive in OPC.
  for (const entry& e : entries_) {
    if (e.name.size() != name.size()) continue;
    bool same = true;
    for (std::size_t i = 0; i < name.size() && same; ++i) {
      char a = e.name[i], b = name[i];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
      same = a == b;
    }
    if (same) return &e;
  }
  return nullptr;
}

result<std::vector<std::uint8_t>> archive::read(const entry& e, std::size_t cap) const {
  if (e.method != 0 && e.method != 8) return err(status::unsupported_format);
  if (e.size > cap) return err(status::unsupported_format);
  const std::size_t local = e.local_offset;
  if (local + kLocalBytes > bytes_.size()) return err(status::corrupt);
  const std::uint8_t* l = bytes_.data() + local;
  if (u32(l) != kLocalSig) return err(status::corrupt);
  const std::size_t data = local + kLocalBytes + u16(l + 26) + u16(l + 28);
  if (data > bytes_.size() || e.compressed > bytes_.size() - data) return err(status::corrupt);
  const std::uint8_t* src = bytes_.data() + data;

  std::vector<std::uint8_t> out;
  try {
    out.resize(e.size);
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
  if (e.method == 0) {
    if (e.compressed != e.size) return err(status::corrupt);
    std::copy(src, src + e.size, out.begin());
    return out;
  }

  z_stream z{};
  if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return err(status::out_of_memory);
  z.next_in = const_cast<Bytef*>(src);
  z.avail_in = e.compressed;
  z.next_out = out.data();
  z.avail_out = e.size;
  const int rc = inflate(&z, Z_FINISH);
  const auto produced = z.total_out;
  inflateEnd(&z);
  // Exactly the declared size: more would have been cut off by avail_out, less
  // means a truncated or lying entry.
  if (rc != Z_STREAM_END || produced != e.size) return err(status::corrupt);
  return out;
}

result<std::vector<std::uint8_t>> archive::read(std::string_view name, std::size_t cap) const {
  const entry* e = find(name);
  if (!e) return err(status::unsupported_format);
  return read(*e, cap);
}

}  // namespace mv::codec::zip
