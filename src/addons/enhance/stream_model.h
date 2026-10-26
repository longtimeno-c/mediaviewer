// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance (#297 slice 4): a streaming sound model on ONNX Runtime, as
// sound_job.h's frame_model. The shape is its model.json's `io` section:
//
//   {"runner": "stream", "rate": 48000, "frame": 480, "state": 45304, "delay": 1440}
//
// Three inputs by position: one frame of one channel, the state it carries,
// and a scalar (DeepFilterNet3's attenuation limit; 0 is none). The first
// two outputs are the frame out and the new state. ONNX Runtime is loaded at
// run time from the add-on's own folder (src/infer, never linked); CPU, one
// thread: a 10 ms frame is too small for a GPU round trip, and one thread runs
// it at about a seventh of real time (docs/plans/enhance.md).
#pragma once

#include <memory>
#include <string>

#include "addons/enhance/picture_job.h"
#include "addons/enhance/sound_job.h"
#include "addons/enhance/spec.h"
#include "core/result.h"

namespace mv::infer {
class runtime;
}

namespace mv::enhance {

struct stream_shape {
  std::uint32_t rate = 0;
  std::size_t frame = 0;
  std::size_t state = 0;
  std::size_t delay = 0;
};

// The `io` section of a streaming model, checked; nullopt when it is not one.
[[nodiscard]] std::optional<stream_shape> stream_shape_of(const model_spec& spec);

// A tiled picture model's `io` (slice 5):
//   {"runner": "tiles", "scale": 4, "tile": 256, "overlap": 16, "dims": ["h", "w"]}
// `dims` names the graph's free height and width, pinned to `tile` so Core ML
// compiles it.
struct tile_shape {
  std::uint32_t scale = 0;
  std::uint32_t tile = 0;
  std::uint32_t overlap = 0;
  std::string dim_h, dim_w;
};
[[nodiscard]] std::optional<tile_shape> tile_shape_of(const model_spec& spec);

// ONNX Runtime from `dir`, loaded on first use and kept until this object
// goes (the add-on's shutdown, after its job has stopped): one per add-on,
// since ORT's environment is process-wide.
class runtime_holder {
 public:
  explicit runtime_holder(std::string dir);
  ~runtime_holder();
  runtime_holder(const runtime_holder&) = delete;
  runtime_holder& operator=(const runtime_holder&) = delete;
  // Opens the model's first weights file under `models_dir`. Any thread.
  [[nodiscard]] result<std::unique_ptr<frame_model>> open(const model_spec& spec, const std::string& models_dir);
  // A tiled picture model: on Core ML where this runtime has it (compiled
  // once into `cache_dir`), else the CPU with all but one core.
  [[nodiscard]] result<std::unique_ptr<tile_model>> open_tiles(const model_spec& spec, const std::string& models_dir,
                                                               const std::string& cache_dir);

 private:
  [[nodiscard]] result<infer::runtime*> runtime();
  struct impl;
  std::unique_ptr<impl> p_;
};

}  // namespace mv::enhance
