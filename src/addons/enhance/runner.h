// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance's jobs (#297 slice 4): engine.h's runner over ports.
//
// One job at a time, on a thread of the runner's own: never the UI or render
// thread, never the player's decoder (the host's sound_* rows open their own).
// The run gate is checked before it starts: the model's `run_bytes` against
// the memory available now. The result is a new file beside the original
// (`<stem>-enhance.m4a`, or the clip with its picture copied); a cancel or a
// failure leaves none.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "addons/enhance/engine.h"
#include "addons/enhance/picture_job.h"
#include "addons/enhance/sound_job.h"

namespace mv::enhance {

struct run_port {
  struct reader {
    std::uint32_t channels = 1;
    std::int64_t total_frames = 0;
    std::function<result<std::vector<float>>(std::size_t max_frames)> read;  // closes when destroyed
  };
  struct writer {
    std::function<expected(std::span<const float> interleaved)> write;
    std::function<result<std::string>()> finish;  // the new file's path; destroyed unfinished, nothing is left
  };
  // [job thread] A file's sound at `rate`, its own channels.
  std::function<result<reader>(const std::string& path, std::uint32_t rate)> open_sound;
  // [job thread] The new file beside `source`.
  std::function<result<writer>(const std::string& source, std::uint32_t rate, std::uint32_t channels)> create;
  // [job thread] The kind's model, from its weights in `models_dir`.
  std::function<result<std::unique_ptr<frame_model>>(const model_spec& spec, const std::string& models_dir)>
      load_model;
  std::function<std::optional<std::uint64_t>()> free_memory;

  // ---- a clip's picture (slice 5) ----
  struct picture_reader {
    std::uint32_t width = 0, height = 0;
    bool hdr = false;
    std::int64_t total_frames = 0;  // an estimate
    std::function<result<bool>(std::vector<std::uint8_t>& rgb, std::int64_t& pts_us)> next;
  };
  struct clip_writer {
    std::function<expected(const std::uint8_t* rgb, std::uint32_t stride, std::int64_t pts_us)> frame;
    std::function<expected(std::span<const float> interleaved)> sound;
    std::function<result<std::string>()> finish;
  };
  std::function<result<picture_reader>(const std::string& path)> open_frames;
  // `sound_rate` 0: the source's own sound is copied.
  std::function<result<clip_writer>(const std::string& source, std::uint32_t width, std::uint32_t height,
                                    std::uint32_t sound_rate, std::uint32_t sound_channels)>
      create_clip;
  std::function<result<std::unique_ptr<tile_model>>(const model_spec& spec, const std::string& models_dir)>
      load_tiles;

  // ---- a photo (slice 3) ----
  struct still {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgb;
  };
  // [job thread] At full resolution, upright (seconds for a RAW).
  std::function<result<still>(const std::string& path)> open_still;
  // [job thread] The new photo beside `source`; its path.
  std::function<result<std::string>(const std::string& source, const still& pixels)> write_still;
};

// The largest photo Enhance writes: 100 megapixels (a 4x of a 25 MP photo is
// past it), and 16384 on its long edge.
inline constexpr std::uint64_t kMaxStillPixels = 100'000'000;
inline constexpr std::uint32_t kMaxStillEdge = 16384;

// The largest clip Enhance writes: 4K UHD (hardware encoders' common limit).
inline constexpr std::uint32_t kMaxLongEdge = 3840;
inline constexpr std::uint32_t kMaxShortEdge = 2160;

class job_runner final : public runner {
 public:
  explicit job_runner(run_port port);
  ~job_runner() override;

  [[nodiscard]] bool supports(model_kind k, item_kind item) const override;
  [[nodiscard]] std::string check(const job_request& r) const override;
  void start(job_request r, job_events events) override;
  void cancel() override;

 private:
  void run(job_request r, job_events events);
  void run_sound(job_request r, job_events events);
  void run_picture(job_request r, job_events events);
  void run_still(job_request r, job_events events);

  run_port port_;
  std::mutex m_;
  std::thread t_;
  std::atomic<bool> cancel_{false};
};

}  // namespace mv::enhance
