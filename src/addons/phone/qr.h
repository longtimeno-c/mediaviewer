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
// A QR code for the Phone access card's pairing code (docs/plans/mobile.md
// §7.4), the portable twin of src.managed/Shared/QrCode.cs: byte mode, error
// correction M, the smallest version that holds the bytes, the mask with the
// lowest penalty (ties to the lowest number). Module for module the same
// matrix as the C#. No I/O, no OS headers, nothing thrown.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace mv::phone::qr {

struct code {
  int size = 0;     // modules per side: 21 (version 1) ... 177 (version 40)
  int version = 0;  // 1..40
  int mask = 0;     // 0..7, the mask chosen
  std::vector<std::uint8_t> modules;  // size*size, row-major, 1 = dark

  // Outside the symbol reads as light (the quiet zone).
  bool dark(int x, int y) const {
    return x >= 0 && y >= 0 && x < size && y < size &&
           modules[static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)] != 0;
  }
};

// The code for `bytes` (UTF-8 text is passed as its bytes); nullopt when it
// is too long for version 40-M (more than 2,331 bytes).
std::optional<code> encode(std::string_view bytes);

// The code drawn as opaque BGRA: white, a quiet zone of four modules on each
// side, each module floor(side_px / (size + 8)) pixels (at least 1), so
// out_side = module_px * (size + 8), which is <= side_px unless side_px is
// smaller than size + 8. Stride is out_side * 4. Opaque, so premultiplied
// and straight alpha are the same bytes. An empty code (size 0) gives an
// empty image and out_side 0.
void draw_bgra(const code& c, int side_px, std::vector<std::uint8_t>& out_bgra, int& out_side);

}  // namespace mv::phone::qr
