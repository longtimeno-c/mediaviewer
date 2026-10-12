// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <exiv2/exiv2.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>

#include "codec/decode.h"
#include "codec/format.h"
#include "io/file.h"
#include "io/file_port.h"
#include "meta/generators.h"
#include "meta/internal.h"
#include "meta/search.h"
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

// #290: the ISO-BMFF walk reads box headers at offsets, never the whole file.
class file_source final : public parse::byte_source {
 public:
  bool open(std::string_view utf8_path) { return f_.open(utf8_path, io::read_mode::sequential).has_value(); }
  [[nodiscard]] std::uint64_t size() const noexcept override { return f_.size(); }
  [[nodiscard]] std::size_t read_at(std::uint64_t offset, std::span<std::uint8_t> into) noexcept override {
    auto n = f_.read_at(offset, into);
    return n ? *n : 0;
  }

 private:
  io::file_reader f_;
};

bool is_bmff(std::span<const std::uint8_t> head) noexcept {
  return head.size() >= 12 && head[4] == 'f' && head[5] == 't' && head[6] == 'y' && head[7] == 'p';
}

// #290: what Exiv2 and libavformat do not surface (meta/parse.h), then the
// provenance rule (generators.h). Runs over the prefix already in memory; only
// ISO-BMFF larger than the prefix opens the file again, for a few header reads
// and the boxes it keeps.
void read_beyond(std::string_view utf8_path, std::span<const std::uint8_t> prefix, codec::format_family family,
                 parse::evidence& ev, metadata& out) {
  parse::sink rows{out.properties, ev, 4000};
  switch (family) {
    case codec::format_family::jpeg: parse::read_jpeg(prefix, rows); break;
    case codec::format_family::png: parse::read_png(prefix, rows); break;
    case codec::format_family::webp: parse::read_webp(prefix, rows); break;
    default: break;
  }
  const bool still_bmff = family == codec::format_family::heic || family == codec::format_family::avif;
  if (still_bmff || (family == codec::format_family::unknown && is_bmff(prefix))) {
    if (out.s.file_size != 0 && prefix.size() >= out.s.file_size) {
      // The whole file is already in memory (most stills): no second open.
      parse::memory_source m(prefix);
      parse::read_bmff(m, still_bmff, rows);
    } else {
      file_source f;
      if (f.open(utf8_path)) parse::read_bmff(f, still_bmff, rows);
    }
  }
  const provenance p = derive_provenance(ev);
  out.s.made_with = p.made_with;
  out.s.ai_generated = p.ai_generated;
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
  parse::evidence ev;
  if (family == codec::format_family::unknown) {
    // Not a still we know: a clip, or a file with nothing to say. Either way
    // the answer is a (possibly empty) metadata, not an error.
    (void)detail::read_clip(utf8_path, out, &ev);  // false = nothing to say
    read_beyond(utf8_path, *prefix, family, ev, out);
    detail::overlay_sidecar(utf8_path, out);
    return out;
  }

  const bool raw = family == codec::format_family::raw || codec::looks_like_raw(*prefix);
  // RAW (LibRaw's flip) and, from PR 10, JPEG (codec/orient.h) reach the
  // screen already rotated by their EXIF orientation.
  const bool oriented = raw || family == codec::format_family::jpeg;
  detail::read_still(*prefix, oriented, out, &ev);
  if (out.properties.empty() && prefix->size() >= kPrefixBytes) {
    if (auto whole = io::read_prefix(utf8_path, kWholeFileCap)) detail::read_still(*whole, oriented, out, &ev);
  }
  read_beyond(utf8_path, *prefix, family, ev, out);
  detail::overlay_sidecar(utf8_path, out);
  if (family == codec::format_family::jpeg) {
    auto target = write_target_for(utf8_path);
    out.writes_in_file = target && *target == write_target::in_file;
  }
  return out;
}

// #291: the folder pass's read (search.h). The head of the file holds a
// still's EXIF / IPTC / XMP / C2PA (JPEG APPn, PNG chunks, the HEIF meta box);
// the scan data after it is never read. No write-target probe: nothing here
// edits.
result<search_facts> read_search_facts(std::string_view utf8_path) {
  auto prefix = io::read_prefix(utf8_path, kSearchPrefixBytes);
  if (!prefix && prefix.error() == status::corrupt) return search_facts{};  // empty file
  if (!prefix) return err(prefix.error());
  const std::span<const std::uint8_t> head(prefix->data(), std::min<std::size_t>(prefix->size(), 1024));
  const codec::format_family family = codec::probe(head);
  metadata out;
  out.s.file_size = prefix->size() < kSearchPrefixBytes ? prefix->size() : file_size_of(utf8_path);
  parse::evidence ev;
  if (family == codec::format_family::unknown) {
    (void)detail::read_clip(utf8_path, out, &ev);
    read_beyond(utf8_path, *prefix, family, ev, out);
    detail::overlay_sidecar(utf8_path, out);
    return search_facts_of(out);
  }
  const bool raw = family == codec::format_family::raw || codec::looks_like_raw(*prefix);
  detail::read_still(*prefix, raw || family == codec::format_family::jpeg, out, &ev);
  if (out.properties.empty() && prefix->size() >= kSearchPrefixBytes &&
      (raw || family == codec::format_family::tiff)) {
    // TIFF-container RAW keeps IFDs past the head: the pane's 8 MB, once.
    if (auto more = io::read_prefix(utf8_path, kPrefixBytes)) detail::read_still(*more, true, out, &ev);
  }
  read_beyond(utf8_path, *prefix, family, ev, out);
  detail::overlay_sidecar(utf8_path, out);
  return search_facts_of(out);
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

io::cull_marks read_cull_marks(std::string_view utf8_path) {
  io::cull_marks out;
  detail::field_state f;
  if (auto prefix = io::read_prefix(utf8_path, kDatePrefixBytes)) {
    const std::span<const std::uint8_t> head(prefix->data(), std::min<std::size_t>(prefix->size(), 1024));
    if (codec::probe(head) != codec::format_family::unknown) {
      auto fields = detail::still_fields(*prefix);
      if (!fields && prefix->size() >= kDatePrefixBytes) {
        if (auto more = io::read_prefix(utf8_path, kPrefixBytes)) fields = detail::still_fields(*more);
      }
      if (fields) f = std::move(*fields);
    }
  }
  // The sidecar wins, field by field, as overlay_sidecar does for `read`.
  try {
    Exiv2::XmpData xmp;
    if (detail::load_sidecar(sidecar_path_for(utf8_path), xmp)) {
      const Exiv2::ExifData none;
      const detail::field_state side = detail::read_fields(none, xmp);
      if (side.rating) f.rating = side.rating;
      if (side.label) f.label = side.label;
    }
  } catch (...) {
    // A damaged sidecar changes nothing.
  }
  if (f.rating) out.rating = static_cast<std::int8_t>(std::clamp(*f.rating, -1, 5));
  if (f.label) out.label = io::cull_label_of(*f.label);
  return out;
}

}  // namespace mv::meta
