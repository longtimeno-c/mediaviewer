// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A QR code for the terminal: `driftwire host setup` shows a phone's pairing
// code (docs/plans/self-hosted.md §2 step 4) the way the desktop's card does.
//
// The encoder is the desktop's own (src.managed/Shared/QrCode.cs) in C++: the
// algorithm of Project Nayuki's QR Code generator library (MIT License,
// notice in qr.cpp), reduced to byte mode at error correction M, which is
// what the Mac's CIQRCodeGenerator and the Windows card are asked for. No new
// dependency (docs/design/11-licensing.md: MIT, compatible, notice kept).
//
// Pure, no I/O. Portable (D9).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mv::driftwire::cli {

class qr_code {
 public:
  // The code for `bytes` (UTF-8 text is bytes); nullopt when too long for version 40.
  [[nodiscard]] static std::optional<qr_code> encode(std::string_view bytes);

  [[nodiscard]] int size() const noexcept { return size_; }  // modules a side: 21 … 177
  [[nodiscard]] int version() const noexcept { return version_; }
  [[nodiscard]] int mask() const noexcept { return mask_; }
  // Dark? Outside the symbol is light (the quiet zone).
  [[nodiscard]] bool dark(int x, int y) const noexcept;

 private:
  qr_code(int version, const std::vector<std::uint8_t>& data);
  void set_function(int x, int y, bool dark);
  void draw_function_patterns();
  void draw_finder(int x, int y);
  void draw_alignment(int x, int y);
  [[nodiscard]] std::vector<int> alignment_positions() const;
  void draw_format_bits(int mask);
  void draw_version();
  [[nodiscard]] std::vector<std::uint8_t> add_ecc_and_interleave(const std::vector<std::uint8_t>& data) const;
  void draw_codewords(const std::vector<std::uint8_t>& data);
  void apply_mask(int mask);
  [[nodiscard]] long penalty() const;
  [[nodiscard]] int finder_like(int line, bool row) const;
  [[nodiscard]] bool at(int x, int y) const noexcept { return modules_[static_cast<std::size_t>(y * size_ + x)] != 0; }

  int version_ = 1, size_ = 21, mask_ = 0;
  std::vector<std::uint8_t> modules_;   // [y * size + x], 1 = dark
  std::vector<std::uint8_t> function_;  // finder, timing, alignment, format, version
};

// The code as terminal lines, two rows of modules a line (▀ ▄ █ and space),
// with a quiet zone of `quiet` modules. `colour`: each line sets black on
// white itself (ANSI), so the code reads the same on a dark or a light
// terminal; without it, dark modules are drawn as ink on the terminal's own
// background (right on a light one). Every line has the same width:
// size + 2 * quiet columns.
[[nodiscard]] std::vector<std::string> qr_terminal_lines(const qr_code& q, bool colour, int quiet = 2);

}  // namespace mv::driftwire::cli
