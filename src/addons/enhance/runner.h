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
};

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

  run_port port_;
  std::mutex m_;
  std::thread t_;
  std::atomic<bool> cancel_{false};
};

}  // namespace mv::enhance
