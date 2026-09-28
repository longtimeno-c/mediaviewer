// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "infer/models.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/json.h"

namespace mv::infer {
namespace {

std::filesystem::path fs_path(const std::string& utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string join(const std::string& dir, const std::string& rel) {
  const std::u8string u = (fs_path(dir) / fs_path(rel)).lexically_normal().generic_u8string();
  return std::string(u.begin(), u.end());
}

result<std::string> read_text(const std::string& path) {
  std::ifstream in(fs_path(path), std::ios::binary);
  if (!in) return err(status::io);
  std::ostringstream ss;
  ss << in.rdbuf();
  if (!in && !in.eof()) return err(status::io);
  return ss.str();
}

bool read_triplet(const json::value* v, std::array<float, 3>& out) {
  if (!v || v->k != json::kind::array || v->a.size() != 3) return false;
  for (std::size_t i = 0; i < 3; ++i) {
    if (v->a[i].k != json::kind::number) return false;
    out[i] = static_cast<float>(v->a[i].is_integer ? static_cast<double>(v->a[i].i) : v->a[i].d);
  }
  return true;
}

float number_or(const json::value& doc, std::string_view key, float fallback) {
  const json::value* v = doc.find(key);
  if (!v || v->k != json::kind::number) return fallback;
  return static_cast<float>(v->is_integer ? static_cast<double>(v->i) : v->d);
}

// A relative path inside the model folder (never "..", never absolute).
bool inside(const std::string& rel) {
  if (rel.empty() || rel.front() == '/' || rel.front() == '\\' || rel.find(':') != std::string::npos) {
    return false;
  }
  for (const auto& part : fs_path(rel)) {
    if (part == "..") return false;
  }
  return true;
}

}  // namespace

void l2_normalise(std::span<float> v) noexcept {
  double n = 0;
  for (float x : v) n += static_cast<double>(x) * x;
  n = std::sqrt(n);
  if (n <= 0) return;
  for (float& x : v) x = static_cast<float>(x / n);
}

float dot(std::span<const float> a, std::span<const float> b) noexcept {
  const std::size_t n = std::min(a.size(), b.size());
  float s = 0;
  for (std::size_t i = 0; i < n; ++i) s += a[i] * b[i];
  return s;
}

result<clip_spec> read_clip_spec(const std::string& folder) {
  MV_TRY(std::string text, read_text(join(folder, "model.json")));
  const auto doc = json::parse(text, 8);
  if (!doc || doc->k != json::kind::object) return err(status::corrupt);
  clip_spec s;
  const std::string* id = doc->str("id");
  const std::string* name = doc->str("name");
  const std::string* precision = doc->str("precision");
  const std::string* image = doc->str("image");
  const std::string* text_file = doc->str("text");
  const std::string* tokenizer = doc->str("tokenizer");
  const auto dim = doc->integer("dim");
  const auto size = doc->integer("image_size");
  if (!id || !name || !precision || !image || !text_file || !tokenizer || !dim || !size || *dim <= 0 ||
      *dim > 4096 || *size < 32 || *size > 1024 || !inside(*image) || !inside(*text_file)) {
    return err(status::corrupt);
  }
  // The tokenizer folder is shared by the towers: a sibling of this folder.
  const std::string tok_dir = join(join(folder, ".."), *tokenizer);
  if (tokenizer->find("..") != std::string::npos || tokenizer->find('/') != std::string::npos ||
      tokenizer->find('\\') != std::string::npos) {
    return err(status::corrupt);
  }
  s.id = *id;
  s.name = *name;
  s.precision = *precision;
  s.dim = static_cast<std::uint32_t>(*dim);
  s.norm.size = static_cast<std::uint32_t>(*size);
  if (const json::value* m = doc->find("mean"); m && !read_triplet(m, s.norm.mean)) return err(status::corrupt);
  if (const json::value* sd = doc->find("std"); sd && !read_triplet(sd, s.norm.std)) return err(status::corrupt);
  s.quality = static_cast<std::uint32_t>(doc->integer("quality").value_or(1));
  s.context = static_cast<std::uint32_t>(doc->integer("context").value_or(77));
  s.dedupe = number_or(*doc, "dedupe", s.dedupe);
  s.query_margin = number_or(*doc, "query_margin", s.query_margin);
  s.query_z = number_or(*doc, "query_z", s.query_z);
  s.result_z = number_or(*doc, "result_z", s.result_z);
  s.result_margin = number_or(*doc, "result_margin", s.result_margin);
  s.similar_min = number_or(*doc, "similar_min", s.similar_min);
  if (const json::value* g = doc->find("generic_prompts"); g && g->k == json::kind::array) {
    for (const auto& p : g->a) {
      if (p.k == json::kind::string && !p.s.empty()) s.generic_prompts.push_back(p.s);
    }
  }
  if (s.generic_prompts.empty()) {
    s.generic_prompts = {"a photo.", "a picture of something.", "an image.", "a photo of a thing.",
                         "a blurry photo."};
  }
  s.image_file = join(folder, *image);
  s.text_file = join(folder, *text_file);
  s.vocab_file = join(tok_dir, "vocab.json");
  s.merges_file = join(tok_dir, "merges.txt");
  return s;
}

result<std::unique_ptr<clip_model>> clip_model::open(const runtime& rt, const clip_spec& spec,
                                                     const session_options& options,
                                                     provider_fault* fault) {
  std::unique_ptr<clip_model> m(new clip_model());
  m->spec_ = spec;
  m->key_ = spec.spec_key();
  MV_TRY(std::string vocab, read_text(spec.vocab_file));
  MV_TRY(std::string merges, read_text(spec.merges_file));
  MV_TRY(clip_tokenizer tok, clip_tokenizer::load(vocab, merges, spec.context));
  m->tok_ = std::move(tok);
  session_options image_opts = options;
  if (options.on == backend::coreml) {
    // Core ML needs static shapes (ort.h): the image tower runs four at a time,
    // the engine's photo batch; embed_images splits and pads to it.
    const auto side = static_cast<std::int64_t>(spec.norm.size);
    m->image_batch_ = kCoreMLImageBatch;
    image_opts.fixed_dims = {{"batch_size", static_cast<std::int64_t>(kCoreMLImageBatch)},
                             {"num_channels", 3}, {"height", side}, {"width", side}};
  }
  MV_TRY(auto image, session::open(rt, spec.image_file, image_opts, fault));
  m->image_ = std::move(image);
  // The text tower is tiny per query and latency-bound: CPU keeps it off a
  // provider's queue and gives the same vector on every compute choice.
  session_options text_opts = options;
  text_opts.on = backend::cpu;
  MV_TRY(auto text, session::open(rt, spec.text_file, text_opts, nullptr));
  m->text_ = std::move(text);
  return m;
}

expected clip_model::embed_images(std::span<const rgb_view> images, std::vector<float>& out) {
  out.clear();
  if (images.empty()) return {};
  if (image_batch_ > 0 && images.size() != image_batch_) {
    // A fixed batch: run it in slices, padding the last with its final image.
    std::vector<rgb_view> slice(image_batch_);
    std::vector<float> part;
    for (std::size_t at = 0; at < images.size(); at += image_batch_) {
      const std::size_t k = std::min(image_batch_, images.size() - at);
      for (std::size_t i = 0; i < image_batch_; ++i) slice[i] = images[at + std::min(i, k - 1)];
      MV_TRY_VOID(embed_images(slice, part));
      out.insert(out.end(), part.begin(), part.begin() + static_cast<std::ptrdiff_t>(k * spec_.dim));
    }
    return {};
  }
  tensor_f32 in;
  const auto n = static_cast<std::int64_t>(images.size());
  const auto s = static_cast<std::int64_t>(spec_.norm.size);
  in.shape = {n, 3, s, s};
  in.data.reserve(static_cast<std::size_t>(n * 3 * s * s));
  for (const rgb_view& img : images) {
    if (!img.rgb || img.width == 0 || img.height == 0) return err(status::invalid_arg);
    clip_tensor(img, spec_.norm, in.data);
  }
  MV_TRY(auto outs, image_->run(std::span<const tensor_f32>(&in, 1)));
  if (outs.empty()) return err(status::internal);
  // The projected embedding: [n, dim] (some exports add a pooled output first).
  const tensor_f32* e = nullptr;
  for (const tensor_f32& t : outs) {
    if (t.shape.size() == 2 && t.shape[0] == n && t.shape[1] == spec_.dim) {
      e = &t;
      break;
    }
  }
  if (!e) return err(status::corrupt);
  out = e->data;
  for (std::int64_t i = 0; i < n; ++i) {
    l2_normalise(std::span<float>(out.data() + i * spec_.dim, spec_.dim));
  }
  return {};
}

result<std::vector<float>> clip_model::embed_text(std::string_view utf8) {
  const std::vector<std::int64_t> ids = tok_.encode(utf8);
  tensor_i64 in;
  in.shape = {1, static_cast<std::int64_t>(ids.size())};
  in.data = ids;
  MV_TRY(auto outs, text_->run_ids(in));
  for (const tensor_f32& t : outs) {
    if (t.shape.size() == 2 && t.shape[0] == 1 && t.shape[1] == spec_.dim) {
      std::vector<float> v = t.data;
      l2_normalise(v);
      return v;
    }
  }
  return err(status::corrupt);
}

// ---- faces ---------------------------------------------------------------------

result<face_spec> read_face_spec(const std::string& folder) {
  MV_TRY(std::string text, read_text(join(folder, "model.json")));
  const auto doc = json::parse(text, 8);
  if (!doc || doc->k != json::kind::object) return err(status::corrupt);
  const std::string* det = doc->str("detector");
  const std::string* emb = doc->str("embedder");
  if (!det || !emb || !inside(*det) || !inside(*emb)) return err(status::corrupt);
  face_spec s;
  s.detector_file = join(folder, *det);
  s.embedder_file = join(folder, *emb);
  s.detector_side = static_cast<std::uint32_t>(doc->integer("detector_side").value_or(640));
  s.min_confidence = number_or(*doc, "min_confidence", s.min_confidence);
  s.nms_iou = number_or(*doc, "nms_iou", s.nms_iou);
  s.min_face_fraction = number_or(*doc, "min_face_fraction", s.min_face_fraction);
  s.same_person = number_or(*doc, "same_person", s.same_person);
  if (const std::string* key = doc->str("spec")) s.spec_key = *key;
  return s;
}

result<std::unique_ptr<face_models>> face_models::open(const runtime& rt, const face_spec& spec,
                                                       const session_options& options) {
  auto m = std::make_unique<face_models>();
  m->spec_ = spec;
  MV_TRY(auto det, session::open(rt, spec.detector_file, options, nullptr));
  MV_TRY(auto emb, session::open(rt, spec.embedder_file, options, nullptr));
  m->detector_ = std::move(det);
  m->embedder_ = std::move(emb);
  return m;
}

std::vector<face_box> decode_yunet(std::span<const tensor_f32> outs, std::uint32_t side,
                                   float min_confidence, float nms_iou) {
  std::vector<face_box> found;
  if (outs.size() != 12) return found;
  constexpr int kStrides[3] = {8, 16, 32};
  for (int si = 0; si < 3; ++si) {
    const int stride = kStrides[si];
    const int cols = static_cast<int>(side) / stride;
    const int rows = static_cast<int>(side) / stride;
    const auto n = static_cast<std::size_t>(cols) * rows;
    const tensor_f32& cls = outs[static_cast<std::size_t>(si)];
    const tensor_f32& obj = outs[static_cast<std::size_t>(si + 3)];
    const tensor_f32& box = outs[static_cast<std::size_t>(si + 6)];
    const tensor_f32& kps = outs[static_cast<std::size_t>(si + 9)];
    if (cls.data.size() < n || obj.data.size() < n || box.data.size() < n * 4 || kps.data.size() < n * 10) {
      continue;
    }
    for (int r = 0; r < rows; ++r) {
      for (int c = 0; c < cols; ++c) {
        const std::size_t idx = static_cast<std::size_t>(r) * cols + c;
        const float cs = std::clamp(cls.data[idx], 0.0f, 1.0f);
        const float os = std::clamp(obj.data[idx], 0.0f, 1.0f);
        const float score = std::sqrt(cs * os);
        if (score < min_confidence) continue;
        face_box f;
        f.score = score;
        const float cx = (c + box.data[idx * 4]) * stride;
        const float cy = (r + box.data[idx * 4 + 1]) * stride;
        f.w = std::exp(box.data[idx * 4 + 2]) * stride;
        f.h = std::exp(box.data[idx * 4 + 3]) * stride;
        f.x = cx - f.w / 2;
        f.y = cy - f.h / 2;
        for (int k = 0; k < 5; ++k) {
          f.landmarks[static_cast<std::size_t>(k * 2)] = (kps.data[idx * 10 + k * 2] + c) * stride;
          f.landmarks[static_cast<std::size_t>(k * 2 + 1)] = (kps.data[idx * 10 + k * 2 + 1] + r) * stride;
        }
        found.push_back(f);
      }
    }
  }
  std::sort(found.begin(), found.end(), [](const face_box& a, const face_box& b) { return a.score > b.score; });
  std::vector<face_box> kept;
  const auto iou = [](const face_box& a, const face_box& b) {
    const float x1 = std::max(a.x, b.x), y1 = std::max(a.y, b.y);
    const float x2 = std::min(a.x + a.w, b.x + b.w), y2 = std::min(a.y + a.h, b.y + b.h);
    const float inter = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
    const float uni = a.w * a.h + b.w * b.h - inter;
    return uni > 0 ? inter / uni : 0.0f;
  };
  for (const face_box& f : found) {
    if (std::none_of(kept.begin(), kept.end(), [&](const face_box& k) { return iou(k, f) > nms_iou; })) {
      kept.push_back(f);
    }
  }
  return kept;
}

result<std::vector<face_box>> face_models::detect(const rgb_view& img) const {
  tensor_f32 in;
  float scale = 1;
  yunet_tensor(img, spec_.detector_side, in.data, scale);
  const auto side = static_cast<std::int64_t>(spec_.detector_side);
  in.shape = {1, 3, side, side};
  MV_TRY(auto outs, detector_->run(std::span<const tensor_f32>(&in, 1)));
  std::vector<face_box> faces = decode_yunet(outs, spec_.detector_side, spec_.min_confidence, spec_.nms_iou);
  const float min_side = spec_.min_face_fraction * static_cast<float>(std::min(img.width, img.height));
  std::vector<face_box> out;
  for (face_box f : faces) {
    f.x *= scale;
    f.y *= scale;
    f.w *= scale;
    f.h *= scale;
    for (float& v : f.landmarks) v *= scale;
    if (std::min(f.w, f.h) < min_side) continue;
    out.push_back(f);
  }
  return out;
}

result<std::vector<float>> face_models::embed(const rgb_view& img, const face_box& face) const {
  tensor_f32 in;
  sface_tensor(img, face.landmarks, in.data);
  in.shape = {1, 3, 112, 112};
  MV_TRY(auto outs, embedder_->run(std::span<const tensor_f32>(&in, 1)));
  if (outs.empty() || outs[0].data.empty()) return err(status::corrupt);
  std::vector<float> v = outs[0].data;
  l2_normalise(v);
  return v;
}

}  // namespace mv::infer
