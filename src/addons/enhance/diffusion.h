// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance's Restyle (docs/plans/edit-adjust.md slice 5b, owner 2026-11-03):
// a typed request ("make it a snowy winter day") applied to a photo by
// InstructPix2Pix (Brooks et al.; weights CreativeML OpenRAIL-M, accepted by
// the owner), run on ONNX Runtime from the add-on's own folder.
//
//   1. The photo at about kRestyleEdge on its long edge (multiples of 64).
//   2. The prompt and an empty prompt through CLIP's text encoder (77 tokens).
//   3. The picture's latent (the VAE encoder's mean).
//   4. 20 Euler-ancestral steps from seeded noise: each step one UNet run of
//      three (prompt + picture, no prompt + picture, no prompt + no picture),
//      guided as the reference pipeline guides (text 7.5, picture 1.5).
//   5. The VAE decoder; then the change carried back onto the full-size
//      photo as low-frequency colour and tone (carry_back), so the photo
//      keeps its own detail.
//
// What it is good at: whole-picture restyles (season, weather, time of day,
// mood). What it is not: removing or adding objects (measured, the plan's
// spike). The runner's thread only (rule 1).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/result.h"

namespace mv::infer {
class runtime;
}

namespace mv::enhance {

inline constexpr std::uint32_t kRestyleEdge = 512;

struct rgb8 {
  std::uint32_t width = 0, height = 0;
  std::vector<std::uint8_t> rgb;  // width * height * 3
};

// The graphs' tensor and free-dimension names, from the spec's io.tensors.
// Defaults are tools/package/restyle-export.py's; a diffusers-style export
// (ForserX/instruct-pix2pix-onnx, pinned) names them as the comments say.
struct restyle_io {
  std::string ids = "input_ids";       // the text encoder's in (int64 or int32)
  std::string sample = "sample";       // the UNet's ins
  std::string timestep = "timestep";
  std::string hidden = "hidden";       // "encoder_hidden_states"
  std::string encode_in = "image";     // the VAE encoder's in ("sample")
  std::string decode_in = "latent";    // the VAE decoder's in ("latent_sample")
  std::string dim_batch = "b", dim_h = "h", dim_w = "w";  // "batch", "height", "width"
  std::vector<std::pair<std::string, std::int64_t>> fixed;  // more dims to pin ("channels" 8, "sequence" 77)
  // Core ML for the UNet where the runtime has it. The pinned export's UNet
  // compiles there but fails to run (2026-11-04), so its spec turns it off:
  // the CPU, about 5 s a step at Standard on an M-series Mac.
  bool coreml = true;
  int threads = 0;  // the CPU sessions' threads (the performance level); 0 cores - 1, at most 8
};

struct restyle_params {
  std::string prompt;
  int steps = 20;
  float text_guidance = 7.5f;   // how strongly the words steer
  float image_guidance = 1.5f;  // how closely the picture is kept
  std::uint32_t edge = kRestyleEdge;
  std::uint64_t seed = 7;
};

// The reference scheduler (diffusers EulerAncestralDiscrete, InstructPix2Pix's
// config: scaled-linear betas 0.00085 .. 0.012 over 1000, linspace spacing).
struct euler_schedule {
  std::vector<float> timesteps;  // `steps` of them, descending
  std::vector<float> sigmas;     // steps + 1, the last 0
  float init = 0.0f;             // the starting noise's scale
};
[[nodiscard]] euler_schedule make_euler_schedule(int steps);

// Bilinear, for the edit's small copy and its way back.
[[nodiscard]] rgb8 resize_rgb(const rgb8& src, std::uint32_t width, std::uint32_t height);
// The size the edit runs at: about `edge` on the long side, both sides
// multiples of 64 (the UNet's three halvings of an eighth-size latent).
void restyle_size(std::uint32_t w, std::uint32_t h, std::uint32_t edge, std::uint32_t& out_w, std::uint32_t& out_h);

// `full` changed as `before` -> `after` changed (both the edit's size): each
// channel multiplied by the blurred ratio of after to before, enlarged;
// `strength` 0..1 mixes the ratio toward 1.
[[nodiscard]] rgb8 carry_back(const rgb8& full, const rgb8& before, const rgb8& after, float strength);

class restyle_model {
 public:
  ~restyle_model();
  // `dir` holds text_encoder.onnx, unet.onnx (+ .data), vae_encoder.onnx,
  // vae_decoder.onnx, vocab.json, merges.txt. The UNet runs on Core ML where
  // the runtime has it (compiled into `cache_dir` per picture size), else CPU.
  [[nodiscard]] static result<std::unique_ptr<restyle_model>> open(infer::runtime& rt, const std::string& dir,
                                                                   const std::string& cache_dir,
                                                                   restyle_io io = {});
  // `small` is at the edit's size (restyle_size: multiples of 64, else
  // invalid_arg). `progress(step, steps)`
  // returning false cancels (status::cancelled).
  [[nodiscard]] result<rgb8> run(const rgb8& small, const restyle_params& p,
                                 const std::function<bool(int, int)>& progress);
  // Opens the UNet for a picture of this size (Core ML compiles it the first
  // time, minutes, then it is cached); after it, on_gpu says where it runs.
  [[nodiscard]] expected prepare(std::uint32_t width, std::uint32_t height);
  [[nodiscard]] bool on_gpu() const noexcept;

  struct impl;

 private:
  explicit restyle_model(std::unique_ptr<impl> p);
  std::unique_ptr<impl> p_;
};

}  // namespace mv::enhance
