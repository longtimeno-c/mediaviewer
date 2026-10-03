// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The pack's models behind plan/17's IEmbedder: the CLIP image and text
// towers (search) and the face detector + face embedder (PR 24). A model is
// a folder in the pack with a model.json that names its files and fixes its
// preprocessing, so choosing a larger tower is a pack change, not a code
// change. Embeddings come back L2-normalised.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "infer/clip_tokenizer.h"
#include "infer/ort.h"
#include "infer/preprocess.h"

namespace mv::infer {

// model.json of a CLIP folder (tools/package/ai-models.json writes them).
struct clip_spec {
  std::string id;          // "clip-vit-l14"
  std::string name;        // "CLIP ViT-L/14"
  std::string precision;   // "fp16"
  std::uint32_t quality = 1;  // mv_ai_quality: 1 fast, 2 high
  std::uint32_t dim = 512;
  clip_norm norm;
  std::string image_file;  // absolute
  std::string text_file;
  std::string vocab_file;
  std::string merges_file;
  std::uint32_t context = 77;
  float dedupe = 0.97f;           // plan/17 step 3
  float query_margin = 0.04f;     // "nothing found" below this (PR 20 calibration)...
  float query_z = 2.5f;           // ...unless the top ten stand out by this z (2026-09-27)
  float result_z = 2.0f;          // then a row this many SDs above the mean shows too
  float result_margin = 0.015f;   // a result must beat the generic prompts by this
  float similar_min = 0.62f;      // find-similar: image-to-image cosine floor
  std::vector<std::string> generic_prompts;
  // The index key: a change of model, precision or preprocessing re-queues.
  [[nodiscard]] std::string spec_key() const { return id + "/" + precision + "/pre1"; }
};

[[nodiscard]] result<clip_spec> read_clip_spec(const std::string& folder_utf8);

// plan/17's IEmbedder.
class embedder {
 public:
  virtual ~embedder() = default;
  [[nodiscard]] virtual std::uint32_t dim() const noexcept = 0;
  [[nodiscard]] virtual const std::string& spec_key() const noexcept = 0;
  [[nodiscard]] virtual backend on() const noexcept = 0;
  // Several images in one run; `out` gets images.size() * dim() floats.
  [[nodiscard]] virtual expected embed_images(std::span<const rgb_view> images,
                                              std::vector<float>& out) = 0;
  [[nodiscard]] virtual result<std::vector<float>> embed_text(std::string_view utf8) = 0;
  // The normalised mean of several texts' embeddings: a query's singular and
  // plural (addons/ai/query.h number_forms), so "mountain" and "mountains"
  // ask the same thing. This default embeds them one by one; the CLIP tower
  // runs them as one batch.
  [[nodiscard]] virtual result<std::vector<float>> embed_text_mean(std::span<const std::string> texts);
  // Why a provider asked for was not used (Settings' reason line), once known.
  [[nodiscard]] virtual provider_fault fault() const noexcept { return provider_fault::none; }
  // Still finishing an open in the background (Core ML compiling it): the
  // destructor would wait for that, so the last reference should not go on a
  // thread that must stay responsive.
  [[nodiscard]] virtual bool settling() const noexcept { return false; }
};

// Core ML's image tower runs at this fixed batch (the engine's photo batch).
inline constexpr std::size_t kCoreMLImageBatch = 4;

class clip_model final : public embedder {
 public:
  // Opens both towers on `on` (CPU is the caller's fallback, not this one's).
  [[nodiscard]] static result<std::unique_ptr<clip_model>> open(const runtime& rt,
                                                                const clip_spec& spec,
                                                                const session_options& options,
                                                                provider_fault* fault = nullptr);
  // The text tower and tokenizer only, on CPU: a reader that answers
  // descriptions against stored vectors (the search agent, plan/23) and never
  // embeds a picture. embed_images is status::unsupported_format. The vector
  // is the same as open()'s, whose text tower runs on CPU too.
  [[nodiscard]] static result<std::unique_ptr<clip_model>> open_text_only(const runtime& rt,
                                                                          const clip_spec& spec,
                                                                          const session_options& options);
  [[nodiscard]] std::uint32_t dim() const noexcept override { return spec_.dim; }
  [[nodiscard]] const std::string& spec_key() const noexcept override { return key_; }
  [[nodiscard]] backend on() const noexcept override { return image_ ? image_->on() : backend::cpu; }
  [[nodiscard]] expected embed_images(std::span<const rgb_view> images, std::vector<float>& out) override;
  [[nodiscard]] result<std::vector<float>> embed_text(std::string_view utf8) override;
  [[nodiscard]] result<std::vector<float>> embed_text_mean(std::span<const std::string> texts) override;
  [[nodiscard]] const clip_spec& spec() const noexcept { return spec_; }

 private:
  clip_model() = default;
  clip_spec spec_;
  std::string key_;
  std::unique_ptr<session> image_;
  std::unique_ptr<session> text_;
  clip_tokenizer tok_;
  std::size_t image_batch_ = 0;  // a fixed image batch (Core ML); 0 = any
};

// ---- faces ---------------------------------------------------------------------

struct face_spec {
  std::string detector_file;   // YuNet (MIT), 640 x 640 input
  std::string embedder_file;   // SFace (Apache-2.0), 112 x 112 aligned input
  std::uint32_t detector_side = 640;
  float min_confidence = 0.8f;
  float nms_iou = 0.3f;
  float min_face_fraction = 0.04f;  // faces smaller than this share of the short side are skipped
  float same_person = 0.40f;        // cosine at or above: the same person (SFace ~0.363)
  std::string spec_key = "yunet-2023mar+sface-2021dec/pre1";
};

[[nodiscard]] result<face_spec> read_face_spec(const std::string& folder_utf8);

struct face_box {
  float x = 0, y = 0, w = 0, h = 0;  // image pixels
  float score = 0;
  std::array<float, 10> landmarks{};  // image pixels
};

class face_models {
 public:
  [[nodiscard]] static result<std::unique_ptr<face_models>> open(const runtime& rt,
                                                                 const face_spec& spec,
                                                                 const session_options& options);
  [[nodiscard]] result<std::vector<face_box>> detect(const rgb_view& img) const;
  // 128 floats, L2-normalised: the mean of the aligned face and its mirror
  // (flip averaging). `aligned`, if given, receives the 3 x 112 x 112 crop.
  [[nodiscard]] result<std::vector<float>> embed(const rgb_view& img, const face_box& face,
                                                 std::vector<float>* aligned = nullptr) const;
  [[nodiscard]] const face_spec& spec() const noexcept { return spec_; }

 private:
  face_spec spec_;
  std::unique_ptr<session> detector_;
  std::unique_ptr<session> embedder_;
};

// Decodes YuNet's twelve outputs (cls/obj/bbox/kps at strides 8, 16, 32)
// into boxes in detector pixels, then NMS. Exposed for tests.
[[nodiscard]] std::vector<face_box> decode_yunet(std::span<const tensor_f32> outputs,
                                                 std::uint32_t side, float min_confidence,
                                                 float nms_iou);

// ---- vectors -------------------------------------------------------------------

void l2_normalise(std::span<float> v) noexcept;
[[nodiscard]] float dot(std::span<const float> a, std::span<const float> b) noexcept;

}  // namespace mv::infer
