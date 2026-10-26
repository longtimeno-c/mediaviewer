// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/stream_model.h"

#include <array>
#include <mutex>

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

}  // namespace

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
  infer::runtime* rt = nullptr;
  {
    std::lock_guard lock(p_->m);
    if (!p_->rt) {
      MV_TRY(auto loaded, infer::runtime::load(p_->dir));
      p_->rt = std::move(loaded);
    }
    rt = p_->rt.get();
  }
  infer::session_options o;
  o.on = infer::backend::cpu;
  o.threads = 1;
  MV_TRY(auto s, infer::session::open(*rt, models_dir + "/" + spec.weights.front(), o));
  if (s->input_count() != 3 || s->output_count() < 2) return err(status::unsupported_format);
  return std::unique_ptr<frame_model>(new onnx_stream(std::move(s), *shape));
}

}  // namespace mv::enhance
