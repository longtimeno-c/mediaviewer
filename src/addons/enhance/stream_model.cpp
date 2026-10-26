// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/stream_model.h"

#include <array>
#include <algorithm>
#include <mutex>
#include <thread>

#include "infer/ort.h"

namespace mv::enhance {
namespace {

std::optional<std::int64_t> whole(const json::value& io, std::string_view key, std::int64_t lo, std::int64_t hi) {
  const auto v = io.integer(key);
  if (!v || *v < lo || *v > hi) return std::nullopt;
  return v;
}

class onnx_stream final : public frame_model {
 public:
  onnx_stream(std::unique_ptr<infer::session> s, stream_shape shape) : s_(std::move(s)), shape_(shape) {
    in_[0].shape = {static_cast<std::int64_t>(shape_.frame)};
    in_[1].shape = {static_cast<std::int64_t>(shape_.state)};
    in_[2].shape = {};
    in_[2].data = {0.0f};
  }
  std::size_t frame() const noexcept override { return shape_.frame; }
  std::size_t delay() const noexcept override { return shape_.delay; }
  std::vector<float> initial_state() const override { return std::vector<float>(shape_.state, 0.0f); }

  expected process(std::span<const float> in, std::vector<float>& state, std::span<float> out) override {
    if (in.size() != shape_.frame || out.size() != shape_.frame || state.size() != shape_.state) {
      return err(status::invalid_arg);
    }
    in_[0].data.assign(in.begin(), in.end());
    in_[1].data.swap(state);
    auto r = s_->run(in_);
    in_[1].data.swap(state);
    if (!r) return err(r.error());
    if (r->size() < 2 || (*r)[0].data.size() != shape_.frame || (*r)[1].data.size() != shape_.state) {
      return err(status::corrupt);
    }
    std::copy((*r)[0].data.begin(), (*r)[0].data.end(), out.begin());
    state.swap((*r)[1].data);
    return {};
  }

 private:
  std::unique_ptr<infer::session> s_;
  stream_shape shape_;
  std::array<infer::tensor_f32, 3> in_;
};

class onnx_tiles final : public tile_model {
 public:
  onnx_tiles(std::unique_ptr<infer::session> s, tile_shape shape) : s_(std::move(s)), shape_(std::move(shape)) {
    in_[0].shape = {1, 3, shape_.tile, shape_.tile};
  }
  std::uint32_t tile() const noexcept override { return shape_.tile; }
  std::uint32_t scale() const noexcept override { return shape_.scale; }
  expected run(std::span<const float> in, std::vector<float>& out) override {
    const std::size_t want = std::size_t{3} * shape_.tile * shape_.tile;
    if (in.size() != want) return err(status::invalid_arg);
    in_[0].data.assign(in.begin(), in.end());
    auto r = s_->run(in_);
    if (!r) return err(r.error());
    if (r->empty() || (*r)[0].data.size() != want * shape_.scale * shape_.scale) return err(status::corrupt);
    out.swap((*r)[0].data);
    return {};
  }

 private:
  std::unique_ptr<infer::session> s_;
  tile_shape shape_;
  std::array<infer::tensor_f32, 1> in_;
};

}  // namespace

std::optional<tile_shape> tile_shape_of(const model_spec& spec) {
  const json::value& io = spec.io;
  const std::string* runner = io.str("runner");
  if (!runner || *runner != "tiles") return std::nullopt;
  const auto scale = whole(io, "scale", 1, 8);
  const auto tile = whole(io, "tile", 32, 1024);
  const auto overlap = whole(io, "overlap", 0, 256);
  const json::value* dims = io.find("dims");
  if (!scale || !tile || !overlap || 2 * *overlap >= *tile || !dims || dims->k != json::kind::array ||
      dims->a.size() != 2 || dims->a[0].k != json::kind::string || dims->a[1].k != json::kind::string) {
    return std::nullopt;
  }
  return tile_shape{static_cast<std::uint32_t>(*scale), static_cast<std::uint32_t>(*tile),
                    static_cast<std::uint32_t>(*overlap), dims->a[0].s, dims->a[1].s};
}

std::optional<stream_shape> stream_shape_of(const model_spec& spec) {
  const json::value& io = spec.io;
  const std::string* runner = io.str("runner");
  if (!runner || *runner != "stream") return std::nullopt;
  const auto rate = whole(io, "rate", 8000, 192000);
  const auto frame = whole(io, "frame", 16, 1 << 16);
  const auto state = whole(io, "state", 1, 1 << 24);
  const auto delay = whole(io, "delay", 0, 1 << 20);
  if (!rate || !frame || !state || !delay) return std::nullopt;
  return stream_shape{static_cast<std::uint32_t>(*rate), static_cast<std::size_t>(*frame),
                      static_cast<std::size_t>(*state), static_cast<std::size_t>(*delay)};
}

struct runtime_holder::impl {
  std::string dir;
  std::mutex m;
  std::unique_ptr<infer::runtime> rt;
};

runtime_holder::runtime_holder(std::string dir) : p_(std::make_unique<impl>()) { p_->dir = std::move(dir); }
runtime_holder::~runtime_holder() = default;

result<std::unique_ptr<frame_model>> runtime_holder::open(const model_spec& spec, const std::string& models_dir) {
  const auto shape = stream_shape_of(spec);
  if (!shape || spec.weights.empty()) return err(status::unsupported_format);
  MV_TRY(infer::runtime * rt, runtime());
  infer::session_options o;
  o.on = infer::backend::cpu;
  o.threads = 1;
  MV_TRY(auto s, infer::session::open(*rt, models_dir + "/" + spec.weights.front(), o));
  if (s->input_count() != 3 || s->output_count() < 2) return err(status::unsupported_format);
  return std::unique_ptr<frame_model>(new onnx_stream(std::move(s), *shape));
}

result<infer::runtime*> runtime_holder::runtime() {
  std::lock_guard lock(p_->m);
  if (!p_->rt) {
    MV_TRY(auto loaded, infer::runtime::load(p_->dir));
    p_->rt = std::move(loaded);
  }
  return p_->rt.get();
}

result<std::unique_ptr<tile_model>> runtime_holder::open_tiles(const model_spec& spec, const std::string& models_dir,
                                                               const std::string& cache_dir) {
  const auto shape = tile_shape_of(spec);
  if (!shape || spec.weights.empty()) return err(status::unsupported_format);
  MV_TRY(infer::runtime * rt, runtime());
  const std::string file = models_dir + "/" + spec.weights.front();
  infer::session_options o;
  o.fixed_dims = {{shape->dim_h, shape->tile}, {shape->dim_w, shape->tile}};
  if (rt->has_provider(infer::backend::coreml)) {
    o.on = infer::backend::coreml;
    o.cache_dir_utf8 = cache_dir;
    if (auto s = infer::session::open(*rt, file, o); s && (*s)->on() == infer::backend::coreml) {
      return std::unique_ptr<tile_model>(new onnx_tiles(std::move(*s), *shape));
    }
  }
  o.on = infer::backend::cpu;
  o.cache_dir_utf8.clear();
  o.threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 2u, 9u) - 1);
  MV_TRY(auto s, infer::session::open(*rt, file, o));
  return std::unique_ptr<tile_model>(new onnx_tiles(std::move(s), *shape));
}

}  // namespace mv::enhance
