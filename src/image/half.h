// SPDX-License-Identifier: GPL-2.0-or-later
// IEEE 754 binary16 <-> binary32, portable (no F16C / NEON intrinsics), so the
// FP16 working space (D6, PR 11) is the same bits on both hosts: a
// DXGI_FORMAT_R16G16B16A16_FLOAT texture and an MTLPixelFormatRGBA16Float one
// take these words unchanged. Round-to-nearest-even; NaN stays NaN, overflow
// becomes infinity, tiny values become subnormals or signed zero.
#pragma once

#include <cstdint>
#include <cstring>

namespace mv::image {

[[nodiscard]] inline std::uint16_t float_to_half(float f) noexcept {
  std::uint32_t x = 0;
  std::memcpy(&x, &f, sizeof x);
  const std::uint32_t sign = (x >> 16) & 0x8000u;
  std::uint32_t mag = x & 0x7fffffffu;
  if (mag >= 0x7f800000u) {  // inf or NaN
    return static_cast<std::uint16_t>(sign | 0x7c00u | (mag > 0x7f800000u ? 0x200u : 0u));
  }
  if (mag >= 0x477ff000u) return static_cast<std::uint16_t>(sign | 0x7c00u);  // rounds past 65504
  if (mag < 0x38800000u) {  // below the smallest normal half: subnormal or zero
    if (mag < 0x33000000u) return static_cast<std::uint16_t>(sign);  // under half the smallest subnormal
    const std::uint32_t e = mag >> 23;
    const std::uint32_t m = (mag & 0x7fffffu) | 0x800000u;
    const std::uint32_t shift = 126u - e;  // 14..24
    std::uint32_t h = m >> shift;
    const std::uint32_t rem = m & ((1u << shift) - 1u);
    const std::uint32_t halfway = 1u << (shift - 1u);
    if (rem > halfway || (rem == halfway && (h & 1u))) ++h;
    return static_cast<std::uint16_t>(sign | h);
  }
  // Normal: rebias the exponent (127 -> 15), round the 13 dropped bits.
  mag -= 0x38000000u;
  std::uint32_t h = mag >> 13;
  const std::uint32_t rem = mag & 0x1fffu;
  if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) ++h;
  return static_cast<std::uint16_t>(sign | h);
}

[[nodiscard]] inline float half_to_float(std::uint16_t h) noexcept {
  const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
  const std::uint32_t e = (h >> 10) & 0x1fu;
  std::uint32_t m = h & 0x3ffu;
  std::uint32_t x = 0;
  if (e == 0) {
    if (m == 0) {
      x = sign;
    } else {  // subnormal: normalise
      std::uint32_t exp = 113;  // 127 - 15 + 1
      while ((m & 0x400u) == 0) {
        m <<= 1;
        --exp;
      }
      x = sign | (exp << 23) | ((m & 0x3ffu) << 13);
    }
  } else if (e == 31) {
    x = sign | 0x7f800000u | (m << 13);
  } else {
    x = sign | ((e + 112u) << 23) | (m << 13);
  }
  float f = 0.0f;
  std::memcpy(&f, &x, sizeof f);
  return f;
}

}  // namespace mv::image
