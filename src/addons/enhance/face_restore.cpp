// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/face_restore.h"

#include <algorithm>
#include <cmath>
#include <thread>

#include "core/json.h"
#include "infer/ort.h"

namespace mv::enhance {
namespace {

// The photo at (x, y) in pixel-centre coordinates (pixel i spans i..i+1, its
// centre i + 0.5), bilinear, its edge pixel past the edge.
inline void sample(const rgb8& img, double x, double y, float out[3]) noexcept {
  const double fx = std::clamp(x - 0.5, 0.0, static_cast<double>(img.width - 1));
  const double fy = std::clamp(y - 0.5, 0.0, static_cast<double>(img.height - 1));
  const auto x0 = static_cast<std::uint32_t>(fx), y0 = static_cast<std::uint32_t>(fy);
  const std::uint32_t x1 = std::min(x0 + 1, img.width - 1), y1 = std::min(y0 + 1, img.height - 1);
  const auto ax = static_cast<float>(fx - x0), ay = static_cast<float>(fy - y0);
  const std::uint8_t* p00 = img.rgb.data() + (std::size_t{y0} * img.width + x0) * 3;
  const std::uint8_t* p10 = img.rgb.data() + (std::size_t{y0} * img.width + x1) * 3;
  const std::uint8_t* p01 = img.rgb.data() + (std::size_t{y1} * img.width + x0) * 3;
  const std::uint8_t* p11 = img.rgb.data() + (std::size_t{y1} * img.width + x1) * 3;
  for (int c = 0; c < 3; ++c) {
    const float top = p00[c] + (p10[c] - p00[c]) * ax;
    const float bot = p01[c] + (p11[c] - p01[c]) * ax;
    out[c] = top + (bot - top) * ay;
  }
}

// The mask (side x side) at (u, v), bilinear, 0 outside it.
inline float mask_at(const std::vector<float>& mask, std::uint32_t side, double u, double v) noexcept {
  const double fx = u - 0.5, fy = v - 0.5;
  if (fx < -1.0 || fy < -1.0 || fx > side || fy > side) return 0.0f;
  const auto x0 = static_cast<std::int64_t>(std::floor(fx)), y0 = static_cast<std::int64_t>(std::floor(fy));
  const auto ax = static_cast<float>(fx - x0), ay = static_cast<float>(fy - y0);
  const auto at = [&](std::int64_t x, std::int64_t y) -> float {
    if (x < 0 || y < 0 || x >= side || y >= side) return 0.0f;
    return mask[static_cast<std::size_t>(y) * side + static_cast<std::size_t>(x)];
  };
  const float top = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * ax;
  const float bot = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * ax;
  return top + (bot - top) * ay;
}

inline std::uint8_t to_byte(float v) noexcept {
  return static_cast<std::uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
}

// Samples per side where one output pixel covers `span` source pixels.
inline int grid_for(double span) noexcept {
  return static_cast<int>(std::clamp(std::ceil(span - 1e-6), 1.0, 8.0));
}

// The template at the model's side.
std::array<float, 10> template_at(std::uint32_t side) {
  std::array<float, 10> t{};
  const float k = static_cast<float>(side) / 512.0f;
  for (std::size_t i = 0; i < t.size(); ++i) t[i] = kFfhq512[i] * k;
  return t;
}

// The face of an answer nearest the aligned crop's face centre, when it is
// near enough to be the face the crop was made of.
const face_found* centred(const std::vector<face_found>& faces, std::uint32_t side) {
  const float cx = side * 0.5f, cy = side * 0.56f, reach = side * 0.18f;
  const face_found* best = nullptr;
  float best_d = reach;
  for (const face_found& f : faces) {
    const float d = std::hypot(f.x + f.w * 0.5f - cx, f.y + f.h * 0.5f - cy);
    if (d <= best_d) {
      best_d = d;
      best = &f;
    }
  }
  return best;
}

}  // namespace

double similarity::scale() const noexcept { return std::sqrt(a * a + b * b); }

similarity similarity::inverse() const noexcept {
  const double d = a * a + b * b;
  if (d <= 0.0) return {};
  similarity s;
  s.a = a / d;
  s.b = -b / d;
  s.tx = -(s.a * tx - s.b * ty);
  s.ty = -(s.b * tx + s.a * ty);
  return s;
}

similarity fit_similarity(std::span<const float, 10> from, std::span<const float, 10> to) noexcept {
  double sx = 0, sy = 0, dx = 0, dy = 0;
  for (int i = 0; i < 5; ++i) {
    sx += from[2 * i];
    sy += from[2 * i + 1];
    dx += to[2 * i];
    dy += to[2 * i + 1];
  }
  sx /= 5;
  sy /= 5;
  dx /= 5;
  dy /= 5;
  double den = 0, num_a = 0, num_b = 0;
  for (int i = 0; i < 5; ++i) {
    const double px = from[2 * i] - sx, py = from[2 * i + 1] - sy;
    const double qx = to[2 * i] - dx, qy = to[2 * i + 1] - dy;
    den += px * px + py * py;
    num_a += px * qx + py * qy;
    num_b += px * qy - py * qx;
  }
  if (!(den > 1e-9) || !std::isfinite(den)) return {};
  similarity s;
  s.a = num_a / den;
  s.b = num_b / den;
  s.tx = dx - (s.a * sx - s.b * sy);
  s.ty = dy - (s.b * sx + s.a * sy);
  return s;
}

rgb8 warp_rgb(const rgb8& photo, const similarity& to_out, std::uint32_t width, std::uint32_t height) {
  rgb8 out;
  out.width = width;
  out.height = height;
  out.rgb.assign(std::size_t{width} * height * 3, 0);
  if (photo.width == 0 || photo.height == 0 || photo.rgb.size() < std::size_t{photo.width} * photo.height * 3) {
    return out;
  }
  const similarity inv = to_out.inverse();
  const int n = grid_for(inv.scale());
  const float norm = 1.0f / static_cast<float>(n * n);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      float acc[3] = {0, 0, 0};
      for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
          const double ox = x + (i + 0.5) / n, oy = y + (j + 0.5) / n;
          double px = 0, py = 0;
          inv.apply(ox, oy, px, py);
          float v[3];
          sample(photo, px, py, v);
          for (int c = 0; c < 3; ++c) acc[c] += v[c];
        }
      }
      std::uint8_t* o = out.rgb.data() + (std::size_t{y} * width + x) * 3;
      for (int c = 0; c < 3; ++c) o[c] = to_byte(acc[c] * norm);
    }
  }
  return out;
}

std::vector<float> face_mask(std::uint32_t side) {
  std::vector<float> m(std::size_t{side} * side, 0.0f);
  const double s = side;
  for (std::uint32_t y = 0; y < side; ++y) {
    for (std::uint32_t x = 0; x < side; ++x) {
      const double u = ((x + 0.5) / s - 0.5) / 0.34, v = ((y + 0.5) / s - 0.56) / 0.42;
      const double r = std::sqrt(u * u + v * v);
      const double t = std::clamp((1.0 - r) / 0.25, 0.0, 1.0);
      m[std::size_t{y} * side + x] = static_cast<float>(t * t * (3.0 - 2.0 * t));
    }
  }
  // The crop's own edge is never pasted (a face against the photo's border
  // was filled there from its edge pixel).
  for (std::uint32_t i = 0; i < side; ++i) {
    m[i] = m[std::size_t{side - 1} * side + i] = 0.0f;
    m[std::size_t{i} * side] = m[std::size_t{i} * side + side - 1] = 0.0f;
  }
  return m;
}

rgb8 blend_face(const rgb8& aligned, const rgb8& restored, const std::vector<float>& mask, float strength) {
  rgb8 out = aligned;
  const std::size_t px = std::size_t{aligned.width} * aligned.height;
  if (restored.rgb.size() != aligned.rgb.size() || mask.size() != px) return out;
  const float k = std::clamp(strength, 0.0f, 1.0f);
  for (std::size_t i = 0; i < px; ++i) {
    const float w = mask[i] * k;
    if (w <= 0.0f) continue;
    for (int c = 0; c < 3; ++c) {
      const float a = aligned.rgb[i * 3 + c], r = restored.rgb[i * 3 + c];
      out.rgb[i * 3 + c] = to_byte(a + w * (r - a));
    }
  }
  return out;
}

pixel_box paste_face(rgb8& photo, const similarity& to_face, const rgb8& aligned, const rgb8& restored,
                     const std::vector<float>& mask, float strength) {
  pixel_box box{};
  const std::uint32_t side = aligned.width;
  const float k = std::clamp(strength, 0.0f, 1.0f);
  if (k <= 0.0f || side == 0 || aligned.height != side || restored.width != side || restored.height != side ||
      mask.size() != std::size_t{side} * side || photo.width == 0 || photo.height == 0) {
    return box;
  }
  // The photo pixels the crop covers: its corners back through the inverse.
  const similarity back = to_face.inverse();
  double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
  for (const auto& [u, v] : {std::pair{0.0, 0.0}, {double(side), 0.0}, {0.0, double(side)}, {double(side), double(side)}}) {
    double px = 0, py = 0;
    back.apply(u, v, px, py);
    x0 = std::min(x0, px);
    y0 = std::min(y0, py);
    x1 = std::max(x1, px);
    y1 = std::max(y1, py);
  }
  const auto bx0 = static_cast<std::uint32_t>(std::clamp(std::floor(x0), 0.0, double(photo.width)));
  const auto by0 = static_cast<std::uint32_t>(std::clamp(std::floor(y0), 0.0, double(photo.height)));
  const auto bx1 = static_cast<std::uint32_t>(std::clamp(std::ceil(x1), 0.0, double(photo.width)));
  const auto by1 = static_cast<std::uint32_t>(std::clamp(std::ceil(y1), 0.0, double(photo.height)));
  const double s = to_face.scale();
  const bool carry = s < 1.0;  // the face is larger in the photo than the crop
  const int n = carry ? 1 : grid_for(s);
  const float norm = 1.0f / static_cast<float>(n * n);
  std::uint32_t cx0 = photo.width, cy0 = photo.height, cx1 = 0, cy1 = 0;
  for (std::uint32_t y = by0; y < by1; ++y) {
    for (std::uint32_t x = bx0; x < bx1; ++x) {
      double u = 0, v = 0;
      to_face.apply(x + 0.5, y + 0.5, u, v);
      const float w = mask_at(mask, side, u, v) * k;
      if (w <= 0.0f) continue;
      std::uint8_t* p = photo.rgb.data() + (std::size_t{y} * photo.width + x) * 3;
      if (carry) {
        float r[3], a[3];
        sample(restored, u, v, r);
        sample(aligned, u, v, a);
        for (int c = 0; c < 3; ++c) p[c] = to_byte(p[c] + w * (r[c] - a[c]));
      } else {
        float acc[3] = {0, 0, 0};
        for (int j = 0; j < n; ++j) {
          for (int i = 0; i < n; ++i) {
            double su = 0, sv = 0;
            to_face.apply(x + (i + 0.5) / n, y + (j + 0.5) / n, su, sv);
            float r[3];
            sample(restored, su, sv, r);
            for (int c = 0; c < 3; ++c) acc[c] += r[c];
          }
        }
        for (int c = 0; c < 3; ++c) p[c] = to_byte(p[c] + w * (acc[c] * norm - p[c]));
      }
      cx0 = std::min(cx0, x);
      cy0 = std::min(cy0, y);
      cx1 = std::max(cx1, x + 1);
      cy1 = std::max(cy1, y + 1);
    }
  }
  if (cx1 > cx0 && cy1 > cy0) box = pixel_box{cx0, cy0, cx1 - cx0, cy1 - cy0};
  return box;
}

result<std::vector<face_found>> parse_faces(std::string_view text, std::uint32_t width, std::uint32_t height) {
  const auto doc = json::parse(text, 8);
  if (!doc || doc->k != json::kind::object) return err(status::corrupt);
  const json::value* faces = doc->find("faces");
  if (!faces || faces->k != json::kind::array) return err(status::corrupt);
  const auto num = [](const json::value* v, double fallback) {
    if (!v || v->k != json::kind::number) return fallback;
    const double d = v->is_integer ? static_cast<double>(v->i) : v->d;
    return std::isfinite(d) ? d : fallback;
  };
  const auto fw = static_cast<double>(width), fh = static_cast<double>(height);
  std::vector<face_found> out;
  for (const json::value& f : faces->a) {
    const json::value* lm = f.find("lm");
    if (f.k != json::kind::object || !lm || lm->k != json::kind::array || lm->a.size() != 10) continue;
    face_found ff;
    ff.x = static_cast<float>(num(f.find("x"), 0) * fw);
    ff.y = static_cast<float>(num(f.find("y"), 0) * fh);
    ff.w = static_cast<float>(num(f.find("w"), 0) * fw);
    ff.h = static_cast<float>(num(f.find("h"), 0) * fh);
    ff.score = static_cast<float>(num(f.find("score"), 0));
    ff.sim = static_cast<float>(num(f.find("sim"), -1));
    for (std::size_t i = 0; i < 10; ++i) ff.lm[i] = static_cast<float>(num(&lm->a[i], 0) * (i % 2 == 0 ? fw : fh));
    out.push_back(ff);
  }
  return out;
}

result<std::vector<face_found>> find_faces(const rgb8& photo, const faces_fn& faces) {
  if (!faces || photo.width == 0 || photo.height == 0) return err(status::invalid_arg);
  std::vector<face_found> all;
  // Asks the pack about the photo's (x, y, w, h) at most `edge` on its long
  // side, and keeps its faces in photo pixels. `inner`: drop a face cut by a
  // tile's edge inside the photo (another tile, or the whole, has it whole).
  const auto ask = [&](std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h, std::uint32_t edge,
                       bool inner) -> expected {
    const double f = std::min(1.0, static_cast<double>(edge) / std::max(w, h));
    const auto ow = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(w * f)));
    const auto oh = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(h * f)));
    const double fx = static_cast<double>(ow) / w, fy = static_cast<double>(oh) / h;
    similarity to;
    to.a = fx;  // uniform to a rounding
    to.tx = -static_cast<double>(x) * fx;
    to.ty = -static_cast<double>(y) * fx;
    const rgb8 view = (x == 0 && y == 0 && w == photo.width && h == photo.height && f >= 1.0)
                          ? photo
                          : warp_rgb(photo, to, ow, oh);
    MV_TRY(std::vector<face_found> got, faces(view, nullptr));
    for (face_found g : got) {
      if (inner) {
        const float m = 2.0f;
        if ((x > 0 && g.x < m) || (y > 0 && g.y < m) || (x + w < photo.width && g.x + g.w > ow - m) ||
            (y + h < photo.height && g.y + g.h > oh - m)) {
          continue;
        }
      }
      g.x = static_cast<float>(g.x / fx + x);
      g.y = static_cast<float>(g.y / fy + y);
      g.w = static_cast<float>(g.w / fx);
      g.h = static_cast<float>(g.h / fy);
      for (std::size_t i = 0; i < 10; ++i) {
        g.lm[i] = static_cast<float>(i % 2 == 0 ? g.lm[i] / fx + x : g.lm[i] / fy + y);
      }
      // The same face again: the larger box stands.
      bool merged = false;
      for (face_found& a : all) {
        const float d = std::hypot(a.x + a.w * 0.5f - (g.x + g.w * 0.5f), a.y + a.h * 0.5f - (g.y + g.h * 0.5f));
        if (d < 0.5f * std::min(std::min(a.w, a.h), std::min(g.w, g.h))) {
          if (g.w * g.h > a.w * a.h) a = g;
          merged = true;
          break;
        }
      }
      if (!merged) all.push_back(g);
    }
    return {};
  };
  MV_TRY_VOID(ask(0, 0, photo.width, photo.height, kFindEdge, false));
  const std::uint32_t short_side = std::min(photo.width, photo.height);
  if (short_side >= 2 * kFindTile) {
    const std::uint32_t t = short_side / 2, stride = t / 2;
    const auto starts = [&](std::uint32_t len) {
      std::vector<std::uint32_t> s;
      for (std::uint32_t p = 0; p + t < len; p += stride) s.push_back(p);
      s.push_back(len - t);
      return s;
    };
    for (const std::uint32_t ty : starts(photo.height)) {
      for (const std::uint32_t tx : starts(photo.width)) MV_TRY_VOID(ask(tx, ty, t, t, kFindTile, true));
    }
  }
  return all;
}

result<face_outcome> restore_faces(rgb8& photo, const std::vector<face_found>& found, face_restorer& model,
                                   const faces_fn& faces, const face_options& o) {
  face_outcome r;
  r.found = static_cast<int>(found.size());
  const std::uint32_t side = model.side();
  if (side < 64 || !faces) return err(status::invalid_arg);
  const std::array<float, 10> tpl = template_at(side);
  const std::vector<float> mask = face_mask(side);
  for (std::size_t i = 0; i < found.size(); ++i) {
    if (o.progress && !o.progress(i, found.size())) return err(status::cancelled);
    const face_found& f = found[i];
    if (std::hypot(f.lm[2] - f.lm[0], f.lm[3] - f.lm[1]) < kMinEyeDistance) {
      ++r.small;
      continue;
    }
    similarity to = fit_similarity(f.lm, tpl);
    rgb8 aligned = warp_rgb(photo, to, side, side);
    // Refined once: the pack's landmarks on the aligned crop are found at the
    // face's own size, finer than on the whole photo.
    if (auto again = faces(aligned, nullptr)) {
      if (const face_found* c = centred(*again, side)) {
        const similarity back = to.inverse();
        std::array<float, 10> lm{};
        for (int k = 0; k < 5; ++k) {
          double px = 0, py = 0;
          back.apply(c->lm[2 * k], c->lm[2 * k + 1], px, py);
          lm[2 * k] = static_cast<float>(px);
          lm[2 * k + 1] = static_cast<float>(py);
        }
        to = fit_similarity(lm, tpl);
        aligned = warp_rgb(photo, to, side, side);
      }
    } else if (again.error() == status::not_found || again.error() == status::permission_denied) {
      return err(again.error());
    }
    MV_TRY(rgb8 restored, model.restore(aligned));
    if (restored.width != side || restored.height != side) return err(status::internal);
    // The People guard: the face as it will be pasted, against itself.
    const rgb8 blended = blend_face(aligned, restored, mask, o.strength);
    MV_TRY(std::vector<face_found> judged, faces(aligned, &blended));
    const face_found* same = centred(judged, side);
    if (!same) {
      ++r.kept;
      continue;
    }
    r.sims.push_back(same->sim);
    if (same->sim < o.guard) {
      ++r.kept;
      continue;
    }
    const pixel_box changed = paste_face(photo, to, aligned, restored, mask, o.strength);
    if (changed.width > 0) r.changed.push_back(changed);
    ++r.restored;
  }
  if (o.progress && !o.progress(found.size(), found.size())) return err(status::cancelled);
  return r;
}

struct face_model::impl {
  std::unique_ptr<infer::session> s;
  face_io io;
  std::string out_name;  // the one output asked for (RestoreFormer++ has 14 more)
};

face_model::face_model(std::unique_ptr<impl> p) : p_(std::move(p)) {}
face_model::~face_model() = default;
std::uint32_t face_model::side() const noexcept { return p_->io.side; }

result<std::unique_ptr<face_model>> face_model::open(infer::runtime& rt, const std::string& file, const face_io& io,
                                                     int threads) {
  auto p = std::make_unique<impl>();
  p->io = io;
  infer::session_options so;
  so.on = infer::backend::cpu;
  so.threads = threads > 0 ? threads
                           : static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 2u, 9u) - 1);
  MV_TRY(p->s, infer::session::open(rt, file, so));
  const auto& names = p->s->output_names();
  if (names.empty()) return err(status::unsupported_format);
  p->out_name = io.output.empty() ? names.front() : io.output;
  if (std::find(names.begin(), names.end(), p->out_name) == names.end()) return err(status::unsupported_format);
  return std::unique_ptr<face_model>(new face_model(std::move(p)));
}

result<rgb8> face_model::restore(const rgb8& face) {
  const std::uint32_t n = p_->io.side;
  if (face.width != n || face.height != n || face.rgb.size() != std::size_t{n} * n * 3) return err(status::invalid_arg);
  const std::size_t px = std::size_t{n} * n;
  std::vector<float> in(3 * px);
  for (std::size_t i = 0; i < px; ++i) {
    for (std::size_t c = 0; c < 3; ++c) in[c * px + i] = face.rgb[i * 3 + c] / 127.5f - 1.0f;
  }
  const std::int64_t side = n;
  const infer::session::named_input input[1] = {
      {p_->io.input, {1, 3, side, side}, infer::session::element::f32, in.data(), nullptr, nullptr, in.size()}};
  const std::string wanted[1] = {p_->out_name};
  MV_TRY(auto outs, p_->s->run_named(std::span(input, 1), std::span(wanted, 1)));
  if (outs.size() != 1 || outs.front().data.size() != 3 * px) return err(status::internal);
  const std::vector<float>& o = outs.front().data;
  rgb8 r;
  r.width = n;
  r.height = n;
  r.rgb.resize(3 * px);
  for (std::size_t i = 0; i < px; ++i) {
    for (std::size_t c = 0; c < 3; ++c) {
      r.rgb[i * 3 + c] = to_byte((std::clamp(o[c * px + i], -1.0f, 1.0f) + 1.0f) * 127.5f);
    }
  }
  return r;
}

}  // namespace mv::enhance
