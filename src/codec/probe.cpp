// SPDX-License-Identifier: GPL-2.0-or-later
#include "codec/format.h"

#include <cstring>

namespace mv::codec {

format_family probe(std::span<const std::uint8_t> header) noexcept {
  if (header.size() >= 3 && header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF) {
    return format_family::jpeg;
  }
  static constexpr std::uint8_t kPng[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  if (header.size() >= 8 && std::memcmp(header.data(), kPng, 8) == 0) {
    return format_family::png;
  }
  if (header.size() >= 2 && header[0] == 'B' && header[1] == 'M') {
    return format_family::bmp;
  }
  return format_family::unknown;
}

}  // namespace mv::codec
