// SPDX-License-Identifier: GPL-2.0-or-later
// Format family identified by magic bytes, never by extension
// (plan/04-image-pipeline.md).
#pragma once

#include <cstdint>
#include <span>

namespace mv::codec {

enum class format_family : std::uint32_t {
  unknown = 0,
  jpeg = 1,
  png = 2,
  bmp = 3,
};

// First bytes only. A short or empty span is unknown, not corrupt.
[[nodiscard]] format_family probe(std::span<const std::uint8_t> header) noexcept;

[[nodiscard]] constexpr const char* format_name(format_family f) noexcept {
  switch (f) {
    case format_family::jpeg: return "JPEG";
    case format_family::png:  return "PNG";
    case format_family::bmp:  return "BMP";
    case format_family::unknown: return "unknown";
  }
  return "unknown";
}

}  // namespace mv::codec
