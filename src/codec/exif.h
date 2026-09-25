// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Byte-level EXIF (TIFF) access for the few things the geometry slice has to
// read or change without a metadata library: the Orientation tag, the pixel
// dimensions, and the GPS IFD (PR 10, plan/07 "Export", plan/04).
//
// Every change here is made *in place* on the TIFF block: no entry moves, no
// offset is rewritten, so maker notes that use absolute offsets survive
// byte-for-byte (the PR 12 bar, met early). Anything that would need the
// block to grow is refused instead. All reads are bounds-checked; a damaged
// block is "not present", never a crash.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace mv::codec {

// Where the EXIF TIFF block sits inside a JPEG: [offset, offset + size) of
// the bytes after "Exif\0\0" in the first APP1 that carries it.
struct byte_range {
  std::size_t offset = 0;
  std::size_t size = 0;
};

[[nodiscard]] std::optional<byte_range> find_jpeg_exif(std::span<const std::uint8_t> jpeg) noexcept;

// The XMP packet's APP1 payload (after the namespace string), if any.
[[nodiscard]] std::optional<byte_range> find_jpeg_xmp(std::span<const std::uint8_t> jpeg) noexcept;

// Orientation 1..8 from IFD0, or 0 when absent or out of range.
[[nodiscard]] int exif_orientation(std::span<const std::uint8_t> tiff) noexcept;
[[nodiscard]] int jpeg_orientation(std::span<const std::uint8_t> jpeg) noexcept;

// Overwrites the Orientation value. False when IFD0 has no Orientation entry
// (the caller then decides whether a new EXIF block is acceptable).
[[nodiscard]] bool exif_set_orientation(std::span<std::uint8_t> tiff, int orientation) noexcept;

// Overwrites Exif.Photo.PixelXDimension / PixelYDimension where present (SHORT
// or LONG). A value that does not fit a SHORT entry is refused. Absent tags
// are fine: true.
[[nodiscard]] bool exif_set_pixel_dimensions(std::span<std::uint8_t> tiff, std::uint32_t width,
                                             std::uint32_t height) noexcept;

// Removes the GPS IFD pointer from IFD0 and zeroes the GPS IFD with every
// value it points at, so no coordinate survives in the bytes. True when there
// was nothing to remove as well. False only for a block too damaged to walk.
[[nodiscard]] bool exif_strip_gps(std::span<std::uint8_t> tiff) noexcept;

// True when IFD0 points at a GPS IFD.
[[nodiscard]] bool exif_has_gps(std::span<const std::uint8_t> tiff) noexcept;

// Unlinks IFD1 (the embedded EXIF thumbnail) by zeroing IFD0's next-IFD
// offset. Used when the pixels are rotated: a thumbnail left in the old
// orientation under Orientation = 1 would show sideways in Explorer/Finder.
// The thumbnail bytes stay in the block, unreferenced; nothing moves.
[[nodiscard]] bool exif_drop_thumbnail(std::span<std::uint8_t> tiff) noexcept;

// Rewrites every tiff:Orientation value in an XMP packet (attribute or
// element form) to `orientation`, in place. Same length, so the APP1 stays
// the size it was. Returns how many values were rewritten.
int xmp_set_orientation(std::span<std::uint8_t> xmp, int orientation) noexcept;

// True when the packet names any exif:GPS* property.
[[nodiscard]] bool xmp_has_gps(std::span<const std::uint8_t> xmp) noexcept;

// A little-endian TIFF block whose IFD0 holds only Orientation. For a JPEG
// that has no EXIF at all and needs an orientation written losslessly.
[[nodiscard]] std::vector<std::uint8_t> exif_minimal(int orientation);

}  // namespace mv::codec
