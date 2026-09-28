// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/fcp/search_wire.h"

#include <algorithm>
#include <cstring>
#include <type_traits>

namespace mv::nle {
namespace {

// The fixed layouts on the wire. Little-endian on every host we build for;
// both ends are the same build of this file.
struct wire_header {
  std::uint32_t magic;           // 'MVSR'
  std::uint32_t version;
  std::uint64_t correlation_id;
  std::int32_t status;
  std::uint32_t row_count;
  std::uint32_t moment_count;
  std::uint32_t blob_bytes;
};

struct wire_row {
  std::int64_t pts_ms;
  std::int64_t duration_ms;
  float score;
  std::uint32_t kind;
  std::uint32_t more;
  std::uint32_t match;
  std::uint32_t path_offset;
  std::uint32_t path_len;
  std::uint32_t thumb_offset;
  std::uint32_t thumb_len;
  std::uint32_t moment_first;
  std::uint32_t moment_count;
};

struct wire_moment {
  std::int64_t pts_ms;
  float score;
  std::uint32_t reserved;
};

static_assert(std::is_trivially_copyable_v<wire_header> && sizeof(wire_header) == 32);
static_assert(std::is_trivially_copyable_v<wire_row> && sizeof(wire_row) == 56);
static_assert(std::is_trivially_copyable_v<wire_moment> && sizeof(wire_moment) == 16);

constexpr std::uint32_t kMagic = 0x5253564Du;  // "MVSR"

template <typename T>
void put(std::vector<std::uint8_t>& out, const T& v) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
  out.insert(out.end(), p, p + sizeof(T));
}

template <typename T>
T get(std::span<const std::uint8_t> bytes, std::size_t at) {
  T v;
  std::memcpy(&v, bytes.data() + at, sizeof(T));
  return v;
}

}  // namespace

std::vector<std::uint8_t> encode(const reply& r) {
  const std::size_t n = std::min<std::size_t>(r.rows.size(), kMaxRows);
  std::string blob;
  std::vector<wire_row> rows;
  std::vector<wire_moment> moments;
  rows.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const row& x = r.rows[i];
    wire_row w{};
    w.pts_ms = x.pts_ms;
    w.duration_ms = x.duration_ms;
    w.score = x.score;
    w.kind = x.kind;
    w.more = x.more;
    w.match = x.match;
    w.path_offset = static_cast<std::uint32_t>(blob.size());
    w.path_len = static_cast<std::uint32_t>(x.path.size());
    blob += x.path;
    w.thumb_offset = static_cast<std::uint32_t>(blob.size());
    w.thumb_len = static_cast<std::uint32_t>(x.thumb.size());
    blob += x.thumb;
    w.moment_first = static_cast<std::uint32_t>(moments.size());
    const std::size_t m = std::min<std::size_t>(x.moments.size(), kMaxMomentsPerRow);
    w.moment_count = static_cast<std::uint32_t>(m);
    for (std::size_t k = 0; k < m; ++k) moments.push_back(wire_moment{x.moments[k].pts_ms, x.moments[k].score, 0});
    rows.push_back(w);
  }
  wire_header h{};
  h.magic = kMagic;
  h.version = kWireVersion;
  h.correlation_id = r.correlation_id;
  h.status = static_cast<std::int32_t>(r.code);
  h.row_count = static_cast<std::uint32_t>(rows.size());
  h.moment_count = static_cast<std::uint32_t>(moments.size());
  h.blob_bytes = static_cast<std::uint32_t>(blob.size());
  std::vector<std::uint8_t> out;
  out.reserve(sizeof(h) + rows.size() * sizeof(wire_row) + moments.size() * sizeof(wire_moment) + blob.size());
  put(out, h);
  for (const wire_row& w : rows) put(out, w);
  for (const wire_moment& m : moments) put(out, m);
  out.insert(out.end(), blob.begin(), blob.end());
  return out;
}

result<reply> decode(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < sizeof(wire_header) || bytes.size() > kMaxReplyBytes) return err(status::corrupt);
  const auto h = get<wire_header>(bytes, 0);
  if (h.magic != kMagic) return err(status::corrupt);
  if (h.version != kWireVersion) return err(status::unsupported_format);
  if (h.row_count > kMaxRows || h.moment_count > kMaxRows * kMaxMomentsPerRow) return err(status::corrupt);
  const std::size_t rows_at = sizeof(wire_header);
  const std::size_t moments_at = rows_at + std::size_t{h.row_count} * sizeof(wire_row);
  const std::size_t blob_at = moments_at + std::size_t{h.moment_count} * sizeof(wire_moment);
  if (blob_at + h.blob_bytes != bytes.size()) return err(status::corrupt);
  const std::span<const std::uint8_t> blob = bytes.subspan(blob_at, h.blob_bytes);
  const auto text = [&](std::uint32_t at, std::uint32_t len, std::string& out) {
    if (std::size_t{at} + len > blob.size()) return false;
    out.assign(reinterpret_cast<const char*>(blob.data()) + at, len);
    return true;
  };
  reply r;
  r.version = h.version;
  r.correlation_id = h.correlation_id;
  r.code = static_cast<status>(h.status);
  r.rows.reserve(h.row_count);
  for (std::uint32_t i = 0; i < h.row_count; ++i) {
    const auto w = get<wire_row>(bytes, rows_at + std::size_t{i} * sizeof(wire_row));
    row x;
    if (!text(w.path_offset, w.path_len, x.path) || !text(w.thumb_offset, w.thumb_len, x.thumb)) {
      return err(status::corrupt);
    }
    if (w.moment_count > kMaxMomentsPerRow || std::size_t{w.moment_first} + w.moment_count > h.moment_count) {
      return err(status::corrupt);
    }
    x.pts_ms = w.pts_ms;
    x.duration_ms = w.duration_ms;
    x.score = w.score;
    x.kind = w.kind;
    x.more = w.more;
    x.match = w.match;
    x.moments.reserve(w.moment_count);
    for (std::uint32_t k = 0; k < w.moment_count; ++k) {
      const auto m = get<wire_moment>(bytes, moments_at + (std::size_t{w.moment_first} + k) * sizeof(wire_moment));
      x.moments.push_back(moment{m.pts_ms, m.score});
    }
    r.rows.push_back(std::move(x));
  }
  return r;
}

}  // namespace mv::nle
