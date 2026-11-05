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

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>

#include "addons/enhance/diffusion.h"
#include "addons/enhance/face_restore.h"
#include "addons/enhance/remove.h"
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
  // [job thread] A file's sound at `rate`, its own channels. `keep`: the
  // Video Editor's kept source ranges ((in, out) ns pairs; empty: all of it),
  // the sound then on the edit's own timeline.
  std::function<result<reader>(const std::string& path, std::uint32_t rate, std::span<const std::int64_t> keep)>
      open_sound;
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
    std::int64_t total_frames = 0;  // an estimate, of the whole clip
    std::int64_t duration_ms = 0;   // of the whole clip
    std::function<result<bool>(std::vector<std::uint8_t>& rgb, std::int64_t& pts_us)> next;
  };
  struct clip_writer {
    std::function<expected(const std::uint8_t* rgb, std::uint32_t stride, std::int64_t pts_us)> frame;
    std::function<expected(std::span<const float> interleaved)> sound;
    std::function<result<std::string>()> finish;
  };
  // `keep` as open_sound's: the frames' times are then the edit's.
  std::function<result<picture_reader>(const std::string& path, std::span<const std::int64_t> keep)> open_frames;
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
    // What write_still names the new photo: "<stem>-<suffix>.jpg" ([a-z0-9_-]).
    // edit-refresh S6: Extend's is "extended".
    std::string suffix = "enhance";
  };
  // [job thread] At full resolution, upright (seconds for a RAW).
  std::function<result<still>(const std::string& path)> open_still;
  // [job thread] The new photo beside `source`; its path.
  std::function<result<std::string>(const std::string& source, const still& pixels)> write_still;

  // ---- Restyle (docs/plans/edit-adjust.md slice 5b) ----
  // [job thread] The restyle model from its files in `models_dir`.
  std::function<result<std::unique_ptr<restyle_model>>(const model_spec& spec, const std::string& models_dir, int threads)>
      load_restyle;

  // ---- Remove (docs/plans/edit-adjust.md slice 7) ----
  // [job thread] The inpainting model from its files in `models_dir`.
  std::function<result<std::unique_ptr<inpaint_filler>>(const model_spec& spec, const std::string& models_dir)>
      load_inpaint;
  // [any thread] The brush over the photo at `path` (u, v, r triples); empty none.
  std::function<std::vector<float>(const std::string& path)> still_mask;

  // ---- Restore faces (docs/plans/edit-refresh.md S10) ----
  // [job thread] The AI pack's faces in a picture (the host table's
  // still_faces): status::not_found without Local search,
  // status::permission_denied while People is off.
  faces_fn still_faces;
  // [job thread] The face model `choice` names in the spec's io.models.
  std::function<result<std::unique_ptr<face_restorer>>(const model_spec& spec, const std::string& models_dir,
                                                       const std::string& choice, int threads)>
      load_face;
  // [job thread] write_still with its own name: "<stem>-<suffix>.jpg" (doc 29).
  std::function<result<std::string>(const std::string& source, const still& pixels, const std::string& suffix)>
      write_still_as;

  // ---- Extend on the canvas (docs/plans/edit-refresh.md S6) ----
  // [any thread] The frame the person dragged past the photo's edges: left,
  // top, right, bottom, each a fraction of the edited picture's width or
  // height (remove.h extend_from_fractions); all 0 when the host set none.
  std::function<std::array<float, 4>(const std::string& path)> still_extend;
};

// The largest photo Enhance writes: 100 megapixels (a 4x of a 25 MP photo is
// past it), and 16384 on its long edge.
inline constexpr std::uint64_t kMaxStillPixels = 100'000'000;
inline constexpr std::uint32_t kMaxStillEdge = 16384;

// The largest clip Enhance writes: 4K UHD (hardware encoders' common limit).
inline constexpr std::uint32_t kMaxLongEdge = 3840;
inline constexpr std::uint32_t kMaxShortEdge = 2160;

// Restyle's tensor names from its spec (io.tensors); the defaults when absent.
[[nodiscard]] restyle_io restyle_io_of(const model_spec& spec);

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
  void run_restyle(job_request r, job_events events);
  void run_remove(job_request r, job_events events);
  void run_extend(job_request r, job_events events);
  void run_faces(job_request r, job_events events);

  run_port port_;
  std::mutex m_;
  std::thread t_;
  std::atomic<bool> cancel_{false};
};

}  // namespace mv::enhance
