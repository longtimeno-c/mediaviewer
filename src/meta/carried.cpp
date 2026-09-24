// SPDX-License-Identifier: GPL-2.0-or-later
// PR 10: the metadata a re-encoded export carries (meta.h read_carried).
#include <exiv2/exiv2.hpp>

#include <algorithm>
#include <cstring>
#include <string>

#include "meta/internal.h"
#include "meta/meta.h"

namespace mv::meta {
namespace {

// One APP1 segment holds 65533 bytes, "Exif\0\0" included.
constexpr std::size_t kMaxExifBlock = 65533 - 6;

// IFD0 tags that describe the source file's own pixels or container, not the
// photograph. Carried into a JPEG they would describe pixels that are not
// there (strip offsets into nothing) or claim a format the file is not.
bool structural(const Exiv2::Exifdatum& d) {
  static const char* const kKeys[] = {
      "Exif.Image.NewSubfileType",     "Exif.Image.SubfileType",
      "Exif.Image.ImageWidth",         "Exif.Image.ImageLength",
      "Exif.Image.BitsPerSample",      "Exif.Image.Compression",
      "Exif.Image.PhotometricInterpretation", "Exif.Image.FillOrder",
      "Exif.Image.StripOffsets",       "Exif.Image.SamplesPerPixel",
      "Exif.Image.RowsPerStrip",       "Exif.Image.StripByteCounts",
      "Exif.Image.PlanarConfiguration", "Exif.Image.Predictor",
      "Exif.Image.TileWidth",          "Exif.Image.TileLength",
      "Exif.Image.TileOffsets",        "Exif.Image.TileByteCounts",
      "Exif.Image.SubIFDs",            "Exif.Image.ExtraSamples",
      "Exif.Image.SampleFormat",       "Exif.Image.JPEGTables",
      "Exif.Image.JPEGProc",           "Exif.Image.JPEGInterchangeFormat",
      "Exif.Image.JPEGInterchangeFormatLength",
      "Exif.Image.YCbCrCoefficients",  "Exif.Image.YCbCrSubSampling",
      "Exif.Image.YCbCrPositioning",   "Exif.Image.ReferenceBlackWhite",
      "Exif.Image.XMLPacket",          "Exif.Image.IPTCNAA",
      "Exif.Image.InterColorProfile",  "Exif.Image.ImageResources",
      "Exif.Image.CFARepeatPatternDim", "Exif.Image.CFAPattern",
  };
  const std::string key = d.key();
  for (const char* k : kKeys) {
    if (key == k) return true;
  }
  // DNG's own tags (0xC612 DNGVersion .. the end of the DNG range) describe the
  // raw data and how to develop it: meaningless on a rendered JPEG.
  return d.groupName() == "Image" && d.tag() >= 0xC612;
}

// Only the groups a JPEG's EXIF carries: IFD0 (less the above), the Exif and
// GPS IFDs, interoperability, and maker notes. Sub-images, second images and
// the thumbnail IFD are the source file's, not the photograph's.
bool carried_group(const std::string& group) {
  return group == "Image" || group == "Photo" || group == "GPSInfo" || group == "Iop" ||
         Exiv2::ExifTags::isMakerGroup(group);
}

std::vector<std::uint8_t> encode(Exiv2::ExifData exif) {  // encode() takes it non-const
  if (exif.empty()) return {};
  Exiv2::Blob blob;
  Exiv2::ExifParser::encode(blob, nullptr, 0, Exiv2::littleEndian, exif);
  return {blob.begin(), blob.end()};
}

}  // namespace

carried_metadata read_carried(std::span<const std::uint8_t> bytes) noexcept {
  carried_metadata out;
  try {
    auto image = detail::open_image(bytes);
    if (!image) return out;

    Exiv2::ExifData exif;
    for (const auto& d : image->exifData()) {
      if (!carried_group(d.groupName()) || structural(d)) continue;
      exif.add(d);
    }
    if (!exif.empty()) {
      // The export is upright whatever the source said (edit/export.h also
      // patches this, but a source with no Orientation gets none added there).
      exif["Exif.Image.Orientation"] = static_cast<std::uint16_t>(1);
      try {
        out.exif = encode(exif);
      } catch (...) {
        out.exif.clear();  // a maker note Exiv2 cannot re-encode: retried below
      }
      if (out.exif.empty() || out.exif.size() > kMaxExifBlock) {
        // Keep the photograph's metadata and lose the maker notes, rather than
        // lose everything to a block one APP1 cannot hold.
        Exiv2::ExifData plain;
        for (const auto& d : exif) {
          if (!Exiv2::ExifTags::isMakerGroup(d.groupName())) plain.add(d);
        }
        out.exif = encode(plain);
        if (out.exif.size() > kMaxExifBlock) out.exif.clear();
      }
    }

    std::string packet = image->xmpPacket();
    if (packet.empty() && !image->xmpData().empty()) {
      Exiv2::XmpParser::encode(packet, image->xmpData());
    }
    out.xmp.assign(packet.begin(), packet.end());
  } catch (...) {
    // A damaged tag table: carry nothing rather than something half-read.
    out = carried_metadata{};
  }
  return out;
}

}  // namespace mv::meta
