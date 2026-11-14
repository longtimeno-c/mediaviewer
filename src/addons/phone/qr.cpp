// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The algorithm follows Project Nayuki's QR Code generator library
// (https://www.nayuki.io/page/qr-code-generator-library), which is
// Copyright (c) Project Nayuki, MIT License: the tables, the Reed-Solomon
// steps and the module placement are its, reduced to what a phone's pairing
// code needs (byte mode, one error-correction level). Its notice:
//
//   Copyright (c) Project Nayuki. (MIT License)
//   https://www.nayuki.io/page/qr-code-generator-library
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy of
//   this software and associated documentation files (the "Software"), to deal in
//   the Software without restriction, including without limitation the rights to
//   use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
//   the Software, and to permit persons to whom the Software is furnished to do so,
//   subject to the following conditions:
//   - The above copyright notice and this permission notice shall be included in
//     all copies or substantial portions of the Software.
//   - The Software is provided "as is", without warranty of any kind, express or
//     implied, including but not limited to the warranties of merchantability,
//     fitness for a particular purpose and noninfringement. In no event shall the
//     authors or copyright holders be liable for any claim, damages or other
//     liability, whether in an action of contract, tort or otherwise, arising from,
//     out of or in connection with the Software or the use or other dealings in the
//     Software.
//
// The portable twin of src.managed/Shared/QrCode.cs; keep the two in step
// (tests/test_phone_qr.cpp pins the same matrices as QrCodeTests.cs).
#include "addons/phone/qr.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <limits>

namespace mv::phone::qr {
namespace {

// Error correction M, by version (index 0 unused).
constexpr int k_ecc_per_block[41] = {
    -1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26,
    26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
};
constexpr int k_ecc_blocks[41] = {
    -1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16,
    17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49,
};
constexpr int k_format_bits_m = 0;  // L = 1, M = 0, Q = 3, H = 2

using bytes_t = std::vector<std::uint8_t>;

int raw_data_modules(int ver) {
  int result = (16 * ver + 128) * ver + 64;
  if (ver >= 2) {
    const int align = ver / 7 + 2;
    result -= (25 * align - 10) * align - 55;
    if (ver >= 7) result -= 36;
  }
  return result;
}

int data_codewords(int ver) { return raw_data_modules(ver) / 8 - k_ecc_per_block[ver] * k_ecc_blocks[ver]; }

// GF(2^8) over x^8 + x^4 + x^3 + x^2 + 1.
std::uint8_t gf_multiply(int x, int y) {
  int z = 0;
  for (int i = 7; i >= 0; --i) {
    z = (z << 1) ^ ((z >> 7) * 0x11D);
    z ^= ((y >> i) & 1) * x;
  }
  return static_cast<std::uint8_t>(z);
}

bytes_t reed_solomon_divisor(int degree) {
  bytes_t result(static_cast<std::size_t>(degree), 0);
  result.back() = 1;
  int root = 1;
  for (int i = 0; i < degree; ++i) {
    for (std::size_t j = 0; j < result.size(); ++j) {
      result[j] = gf_multiply(result[j], root);
      if (j + 1 < result.size()) result[j] = static_cast<std::uint8_t>(result[j] ^ result[j + 1]);
    }
    root = gf_multiply(root, 0x02);
  }
  return result;
}

bytes_t reed_solomon_remainder(const std::uint8_t* data, std::size_t len, const bytes_t& divisor) {
  bytes_t result(divisor.size(), 0);
  for (std::size_t n = 0; n < len; ++n) {
    const int factor = data[n] ^ result[0];
    std::copy(result.begin() + 1, result.end(), result.begin());
    result.back() = 0;
    for (std::size_t i = 0; i < divisor.size(); ++i)
      result[i] = static_cast<std::uint8_t>(result[i] ^ gf_multiply(divisor[i], factor));
  }
  return result;
}

bytes_t data_with_padding(std::string_view data, int count_bits, int capacity_bits) {
  std::vector<bool> bits;
  bits.reserve(static_cast<std::size_t>(capacity_bits));
  auto append = [&bits](int value, int count) {
    for (int i = count - 1; i >= 0; --i) bits.push_back(((value >> i) & 1) != 0);
  };
  const auto bit_count = [&bits] { return static_cast<int>(bits.size()); };
  append(0b0100, 4);  // byte mode
  append(static_cast<int>(data.size()), count_bits);
  for (const char ch : data) append(static_cast<unsigned char>(ch), 8);
  append(0, std::min(4, capacity_bits - bit_count()));  // terminator
  append(0, (8 - bit_count() % 8) % 8);
  for (int pad = 0xEC; bit_count() < capacity_bits; pad ^= 0xEC ^ 0x11) append(pad, 8);
  bytes_t out(bits.size() / 8, 0);
  for (std::size_t i = 0; i < bits.size(); ++i)
    if (bits[i]) out[i >> 3] = static_cast<std::uint8_t>(out[i >> 3] | (0x80u >> (i & 7)));
  return out;
}

class builder {
 public:
  explicit builder(int ver)
      : version_(ver),
        size_(ver * 4 + 17),
        modules_(static_cast<std::size_t>(size_) * static_cast<std::size_t>(size_), 0),
        function_(modules_.size(), 0) {}

  code build(const bytes_t& data_codewords) {
    draw_function_patterns();
    draw_codewords(add_ecc_and_interleave(data_codewords));

    // The mask with the lowest penalty, ties to the lowest number.
    int best = 0;
    long long best_penalty = std::numeric_limits<long long>::max();
    for (int m = 0; m < 8; ++m) {
      apply_mask(m);
      draw_format_bits(m);
      const long long p = penalty();
      if (p < best_penalty) {
        best = m;
        best_penalty = p;
      }
      apply_mask(m);  // XOR again: undone
    }
    apply_mask(best);
    draw_format_bits(best);

    code c;
    c.size = size_;
    c.version = version_;
    c.mask = best;
    c.modules = std::move(modules_);
    return c;
  }

 private:
  std::size_t at(int x, int y) const {
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(size_) + static_cast<std::size_t>(x);
  }
  bool module(int x, int y) const { return modules_[at(x, y)] != 0; }

  void set_function(int x, int y, bool is_dark) {
    modules_[at(x, y)] = is_dark ? 1 : 0;
    function_[at(x, y)] = 1;
  }

  void draw_function_patterns() {
    for (int i = 0; i < size_; ++i) {
      set_function(6, i, i % 2 == 0);
      set_function(i, 6, i % 2 == 0);
    }
    draw_finder(3, 3);
    draw_finder(size_ - 4, 3);
    draw_finder(3, size_ - 4);
    const std::vector<int> align = alignment_positions();
    const int n = static_cast<int>(align.size());
    for (int i = 0; i < n; ++i)
      for (int j = 0; j < n; ++j)
        if (!((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)))
          draw_alignment(align[static_cast<std::size_t>(i)], align[static_cast<std::size_t>(j)]);
    draw_format_bits(0);  // reserves the area; drawn for real once the mask is chosen
    draw_version();
  }

  void draw_finder(int x, int y) {
    for (int dy = -4; dy <= 4; ++dy)
      for (int dx = -4; dx <= 4; ++dx) {
        const int dist = std::max(std::abs(dx), std::abs(dy));
        const int xx = x + dx, yy = y + dy;
        if (xx >= 0 && xx < size_ && yy >= 0 && yy < size_) set_function(xx, yy, dist != 2 && dist != 4);
      }
  }

  void draw_alignment(int x, int y) {
    for (int dy = -2; dy <= 2; ++dy)
      for (int dx = -2; dx <= 2; ++dx) set_function(x + dx, y + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
  }

  std::vector<int> alignment_positions() const {
    if (version_ == 1) return {};
    const int n = version_ / 7 + 2;
    const int step = (version_ * 8 + n * 3 + 5) / (n * 4 - 4) * 2;
    std::vector<int> result(static_cast<std::size_t>(n), 0);
    result[0] = 6;
    for (int i = n - 1, pos = size_ - 7; i >= 1; --i, pos -= step) result[static_cast<std::size_t>(i)] = pos;
    return result;
  }

  void draw_format_bits(int mask) {
    const int data = k_format_bits_m << 3 | mask;
    int rem = data;
    for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    const int bits = (data << 10 | rem) ^ 0x5412;
    const auto bit = [bits](int i) { return ((bits >> i) & 1) != 0; };

    for (int i = 0; i <= 5; ++i) set_function(8, i, bit(i));
    set_function(8, 7, bit(6));
    set_function(8, 8, bit(7));
    set_function(7, 8, bit(8));
    for (int i = 9; i < 15; ++i) set_function(14 - i, 8, bit(i));

    for (int i = 0; i < 8; ++i) set_function(size_ - 1 - i, 8, bit(i));
    for (int i = 8; i < 15; ++i) set_function(8, size_ - 15 + i, bit(i));
    set_function(8, size_ - 8, true);  // the dark module
  }

  void draw_version() {
    if (version_ < 7) return;
    int rem = version_;
    for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    const int bits = version_ << 12 | rem;
    for (int i = 0; i < 18; ++i) {
      const bool b = ((bits >> i) & 1) != 0;
      const int a = size_ - 11 + i % 3, c = i / 3;
      set_function(a, c, b);
      set_function(c, a, b);
    }
  }

  bytes_t add_ecc_and_interleave(const bytes_t& data) const {
    const int blocks = k_ecc_blocks[version_];
    const int ecc_len = k_ecc_per_block[version_];
    const int raw = raw_data_modules(version_) / 8;
    const int short_blocks = blocks - raw % blocks;
    const int short_len = raw / blocks;
    const bytes_t divisor = reed_solomon_divisor(ecc_len);

    std::vector<bytes_t> all(static_cast<std::size_t>(blocks));
    std::size_t k = 0;
    for (int i = 0; i < blocks; ++i) {
      const int dat_len = short_len - ecc_len + (i < short_blocks ? 0 : 1);
      const auto dat = static_cast<std::size_t>(dat_len);
      bytes_t block(static_cast<std::size_t>(short_len + 1), 0);
      std::copy(data.begin() + static_cast<std::ptrdiff_t>(k), data.begin() + static_cast<std::ptrdiff_t>(k + dat),
                block.begin());
      const bytes_t ecc = reed_solomon_remainder(data.data() + k, dat, divisor);
      k += dat;
      // A short block keeps one empty slot before its ECC, skipped below.
      std::copy(ecc.begin(), ecc.end(), block.begin() + (short_len + 1 - ecc_len));
      all[static_cast<std::size_t>(i)] = std::move(block);
    }

    bytes_t result(static_cast<std::size_t>(raw), 0);
    std::size_t n = 0;
    const int block_len = short_len + 1;
    for (int i = 0; i < block_len; ++i)
      for (int j = 0; j < blocks; ++j)
        if (i != short_len - ecc_len || j >= short_blocks)
          result[n++] = all[static_cast<std::size_t>(j)][static_cast<std::size_t>(i)];
    return result;
  }

  void draw_codewords(const bytes_t& data) {
    std::size_t i = 0;
    const std::size_t total_bits = data.size() * 8;
    for (int right = size_ - 1; right >= 1; right -= 2) {
      if (right == 6) right = 5;  // skip the vertical timing column
      const bool upward = ((right + 1) & 2) == 0;
      for (int vert = 0; vert < size_; ++vert)
        for (int j = 0; j < 2; ++j) {
          const int x = right - j;
          const int y = upward ? size_ - 1 - vert : vert;
          if (function_[at(x, y)] != 0 || i >= total_bits) continue;
          modules_[at(x, y)] = static_cast<std::uint8_t>((data[i >> 3] >> (7 - (i & 7))) & 1);
          ++i;
        }
    }
  }

  void apply_mask(int mask) {
    for (int y = 0; y < size_; ++y)
      for (int x = 0; x < size_; ++x) {
        bool invert = false;
        switch (mask) {
          case 0: invert = (x + y) % 2 == 0; break;
          case 1: invert = y % 2 == 0; break;
          case 2: invert = x % 3 == 0; break;
          case 3: invert = (x + y) % 3 == 0; break;
          case 4: invert = (x / 3 + y / 2) % 2 == 0; break;
          case 5: invert = x * y % 2 + x * y % 3 == 0; break;
          case 6: invert = (x * y % 2 + x * y % 3) % 2 == 0; break;
          default: invert = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
        }
        if (invert && function_[at(x, y)] == 0) modules_[at(x, y)] ^= 1;
      }
  }

  // ---- choosing the mask (ISO/IEC 18004 §7.8.3) ----

  long long penalty() const {
    long long result = 0;
    for (int pass = 0; pass < 2; ++pass) {  // rows, then columns
      for (int a = 0; a < size_; ++a) {
        bool run_colour = false;
        int run = 0;
        for (int b = 0; b < size_; ++b) {
          const bool c = pass == 0 ? module(b, a) : module(a, b);
          if (b > 0 && c == run_colour) {
            ++run;
          } else {
            if (run >= 5) result += 3 + (run - 5);
            run_colour = c;
            run = 1;
          }
        }
        if (run >= 5) result += 3 + (run - 5);
        result += 40LL * finder_like(a, pass == 0);
      }
    }
    for (int y = 0; y < size_ - 1; ++y)
      for (int x = 0; x < size_ - 1; ++x) {
        const bool c = module(x, y);
        if (c == module(x + 1, y) && c == module(x, y + 1) && c == module(x + 1, y + 1)) result += 3;
      }
    int dark = 0;
    for (const std::uint8_t m : modules_)
      if (m != 0) ++dark;
    const int total = size_ * size_;
    const int k = (std::abs(dark * 20 - total * 10) + total - 1) / total - 1;
    result += k * 10LL;
    return result;
  }

  // 1:1:3:1:1 dark-light-dark-light-dark with four light modules on either
  // side (outside the symbol counts as light).
  int finder_like(int line, bool row) const {
    const auto on = [this, line, row](int i) {
      return i >= 0 && i < size_ && (row ? module(i, line) : module(line, i));
    };
    int count = 0;
    for (int i = -4; i < size_; ++i) {
      const bool core = on(i) && !on(i + 1) && on(i + 2) && on(i + 3) && on(i + 4) && !on(i + 5) && on(i + 6);
      if (!core) continue;
      const bool before = !on(i - 1) && !on(i - 2) && !on(i - 3) && !on(i - 4);
      const bool after = !on(i + 7) && !on(i + 8) && !on(i + 9) && !on(i + 10);
      if (before || after) ++count;
    }
    return count;
  }

  int version_;
  int size_;
  bytes_t modules_;   // [y * size + x], 1 = dark
  bytes_t function_;  // finder, timing, alignment, format, version
};

}  // namespace

std::optional<code> encode(std::string_view bytes) {
  for (int version = 1; version <= 40; ++version) {
    const int capacity_bits = data_codewords(version) * 8;
    const int count_bits = version <= 9 ? 8 : 16;
    const unsigned long long used = 4ULL + static_cast<unsigned long long>(count_bits) + bytes.size() * 8ULL;
    if (bytes.size() < (std::size_t{1} << count_bits) && used <= static_cast<unsigned long long>(capacity_bits))
      return builder(version).build(data_with_padding(bytes, count_bits, capacity_bits));
  }
  return std::nullopt;
}

void draw_bgra(const code& c, int side_px, std::vector<std::uint8_t>& out_bgra, int& out_side) {
  out_bgra.clear();
  out_side = 0;
  if (c.size <= 0) return;
  const int span = c.size + 8;  // four quiet modules each side
  const int module_px = std::max(1, side_px / span);
  out_side = module_px * span;
  const auto side = static_cast<std::size_t>(out_side);
  out_bgra.assign(side * side * 4, 255);  // white, opaque
  const auto mpx = static_cast<std::size_t>(module_px);
  for (int y = 0; y < c.size; ++y)
    for (int x = 0; x < c.size; ++x) {
      if (!c.dark(x, y)) continue;
      const std::size_t px0 = static_cast<std::size_t>(x + 4) * mpx;
      const std::size_t py0 = static_cast<std::size_t>(y + 4) * mpx;
      for (std::size_t py = py0; py < py0 + mpx; ++py) {
        std::uint8_t* row = out_bgra.data() + (py * side + px0) * 4;
        for (std::size_t px = 0; px < mpx; ++px) {
          row[px * 4 + 0] = 0;
          row[px * 4 + 1] = 0;
          row[px * 4 + 2] = 0;  // alpha stays 255
        }
      }
    }
}

}  // namespace mv::phone::qr
