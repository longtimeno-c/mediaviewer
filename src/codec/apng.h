// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// APNG (PNG family). libspng decodes still PNG and ignores acTL / fcTL / fdAT,
// so the animation chunks are walked here and each frame is handed back to the
// still decoder as a standalone PNG (IHDR sized to the frame, the colour and
// palette chunks, its data as IDAT). No new dependency.
//
// A file that breaks the rules — a sequence gap, a frame rectangle outside the
// canvas, zero frames, a frame count that does not match — is `corrupt`, never
// an out-of-bounds write. A PNG without acTL is `unsupported_format` here, and
// the still decoder handles it as it always has.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "codec/anim.h"
#include "core/result.h"

namespace mv::codec {

struct apng_frame {
  frame_region region;
  std::uint32_t delay_ms = 0;  // already browser-clamped
  // The frame's zlib stream, split across IDAT / fdAT chunk payloads (fdAT's
  // sequence number stripped). Spans into the caller's file bytes.
  std::vector<std::span<const std::uint8_t>> data;
};

struct apng_info {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t plays = 0;  // 0 = loop forever
  // The IHDR payload (13 bytes) and every chunk a frame needs to decode on its
  // own (PLTE, tRNS, and the colour chunks), whole: length, type, data, CRC.
  std::span<const std::uint8_t> ihdr;
  std::vector<std::span<const std::uint8_t>> shared_chunks;
  std::vector<apng_frame> frames;
};

// Spans in the result point into `png`; keep the bytes alive.
[[nodiscard]] result<apng_info> parse_apng(std::span<const std::uint8_t> png);

// A standalone PNG for one frame, for decode_png. Never larger than the file.
[[nodiscard]] result<std::vector<std::uint8_t>> apng_frame_png(const apng_info& info,
                                                               const apng_frame& frame);

}  // namespace mv::codec
