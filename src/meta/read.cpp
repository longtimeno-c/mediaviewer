// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>

#include "codec/decode.h"
#include "codec/format.h"
#include "io/file.h"
#include "meta/internal.h"
#include "meta/write.h"

namespace mv::meta {
namespace {

// Head-resident metadata (JPEG APPn, PNG chunks, WebP, HEIC meta box) fits in
// a prefix; TIFF-container RAW keeps IFDs wherever it likes, so a failed
// prefix read retries on the whole file, up to a cap.
constexpr std::size_t kPrefixBytes = 8u * 1024 * 1024;
constexpr std::size_t kWholeFileCap = 512u * 1024 * 1024;
constexpr std::size_t kDatePrefixBytes = 256u * 1024;

// UTF-8 in, on both hosts: the char8_t constructor is the portable spelling.
std::uint64_t file_size_of(std::string_view utf8_path) {
  std::error_code ec;
  const std::filesystem::path p{std::u8string(reinterpret_cast<const char8_t*>(utf8_path.data()),
                                              utf8_path.size())};
  const auto n = std::filesystem::file_size(p, ec);
  return ec ? 0 : static_cast<std::uint64_t>(n);
}

}  // namespace

result<metadata> read(std::string_view utf8_path) {
  auto prefix = io::read_prefix(utf8_path, kPrefixBytes);
  // read_prefix calls a zero-length file `corrupt`. It is a file with nothing
  // in it, so it has nothing to say: an empty record, like any other file with
  // no metadata. Only a file that cannot be read at all is an error.
  if (!prefix && prefix.error() == status::corrupt) {
    metadata empty;
    empty.s.file_size = 0;
    return empty;
  }
  if (!prefix) return err(prefix.error());
  const std::span<const std::uint8_t> head(prefix->data(), std::min<std::size_t>(prefix->size(), 1024));
  const codec::format_family family = codec::probe(head);
  metadata out;
  out.s.file_size = file_size_of(utf8_path);
  out.s.format = codec::format_name(family);
  if (family == codec::format_family::unknown) {
    // Not a still we know: a clip, or a file with nothing to say. Either way
    // the answer is a (possibly empty) metadata, not an error.
    (void)detail::read_clip(utf8_path, out);  // false = nothing to say
    detail::overlay_sidecar(utf8_path, out);
    return out;
  }

  const bool raw = family == codec::format_family::raw || codec::looks_like_raw(*prefix);
  // RAW (LibRaw's flip) and, from PR 10, JPEG (codec/orient.h) reach the
  // screen already rotated by their EXIF orientation.
  const bool oriented = raw || family == codec::format_family::jpeg;
  detail::read_still(*prefix, oriented, out);
  if (out.properties.empty() && prefix->size() >= kPrefixBytes) {
    if (auto whole = io::read_prefix(utf8_path, kWholeFileCap)) detail::read_still(*whole, oriented, out);
  }
  detail::overlay_sidecar(utf8_path, out);
  return out;
}

std::optional<std::int64_t> read_date_taken(std::string_view utf8_path) {
  auto prefix = io::read_prefix(utf8_path, kDatePrefixBytes);
  if (!prefix) return std::nullopt;
  const std::span<const std::uint8_t> head(prefix->data(), std::min<std::size_t>(prefix->size(), 1024));
  const codec::format_family family = codec::probe(head);
  if (family == codec::format_family::unknown) return detail::clip_date_key(utf8_path);
  if (auto k = detail::still_date_key(*prefix)) return k;
  if (family == codec::format_family::tiff || family == codec::format_family::raw) {
    if (auto more = io::read_prefix(utf8_path, kPrefixBytes)) return detail::still_date_key(*more);
  }
  return std::nullopt;
}

}  // namespace mv::meta
