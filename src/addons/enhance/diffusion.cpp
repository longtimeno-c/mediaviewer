// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/diffusion.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <sstream>
#include <thread>
#include <utility>

#include "infer/clip_tokenizer.h"
#include "infer/ort.h"

namespace mv::enhance {
namespace {

constexpr float kVaeScale = 0.18215f;  // Stable Diffusion's latent scaling
constexpr int kTokens = 77;
constexpr int kHidden = 768;

result<std::string> read_text(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return err(status::io);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

}  // namespace

euler_schedule make_euler_schedule(int steps) {
  euler_schedule s;
  if (steps < 1) return s;
  constexpr int kTrain = 1000;
  std::vector<double> sigma_all(kTrain);
  double cum = 1.0;
  const double b0 = std::sqrt(0.00085), b1 = std::sqrt(0.012);
  for (int i = 0; i < kTrain; ++i) {
    const double b = b0 + (b1 - b0) * i / (kTrain - 1);
    cum *= 1.0 - b * b;
    sigma_all[static_cast<std::size_t>(i)] = std::sqrt((1.0 - cum) / cum);
  }
  for (int i = 0; i < steps; ++i) {
    // linspace(0, 999, steps), reversed.
    const double t = steps == 1 ? 0.0 : static_cast<double>(kTrain - 1) * (steps - 1 - i) / (steps - 1);
    s.timesteps.push_back(static_cast<float>(t));
    const auto lo = static_cast<std::size_t>(std::floor(t));
    const std::size_t hi = std::min<std::size_t>(lo + 1, kTrain - 1);
    const double f = t - static_cast<double>(lo);
    s.sigmas.push_back(static_cast<float>(sigma_all[lo] + (sigma_all[hi] - sigma_all[lo]) * f));
  }
  s.sigmas.push_back(0.0f);
  s.init = *std::max_element(s.sigmas.begin(), s.sigmas.end());  // linspace: the largest sigma
  return s;
}

void restyle_size(std::uint32_t w, std::uint32_t h, std::uint32_t edge, std::uint32_t& out_w, std::uint32_t& out_h) {
  const double k = static_cast<double>(edge) / static_cast<double>(std::max(std::max(w, h), 1u));
  // Multiples of 64: the VAE takes 8 off and the UNet halves the latent three
  // times, so a side that is not a multiple of 64 fails its skip joins.
  auto round64 = [](double v) { return std::max<std::uint32_t>(64, static_cast<std::uint32_t>(std::lround(v / 64.0)) * 64); };
  out_w = round64(w * k);
  out_h = round64(h * k);
}

rgb8 resize_rgb(const rgb8& src, std::uint32_t width, std::uint32_t height) {
  rgb8 out;
  out.width = width;
  out.height = height;
  out.rgb.resize(static_cast<std::size_t>(width) * height * 3);
  if (src.width == 0 || src.height == 0) return out;
  // Shrinking a lot: average the source pixels each output pixel covers;
  // else bilinear between pixel centres.
  const double sx = static_cast<double>(src.width) / width, sy = static_cast<double>(src.height) / height;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      double acc[3] = {0, 0, 0};
      if (sx > 1.5 || sy > 1.5) {
        const auto x0 = static_cast<std::uint32_t>(x * sx), x1 = std::max(x0 + 1, static_cast<std::uint32_t>((x + 1) * sx));
        const auto y0 = static_cast<std::uint32_t>(y * sy), y1 = std::max(y0 + 1, static_cast<std::uint32_t>((y + 1) * sy));
        std::size_t n = 0;
        for (std::uint32_t yy = y0; yy < std::min(y1, src.height); ++yy) {
          for (std::uint32_t xx = x0; xx < std::min(x1, src.width); ++xx) {
            const std::uint8_t* p = &src.rgb[(static_cast<std::size_t>(yy) * src.width + xx) * 3];
            for (int k = 0; k < 3; ++k) acc[k] += p[k];
            ++n;
          }
        }
        for (double& a : acc) a /= static_cast<double>(std::max<std::size_t>(n, 1));
      } else {
        const double fx = std::clamp((x + 0.5) * sx - 0.5, 0.0, src.width - 1.0);
        const double fy = std::clamp((y + 0.5) * sy - 0.5, 0.0, src.height - 1.0);
        const auto ix = static_cast<std::uint32_t>(fx), iy = static_cast<std::uint32_t>(fy);
        const std::uint32_t jx = std::min(ix + 1, src.width - 1), jy = std::min(iy + 1, src.height - 1);
        const double ax = fx - ix, ay = fy - iy;
        auto px = [&](std::uint32_t xx, std::uint32_t yy, int k) {
          return static_cast<double>(src.rgb[(static_cast<std::size_t>(yy) * src.width + xx) * 3 + static_cast<std::size_t>(k)]);
        };
        for (int k = 0; k < 3; ++k) {
          acc[k] = (px(ix, iy, k) * (1 - ax) + px(jx, iy, k) * ax) * (1 - ay) +
                   (px(ix, jy, k) * (1 - ax) + px(jx, jy, k) * ax) * ay;
        }
      }
      std::uint8_t* o = &out.rgb[(static_cast<std::size_t>(y) * width + x) * 3];
      for (int k = 0; k < 3; ++k) o[k] = static_cast<std::uint8_t>(std::lround(std::clamp(acc[k], 0.0, 255.0)));
    }
  }
  return out;
}

rgb8 carry_back(const rgb8& full, const rgb8& before, const rgb8& after, float strength) {
  if (before.width != after.width || before.height != after.height || before.width == 0) return full;
  const std::uint32_t w = before.width, h = before.height;
  // The ratio of after to before per channel, blurred over 3 x 3 so a pixel's
  // own noise does not become a pattern, kept within x0.2 .. x5.
  std::vector<float> ratio(static_cast<std::size_t>(w) * h * 3);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      for (int k = 0; k < 3; ++k) {
        double a = 0, b = 0;
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dx = -1; dx <= 1; ++dx) {
            const auto xx = static_cast<std::uint32_t>(std::clamp<int>(static_cast<int>(x) + dx, 0, static_cast<int>(w) - 1));
            const auto yy = static_cast<std::uint32_t>(std::clamp<int>(static_cast<int>(y) + dy, 0, static_cast<int>(h) - 1));
            const std::size_t i = (static_cast<std::size_t>(yy) * w + xx) * 3 + static_cast<std::size_t>(k);
            a += after.rgb[i];
            b += before.rgb[i];
          }
        }
        const float r = static_cast<float>((a / 9.0 + 8.0) / (b / 9.0 + 8.0));
        ratio[(static_cast<std::size_t>(y) * w + x) * 3 + static_cast<std::size_t>(k)] =
            1.0f + (std::clamp(r, 0.2f, 5.0f) - 1.0f) * std::clamp(strength, 0.0f, 1.0f);
      }
    }
  }
  rgb8 out = full;
  const double sx = static_cast<double>(w) / full.width, sy = static_cast<double>(h) / full.height;
  for (std::uint32_t y = 0; y < full.height; ++y) {
    const double fy = std::clamp((y + 0.5) * sy - 0.5, 0.0, h - 1.0);
    const auto iy = static_cast<std::uint32_t>(fy);
    const std::uint32_t jy = std::min(iy + 1, h - 1);
    const double ay = fy - iy;
    for (std::uint32_t x = 0; x < full.width; ++x) {
      const double fx = std::clamp((x + 0.5) * sx - 0.5, 0.0, w - 1.0);
      const auto ix = static_cast<std::uint32_t>(fx);
      const std::uint32_t jx = std::min(ix + 1, w - 1);
      const double ax = fx - ix;
      std::uint8_t* o = &out.rgb[(static_cast<std::size_t>(y) * full.width + x) * 3];
      for (int k = 0; k < 3; ++k) {
        auto r = [&](std::uint32_t xx, std::uint32_t yy) {
          return ratio[(static_cast<std::size_t>(yy) * w + xx) * 3 + static_cast<std::size_t>(k)];
        };
        const double rr = (r(ix, iy) * (1 - ax) + r(jx, iy) * ax) * (1 - ay) + (r(ix, jy) * (1 - ax) + r(jx, jy) * ax) * ay;
        o[k] = static_cast<std::uint8_t>(std::lround(std::clamp((o[k] + 8.0) * rr - 8.0, 0.0, 255.0)));
      }
    }
  }
  return out;
}

struct restyle_model::impl {
  infer::runtime* rt = nullptr;
  std::string dir, cache;
  std::unique_ptr<infer::session> text, enc, dec;
  std::map<std::pair<std::int64_t, std::int64_t>, std::unique_ptr<infer::session>> unets;  // by latent h, w
  std::unique_ptr<infer::clip_tokenizer> tok;
  restyle_io io;
  bool gpu = false;

  result<infer::session*> unet(std::int64_t h, std::int64_t w) {
    const auto key = std::make_pair(h, w);
    if (auto it = unets.find(key); it != unets.end()) return it->second.get();
    const std::string file = dir + "/unet.onnx";
    infer::session_options o;
    o.fixed_dims = {{io.dim_batch, 3}, {io.dim_h, h}, {io.dim_w, w}};
    for (const auto& [name, size] : io.fixed) o.fixed_dims.emplace_back(name, size);
    if (io.coreml && rt->has_provider(infer::backend::coreml)) {
      o.on = infer::backend::coreml;
      o.cache_dir_utf8 = cache;
      if (auto s = infer::session::open(*rt, file, o); s && (*s)->on() == infer::backend::coreml) {
        gpu = true;
        return (unets[key] = std::move(*s)).get();
      }
    }
    o.on = infer::backend::cpu;
    o.cache_dir_utf8.clear();
    o.threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 2u, 9u) - 1);
    MV_TRY(auto s, infer::session::open(*rt, file, o));
    return (unets[key] = std::move(s)).get();
  }
};

restyle_model::restyle_model(std::unique_ptr<impl> p) : p_(std::move(p)) {}
restyle_model::~restyle_model() = default;
bool restyle_model::on_gpu() const noexcept { return p_->gpu; }

expected restyle_model::prepare(std::uint32_t width, std::uint32_t height) {
  if (width % 64 != 0 || height % 64 != 0 || width == 0 || height == 0) return err(status::invalid_arg);
  MV_TRY(infer::session * s, p_->unet(static_cast<std::int64_t>(height / 8), static_cast<std::int64_t>(width / 8)));
  (void)s;
  return {};
}

result<std::unique_ptr<restyle_model>> restyle_model::open(infer::runtime& rt, const std::string& dir,
                                                           const std::string& cache_dir, restyle_io io) {
  auto p = std::make_unique<impl>();
  p->io = std::move(io);
  p->rt = &rt;
  p->dir = dir;
  p->cache = cache_dir;
  MV_TRY(std::string vocab, read_text(dir + "/vocab.json"));
  MV_TRY(std::string merges, read_text(dir + "/merges.txt"));
  MV_TRY(infer::clip_tokenizer tok, infer::clip_tokenizer::load(vocab, merges, kTokens));
  p->tok = std::make_unique<infer::clip_tokenizer>(std::move(tok));
  infer::session_options cpu;
  cpu.on = infer::backend::cpu;
  cpu.threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 2u, 9u) - 1);
  MV_TRY(p->text, infer::session::open(rt, dir + "/text_encoder.onnx", cpu));
  MV_TRY(p->enc, infer::session::open(rt, dir + "/vae_encoder.onnx", cpu));
  MV_TRY(p->dec, infer::session::open(rt, dir + "/vae_decoder.onnx", cpu));
  return std::unique_ptr<restyle_model>(new restyle_model(std::move(p)));
}

result<rgb8> restyle_model::run(const rgb8& small, const restyle_params& prm,
                                const std::function<bool(int, int)>& progress) {
  if (small.width % 64 != 0 || small.height % 64 != 0 || small.width == 0 || small.height == 0 || prm.steps < 1) {
    return err(status::invalid_arg);
  }
  const std::int64_t W = small.width, H = small.height, w = W / 8, h = H / 8;
  // 1. The prompt and the empty prompt, padded with the end token to 77.
  std::vector<std::int64_t> ids(2 * kTokens, p_->tok->end_id());
  for (int row = 0; row < 2; ++row) {
    const std::vector<std::int64_t> t = p_->tok->encode(row == 0 ? prm.prompt : std::string());
    std::copy_n(t.begin(), std::min<std::size_t>(t.size(), kTokens), ids.begin() + row * kTokens);
  }
  const infer::session::named_input text_in{p_->io.ids, {2, kTokens}, infer::session::element::i64, nullptr,
                                            ids.data(), nullptr, ids.size()};
  MV_TRY(auto text, p_->text->run_named(std::span(&text_in, 1)));
  if (text.empty() || text[0].data.size() != static_cast<std::size_t>(2 * kTokens * kHidden)) return err(status::internal);
  const std::size_t te = static_cast<std::size_t>(kTokens * kHidden);
  // [prompt, empty, empty]: text + picture, picture alone, neither.
  std::vector<float> hidden(3 * te);
  std::copy_n(text[0].data.begin(), te, hidden.begin());
  std::copy_n(text[0].data.begin() + static_cast<std::ptrdiff_t>(te), te, hidden.begin() + static_cast<std::ptrdiff_t>(te));
  std::copy_n(text[0].data.begin() + static_cast<std::ptrdiff_t>(te), te, hidden.begin() + static_cast<std::ptrdiff_t>(2 * te));

  // 2. The picture's latent.
  const std::size_t px = static_cast<std::size_t>(W * H);
  std::vector<float> image(3 * px);
  for (std::size_t i = 0; i < px; ++i) {
    for (int k = 0; k < 3; ++k) image[static_cast<std::size_t>(k) * px + i] = small.rgb[i * 3 + static_cast<std::size_t>(k)] / 127.5f - 1.0f;
  }
  const infer::session::named_input enc_in{p_->io.encode_in, {1, 3, H, W}, infer::session::element::f32, image.data(),
                                           nullptr, nullptr, image.size()};
  MV_TRY(auto enc, p_->enc->run_named(std::span(&enc_in, 1)));
  const std::size_t lat = static_cast<std::size_t>(4 * h * w);
  if (enc.empty() || enc[0].data.size() != lat) return err(status::internal);
  const std::vector<float>& image_lat = enc[0].data;

  // 3. The steps.
  const euler_schedule s = make_euler_schedule(prm.steps);
  std::mt19937_64 rng(prm.seed);
  std::normal_distribution<float> normal(0.0f, 1.0f);
  std::vector<float> latents(lat);
  for (float& v : latents) v = normal(rng) * s.init;
  MV_TRY(infer::session * unet, p_->unet(h, w));
  std::vector<float> sample(3 * 2 * lat);
  for (int i = 0; i < prm.steps; ++i) {
    if (progress && !progress(i, prm.steps)) return err(status::cancelled);
    const float sigma = s.sigmas[static_cast<std::size_t>(i)];
    const float in_scale = 1.0f / std::sqrt(sigma * sigma + 1.0f);
    for (int b = 0; b < 3; ++b) {
      float* dst = sample.data() + static_cast<std::size_t>(b) * 2 * lat;
      for (std::size_t j = 0; j < lat; ++j) dst[j] = latents[j] * in_scale;
      for (std::size_t j = 0; j < lat; ++j) dst[lat + j] = b < 2 ? image_lat[j] : 0.0f;
    }
    const std::vector<float> t(3, s.timesteps[static_cast<std::size_t>(i)]);
    const infer::session::named_input in[3] = {
        {p_->io.sample, {3, 8, h, w}, infer::session::element::f32, sample.data(), nullptr, nullptr, sample.size()},
        {p_->io.timestep, {3}, infer::session::element::f32, t.data(), nullptr, nullptr, t.size()},
        {p_->io.hidden, {3, kTokens, kHidden}, infer::session::element::f32, hidden.data(), nullptr, nullptr, hidden.size()},
    };
    MV_TRY(auto eps, unet->run_named(std::span(in, 3)));
    if (eps.empty() || eps[0].data.size() != 3 * lat) return err(status::internal);
    const float* e_text = eps[0].data.data();
    const float* e_image = e_text + lat;
    const float* e_none = e_image + lat;
    // Euler ancestral (epsilon prediction).
    const float to = s.sigmas[static_cast<std::size_t>(i) + 1];
    const float up = std::sqrt(std::max(0.0f, to * to * (sigma * sigma - to * to) / (sigma * sigma)));
    const float down = std::sqrt(std::max(0.0f, to * to - up * up));
    const float dt = down - sigma;
    for (std::size_t j = 0; j < lat; ++j) {
      const float e = e_none[j] + prm.text_guidance * (e_text[j] - e_image[j]) +
                      prm.image_guidance * (e_image[j] - e_none[j]);
      // derivative = (x - (x - sigma e)) / sigma = e
      latents[j] += e * dt + (up > 0.0f ? normal(rng) * up : 0.0f);
    }
  }
  if (progress && !progress(prm.steps, prm.steps)) return err(status::cancelled);

  // 4. Back to pixels.
  for (float& v : latents) v /= kVaeScale;
  const infer::session::named_input dec_in{p_->io.decode_in, {1, 4, h, w}, infer::session::element::f32, latents.data(),
                                           nullptr, nullptr, latents.size()};
  MV_TRY(auto dec, p_->dec->run_named(std::span(&dec_in, 1)));
  if (dec.empty() || dec[0].data.size() != 3 * px) return err(status::internal);
  rgb8 out;
  out.width = small.width;
  out.height = small.height;
  out.rgb.resize(3 * px);
  for (std::size_t i = 0; i < px; ++i) {
    for (int k = 0; k < 3; ++k) {
      const float v = (dec[0].data[static_cast<std::size_t>(k) * px + i] + 1.0f) * 127.5f;
      out.rgb[i * 3 + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 255.0f)));
    }
  }
  return out;
}

}  // namespace mv::enhance
