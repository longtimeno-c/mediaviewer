// SPDX-License-Identifier: GPL-2.0-or-later
// PR 12: the XMP sidecar beside a file (plan/06: `IMG_1234.xmp`) on the read
// side. The writer (write.cpp) puts rating, orientation and comment there for
// everything it will not rewrite in place; the read model shows them, and the
// sidecar wins over what the file itself says.
#include <exiv2/exiv2.hpp>

#include <string>

#include "io/file.h"
#include "meta/internal.h"
#include "meta/write.h"

namespace mv::meta::detail {
namespace {

// A sidecar is a few KB. Anything past this is not one, and a hostile file
// beside the photo must not make the read model slurp it.
constexpr std::size_t kMaxSidecarBytes = 4u * 1024 * 1024;
constexpr std::size_t kMaxSidecarRows = 64;

}  // namespace

bool load_sidecar(std::string_view sidecar_utf8_path, Exiv2::XmpData& xmp) noexcept {
  try {
    ensure_exiv2();
    auto bytes = io::read_prefix(sidecar_utf8_path, kMaxSidecarBytes + 1);
    if (!bytes || bytes->empty() || bytes->size() > kMaxSidecarBytes) return false;
    const std::string packet(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    Exiv2::XmpData parsed;
    if (Exiv2::XmpParser::decode(parsed, packet) != 0) return false;
    xmp = std::move(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

void overlay_sidecar(std::string_view utf8_path, metadata& out) noexcept {
  try {
    Exiv2::XmpData xmp;
    if (!load_sidecar(sidecar_path_for(utf8_path), xmp)) return;
    const Exiv2::ExifData none;
    const field_state f = read_fields(none, xmp);
    if (f.rating) out.s.rating = *f.rating;
    if (f.comment) out.s.comment = *f.comment;

    std::size_t rows = 0;
    for (const auto& d : xmp) {
      if (rows++ >= kMaxSidecarRows) break;
      property p;
      p.space = origin::xmp;
      p.raw_tag = d.key();
      p.name = d.tagName();
      const std::string label = d.tagLabel();
      p.label = label.empty() ? p.name : label;
      p.group = "Xmp." + d.groupName() + " (sidecar)";
      p.raw = sanitise_utf8(d.toString());
      p.value = p.raw;
      out.properties.push_back(std::move(p));
    }
  } catch (...) {
    // A damaged sidecar changes nothing.
  }
}

}  // namespace mv::meta::detail
