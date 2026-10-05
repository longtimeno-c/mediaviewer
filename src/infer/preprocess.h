// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Pixels -> model tensors (docs/design/17 "Frame -> tensor"). CPU, portable, no
// allocation beyond the output. The CLIP path matches the reference
// (Hugging Face CLIPImageProcessor / PIL): shortest side to `size` with a
// bicubic (a = -0.5) antialiasing filter, centre crop, x/255, per-channel
// mean/std. The PR 20 verify compares the resulting embeddings with the
// Python reference (tests/data/ai/reference.json).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace mv::infer {

struct rgb_view {
  const std::uint8_t* rgb = nullptr;  // stride = width * 3
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

struct clip_norm {
  std::uint32_t size = 224;
  std::array<float, 3> mean{0.48145466f, 0.4578275f, 0.40821073f};
  std::array<float, 3> std{0.26862954f, 0.26130258f, 0.27577711f};
};

// Appends one CHW image (3 * size * size floats) to `out`.
void clip_tensor(const rgb_view& img, const clip_norm& norm, std::vector<float>& out);

// Separable resample of RGB8 with PIL's bicubic antialias filter. Exposed
// for the face path and tests.
[[nodiscard]] std::vector<std::uint8_t> resize_bicubic(const rgb_view& img, std::uint32_t w,
                                                       std::uint32_t h);

// ---- faces (PR 24) -----------------------------------------------------------

// YuNet's input: the image fitted inside side x side (aspect kept, top-left,
// zero padding), BGR, 0..255 floats, CHW. `scale` maps detector pixels back
// to the image (image = detector * scale).
void yunet_tensor(const rgb_view& img, std::uint32_t side, std::vector<float>& out, float& scale);

// SFace's input: the face warped by the similarity transform that takes its
// five landmarks (eyes, nose, mouth corners; image pixels) onto the ArcFace
// 112x112 template, RGB, 0..255 floats, CHW.
void sface_tensor(const rgb_view& img, const std::array<float, 10>& landmarks,
                  std::vector<float>& out);

}  // namespace mv::infer
