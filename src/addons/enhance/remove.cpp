// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/remove.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <thread>

#include "infer/ort.h"

namespace mv::enhance {

namespace {

// Along one axis of a w x h plane: each value the max (or the sum) of its
// window of 2r + 1, clipped at the edges.
template <typename T, typename Op>
std::vector<T> pass(const std::vector<T>& in, std::uint32_t w, std::uint32_t h, std::uint32_t r, bool across,
                    Op op) {
  std::vector<T> out(in.size());
  const std::uint32_t n = across ? w : h, lines = across ? h : w;
  for (std::uint32_t l = 0; l < lines; ++l) {
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::uint32_t lo = i >= r ? i - r : 0, hi = std::min(n - 1, i + r);
      T acc{};
      for (std::uint32_t j = lo; j <= hi; ++j) {
        const std::size_t at = across ? std::size_t{l} * w + j : std::size_t{j} * w + l;
        acc = op(acc, in[at]);
      }
      out[across ? std::size_t{l} * w + i : std::size_t{i} * w + l] = acc;
    }
  }
  return out;
}

}  // namespace

std::optional<pixel_box> remove_box(const mask8& mask) {
  std::uint32_t x0 = mask.width, y0 = mask.height, x1 = 0, y1 = 0;
  for (std::uint32_t y = 0; y < mask.height; ++y) {
    for (std::uint32_t x = 0; x < mask.width; ++x) {
      if (mask.a[std::size_t{y} * mask.width + x] > 127) {
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
      }
    }
  }
  if (x0 > x1 || y0 > y1) return std::nullopt;
  const std::uint32_t margin = std::max((std::max(x1 - x0, y1 - y0) + 1) / 2, kRemoveMinContext);
  // One axis: [lo, hi) grown by `margin`, then to `side` where the photo
  // allows, shifted back inside it at an edge.
  const auto grow = [](std::uint32_t lo, std::uint32_t hi, std::uint32_t margin, std::uint32_t limit,
                       std::uint32_t side) {
    std::int64_t a = static_cast<std::int64_t>(lo) - margin, b = static_cast<std::int64_t>(hi) + 1 + margin;
    if (side > b - a) {
      const std::int64_t extra = side - (b - a);
      a -= extra / 2;
      b += extra - extra / 2;
    }
    if (a < 0) b -= a, a = 0;
    if (b > limit) a -= b - limit, b = limit;
    a = std::max<std::int64_t>(a, 0);
    return std::pair<std::uint32_t, std::uint32_t>(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b));
  };
  const std::uint32_t side = std::max(x1 - x0, y1 - y0) + 1 + 2 * margin;
  const auto [ax, bx] = grow(x0, x1, margin, mask.width, side);
  const auto [ay, by] = grow(y0, y1, margin, mask.height, side);
  return pixel_box{ax, ay, bx - ax, by - ay};
}

rgb8 crop_rgb(const rgb8& src, const pixel_box& box) {
  rgb8 out;
  out.width = box.width;
  out.height = box.height;
  out.rgb.resize(std::size_t{box.width} * box.height * 3);
  for (std::uint32_t y = 0; y < box.height; ++y) {
    const std::uint8_t* row = src.rgb.data() + (std::size_t{box.y + y} * src.width + box.x) * 3;
    std::copy(row, row + std::size_t{box.width} * 3, out.rgb.data() + std::size_t{y} * box.width * 3);
  }
  return out;
}

mask8 crop_mask(const mask8& src, const pixel_box& box, std::uint32_t width, std::uint32_t height) {
  mask8 out;
  out.width = width;
  out.height = height;
  out.a.resize(std::size_t{width} * height);
  for (std::uint32_t y = 0; y < height; ++y) {
    const std::uint32_t sy = box.y + std::min(box.height - 1, static_cast<std::uint32_t>((y + 0.5) * box.height / height));
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::uint32_t sx = box.x + std::min(box.width - 1, static_cast<std::uint32_t>((x + 0.5) * box.width / width));
      out.a[std::size_t{y} * width + x] = src.a[std::size_t{sy} * src.width + sx] > 127 ? 255 : 0;
    }
  }
  return out;
}

void paste_back(rgb8& photo, const pixel_box& box, const rgb8& filled, const mask8& mask, std::uint32_t feather) {
  const std::uint32_t w = box.width, h = box.height;
  std::vector<std::uint32_t> m(std::size_t{w} * h);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      m[std::size_t{y} * w + x] = mask.a[std::size_t{box.y + y} * mask.width + box.x + x] > 127 ? 1 : 0;
    }
  }
  // Grown by `feather`, then averaged over the same radius: 1 on every
  // brushed pixel, falling to 0 at 2 * feather outside.
  const auto max_op = [](std::uint32_t a, std::uint32_t b) { return std::max(a, b); };
  const auto sum_op = [](std::uint32_t a, std::uint32_t b) { return a + b; };
  m = pass(pass(m, w, h, feather, true, max_op), w, h, feather, false, max_op);
  const std::vector<std::uint32_t> sum = pass(pass(m, w, h, feather, true, sum_op), w, h, feather, false, sum_op);
  for (std::uint32_t y = 0; y < h; ++y) {
    const std::uint32_t cy = std::min(h - 1, y + feather) - (y >= feather ? y - feather : 0) + 1;
    for (std::uint32_t x = 0; x < w; ++x) {
      const std::uint32_t cx = std::min(w - 1, x + feather) - (x >= feather ? x - feather : 0) + 1;
      const std::uint32_t s = sum[std::size_t{y} * w + x], count = cx * cy;
      if (s == 0) continue;  // the photo's own pixel, untouched
      std::uint8_t* p = photo.rgb.data() + (std::size_t{box.y + y} * photo.width + box.x + x) * 3;
      const std::uint8_t* f = filled.rgb.data() + (std::size_t{y} * w + x) * 3;
      for (int c = 0; c < 3; ++c) {
        p[c] = s == count ? f[c]
                          : static_cast<std::uint8_t>((p[c] * (count - s) + f[c] * s + count / 2) / count);
      }
    }
  }
}

mask8 mask_from_dabs(const std::vector<float>& dabs, std::uint32_t width, std::uint32_t height) {
  mask8 out;
  out.width = width;
  out.height = height;
  out.a.assign(std::size_t{width} * height, 0);
  if (width == 0 || height == 0) return out;
  const float l = static_cast<float>(std::max(width, height));
  for (std::size_t i = 0; i + 2 < dabs.size(); i += 3) {
    const float cx = dabs[i] * static_cast<float>(width), cy = dabs[i + 1] * static_cast<float>(height);
    const float r = dabs[i + 2] * l;
    if (!(r > 0.0f) || !std::isfinite(cx) || !std::isfinite(cy)) continue;
    const auto lo = [](float v) { return static_cast<std::int64_t>(std::floor(v)); };
    const std::int64_t x0 = std::max<std::int64_t>(0, lo(cx - r));
    const std::int64_t x1 = std::min<std::int64_t>(std::int64_t{width} - 1, lo(cx + r));
    const std::int64_t y0 = std::max<std::int64_t>(0, lo(cy - r));
    const std::int64_t y1 = std::min<std::int64_t>(std::int64_t{height} - 1, lo(cy + r));
    for (std::int64_t y = y0; y <= y1; ++y) {
      for (std::int64_t x = x0; x <= x1; ++x) {
        const float px = static_cast<float>(x) + 0.5f - cx, py = static_cast<float>(y) + 0.5f - cy;
        if (px * px + py * py <= r * r) out.a[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)] = 255;
      }
    }
  }
  return out;
}

extend_margins extend_preset(std::string_view preset, std::uint32_t w, std::uint32_t h) noexcept {
  extend_margins m;
  if (w == 0 || h == 0) return m;
  const auto to_aspect = [&](double aspect) {  // aspect = width / height, grown on one axis only
    const double now = static_cast<double>(w) / h;
    if (aspect > now) {
      const auto extra = static_cast<std::uint32_t>(h * aspect - w);
      m.left = extra / 2;
      m.right = extra - m.left;
    } else if (aspect < now) {
      const auto extra = static_cast<std::uint32_t>(w / aspect - h);
      m.top = extra / 2;
      m.bottom = extra - m.top;
    }
  };
  if (preset == "wider") {
    m.left = m.right = w / 8;
  } else if (preset == "taller") {
    m.top = m.bottom = h / 8;
  } else if (preset == "all") {
    m.left = m.right = w / 10;
    m.top = m.bottom = h / 10;
  } else if (preset == "square") {
    to_aspect(1.0);
  } else if (preset == "16x9") {
    to_aspect(16.0 / 9.0);
  } else if (preset == "4x5") {
    to_aspect(4.0 / 5.0);
  }
  // At most twice the size each way, and 16384 a side.
  const auto cap = [](std::uint32_t& a, std::uint32_t& b, std::uint32_t side) {
    const std::uint32_t most = std::min(side, 16384u > side ? 16384u - side : 0u);
    if (a + b > most) {
      const double k = static_cast<double>(most) / (a + b);
      a = static_cast<std::uint32_t>(a * k);
      b = static_cast<std::uint32_t>(b * k);
    }
  };
  cap(m.left, m.right, w);
  cap(m.top, m.bottom, h);
  return m;
}

void extend_canvas(const rgb8& photo, const extend_margins& m, rgb8& out, mask8& mask) {
  out.width = photo.width + m.left + m.right;
  out.height = photo.height + m.top + m.bottom;
  out.rgb.assign(std::size_t{out.width} * out.height * 3, 0);
  mask.width = out.width;
  mask.height = out.height;
  mask.a.assign(std::size_t{out.width} * out.height, 255);
  // The margins start as the photo mirrored at its edge (texture for the
  // model to continue; a stretched edge pixel gave streaks, 2026-11-04).
  const auto mirror = [](std::int64_t v, std::uint32_t n) {
    const std::int64_t period = 2 * static_cast<std::int64_t>(n);
    std::int64_t r = ((v % period) + period) % period;
    if (r >= n) r = period - 1 - r;
    return static_cast<std::uint32_t>(r);
  };
  for (std::uint32_t y = 0; y < out.height; ++y) {
    const std::uint32_t sy = mirror(static_cast<std::int64_t>(y) - m.top, photo.height);
    for (std::uint32_t x = 0; x < out.width; ++x) {
      const std::uint32_t sx = mirror(static_cast<std::int64_t>(x) - m.left, photo.width);
      const std::uint8_t* s = photo.rgb.data() + (std::size_t{sy} * photo.width + sx) * 3;
      std::uint8_t* d = out.rgb.data() + (std::size_t{y} * out.width + x) * 3;
      d[0] = s[0], d[1] = s[1], d[2] = s[2];
      const bool inside = x >= m.left && x < m.left + photo.width && y >= m.top && y < m.top + photo.height;
      if (inside) mask.a[std::size_t{y} * out.width + x] = 0;
    }
  }
}

std::vector<pixel_box> extend_tiles(std::uint32_t w, std::uint32_t h, const extend_margins& m) {
  std::vector<pixel_box> out;
  const std::uint32_t shorter = std::min(w, h);
  // A tile reaches as far into the photo as the margin is deep (context), at
  // least 256 and at most the picture's shorter side.
  const auto side_for = [&](std::uint32_t margin) { return std::clamp(std::max(margin * 3, 512u), 256u, shorter); };
  const auto along = [&](std::uint32_t length, std::uint32_t tile, auto&& place) {
    const std::uint32_t step = std::max(1u, tile / 2);  // half overlapping: every pixel in two tiles
    for (std::uint32_t at = 0;; at += step) {
      const std::uint32_t start = std::min(at, length > tile ? length - tile : 0u);
      place(start);
      if (start + tile >= length) break;
    }
  };
  if (m.left) {
    const std::uint32_t t = side_for(m.left);
    along(h, t, [&](std::uint32_t y) { out.push_back({0, y, t, std::min(t, h - y)}); });
  }
  if (m.right) {
    const std::uint32_t t = side_for(m.right);
    along(h, t, [&](std::uint32_t y) { out.push_back({w - t, y, t, std::min(t, h - y)}); });
  }
  if (m.top) {
    const std::uint32_t t = side_for(m.top);
    along(w, t, [&](std::uint32_t x) { out.push_back({x, 0, std::min(t, w - x), t}); });
  }
  if (m.bottom) {
    const std::uint32_t t = side_for(m.bottom);
    along(w, t, [&](std::uint32_t x) { out.push_back({x, h - t, std::min(t, w - x), t}); });
  }
  return out;
}

expected extend_fill(rgb8& canvas, const mask8& mask, const std::vector<pixel_box>& tiles, inpaint_filler& model,
                     const std::function<bool(std::size_t, std::size_t)>& progress) {
  const std::uint32_t w = canvas.width, h = canvas.height, side = model.side();
  std::vector<float> sum(std::size_t{w} * h * 3, 0.0f), weight(std::size_t{w} * h, 0.0f);
  for (std::size_t i = 0; i < tiles.size(); ++i) {
    if (progress && !progress(i, tiles.size())) return err(status::cancelled);
    const pixel_box& b = tiles[i];
    MV_TRY(rgb8 filled, model.fill(resize_rgb(crop_rgb(canvas, b), side, side), crop_mask(mask, b, side, side)));
    const rgb8 back = resize_rgb(filled, b.width, b.height);
    for (std::uint32_t y = 0; y < b.height; ++y) {
      // A tent across the tile, never 0 inside it (a tile at the picture's
      // edge is the only one there).
      const float wy = (std::min(y, b.height - 1 - y) + 1.0f) / (b.height * 0.5f + 1.0f);
      for (std::uint32_t x = 0; x < b.width; ++x) {
        const std::size_t at = std::size_t{b.y + y} * w + b.x + x;
        if (mask.a[at] <= 127) continue;
        const float wt = wy * (std::min(x, b.width - 1 - x) + 1.0f) / (b.width * 0.5f + 1.0f);
        const std::uint8_t* f = back.rgb.data() + (std::size_t{y} * b.width + x) * 3;
        for (int c = 0; c < 3; ++c) sum[at * 3 + c] += wt * f[c];
        weight[at] += wt;
      }
    }
  }
  for (std::size_t at = 0; at < weight.size(); ++at) {
    if (mask.a[at] <= 127 || weight[at] <= 0.0f) continue;
    for (int c = 0; c < 3; ++c) {
      canvas.rgb[at * 3 + c] = static_cast<std::uint8_t>(std::clamp(sum[at * 3 + c] / weight[at] + 0.5f, 0.0f, 255.0f));
    }
  }
  if (progress) (void)progress(tiles.size(), tiles.size());
  return {};
}

struct inpaint_model::impl {
  std::unique_ptr<infer::session> s;
  inpaint_io io;
};

inpaint_model::inpaint_model(std::unique_ptr<impl> p) : p_(std::move(p)) {}
inpaint_model::~inpaint_model() = default;
std::uint32_t inpaint_model::side() const noexcept { return p_->io.side; }

result<std::unique_ptr<inpaint_model>> inpaint_model::open(infer::runtime& rt, const std::string& file,
                                                           const inpaint_io& io) {
  auto p = std::make_unique<impl>();
  p->io = io;
  // CPU: the model is small (about a second a fill at 512 px), and its FFT
  // layers are not ones Core ML is known to take.
  infer::session_options o;
  o.on = infer::backend::cpu;
  o.threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 2u, 9u) - 1);
  MV_TRY(p->s, infer::session::open(rt, file, o));
  return std::unique_ptr<inpaint_model>(new inpaint_model(std::move(p)));
}

result<rgb8> inpaint_model::fill(const rgb8& image, const mask8& mask) {
  const std::uint32_t n = p_->io.side;
  if (image.width != n || image.height != n || mask.width != n || mask.height != n) return err(status::invalid_arg);
  const std::size_t px = std::size_t{n} * n;
  std::vector<float> pic(3 * px), m(px);
  for (std::size_t i = 0; i < px; ++i) {
    for (std::size_t k = 0; k < 3; ++k) pic[k * px + i] = image.rgb[i * 3 + k] / 255.0f;
    m[i] = mask.a[i] > 127 ? 1.0f : 0.0f;
  }
  const std::int64_t side = n;
  const infer::session::named_input in[2] = {
      {p_->io.image, {1, 3, side, side}, infer::session::element::f32, pic.data(), nullptr, nullptr, pic.size()},
      {p_->io.mask, {1, 1, side, side}, infer::session::element::f32, m.data(), nullptr, nullptr, m.size()},
  };
  MV_TRY(auto out, p_->s->run_named(std::span(in, 2)));
  if (out.empty() || out[0].data.size() != 3 * px) return err(status::internal);
  rgb8 r;
  r.width = n;
  r.height = n;
  r.rgb.resize(3 * px);
  const float scale = p_->io.out_scale;
  for (std::size_t i = 0; i < px; ++i) {
    for (std::size_t k = 0; k < 3; ++k) {
      const float v = out[0].data[k * px + i] * scale;
      r.rgb[i * 3 + k] = static_cast<std::uint8_t>(std::clamp(std::lround(v), 0l, 255l));
    }
  }
  return r;
}

}  // namespace mv::enhance
