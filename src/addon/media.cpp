// SPDX-License-Identifier: GPL-2.0-or-later
// Host table v2 pixels (media.h) over the viewer's own decoders: the still
// first-pixel path (image/pipeline.h), the clip sampler (edit/clip_sample.h)
// and the JPEG-512 cache (image/thumb.h). Portable; both hosts install it.
#include "addon/media.h"

#include <algorithm>
#include <cstdio>

#include "edit/clip_sample.h"
#include "image/pipeline.h"
#include "image/thumb.h"
#include "io/file.h"
#include "io/file_port.h"
#include "io/paths.h"

namespace mv::addon::media {
namespace {

// Box filter from RGBA to RGB, fitting the long edge (never enlarging).
rgb_image fit_rgb(const std::uint8_t* rgba, std::uint32_t w, std::uint32_t h, std::uint32_t edge) {
  std::uint32_t dw = w, dh = h;
  const std::uint32_t long_edge = std::max(w, h);
  if (edge > 0 && long_edge > edge) {
    dw = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(
                                        static_cast<std::uint64_t>(w) * edge / long_edge));
    dh = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(
                                        static_cast<std::uint64_t>(h) * edge / long_edge));
  }
  rgb_image out;
  out.width = dw;
  out.height = dh;
  out.rgb.resize(static_cast<std::size_t>(dw) * dh * 3);
  for (std::uint32_t y = 0; y < dh; ++y) {
    const std::uint32_t y0 = static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * h / dh);
    std::uint32_t y1 = static_cast<std::uint32_t>((static_cast<std::uint64_t>(y + 1) * h + dh - 1) / dh);
    if (y1 <= y0) y1 = y0 + 1;
    for (std::uint32_t x = 0; x < dw; ++x) {
      const std::uint32_t x0 = static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * w / dw);
      std::uint32_t x1 = static_cast<std::uint32_t>((static_cast<std::uint64_t>(x + 1) * w + dw - 1) / dw);
      if (x1 <= x0) x1 = x0 + 1;
      std::uint32_t r = 0, g = 0, b = 0, n = 0;
      for (std::uint32_t sy = y0; sy < y1 && sy < h; ++sy) {
        const std::uint8_t* row = rgba + static_cast<std::size_t>(sy) * w * 4;
        for (std::uint32_t sx = x0; sx < x1 && sx < w; ++sx) {
          r += row[sx * 4];
          g += row[sx * 4 + 1];
          b += row[sx * 4 + 2];
          ++n;
        }
      }
      if (n == 0) n = 1;
      std::uint8_t* p = out.rgb.data() + (static_cast<std::size_t>(y) * dw + x) * 3;
      p[0] = static_cast<std::uint8_t>(r / n);
      p[1] = static_cast<std::uint8_t>(g / n);
      p[2] = static_cast<std::uint8_t>(b / n);
    }
  }
  return out;
}

image::thumb_store& thumbs() {
  static image::thumb_store store;
  return store;
}

result<image::thumb_key> moment_key(const std::string& path, std::int64_t pts_ms) {
  MV_TRY(io::file_stat st, io::stat_path(path));
  // A moment is its own cache row beside the file's poster: the same path
  // column with the moment appended, which no real file name can collide with
  // because the cache keys absolute paths ("#t=" never ends one).
  char suffix[40];
  std::snprintf(suffix, sizeof(suffix), "#t=%lld", static_cast<long long>(pts_ms));
  return image::thumb_key{path + suffix, st.mtime_unix, st.size};
}

}  // namespace

result<rgb_image> decode_still(const std::string& path, std::uint32_t max_long_edge) {
  MV_TRY(auto bytes, io::read_all(path));
  // First-pixel quality: a DCT-scaled JPEG or the embedded RAW preview. Other
  // families (PNG, HEIC, WebP, ...) have no cheaper path than a decode.
  auto img = image::decode_preview(bytes);
  if (!img) {
    if (img.error() == status::cancelled) return err(status::cancelled);
    img = image::decode_bytes(bytes, nullptr, 1);
  }
  if (!img) return err(img.error());
  if (img->width == 0 || img->height == 0) return err(status::corrupt);
  return fit_rgb(img->rgba.data(), img->width, img->height, max_long_edge);
}

result<std::unique_ptr<video_sampler>> open_sampler(const std::string& path,
                                                    const sampler_options& options) {
  class adapter final : public video_sampler {
   public:
    explicit adapter(std::unique_ptr<edit::clip::frame_sampler> s) : s_(std::move(s)) {
      const auto& f = s_->facts();
      facts_.duration_ms = f.duration_ms;
      facts_.width = f.width;
      facts_.height = f.height;
      facts_.hdr = f.hdr;
    }
    const video_facts& facts() const noexcept override { return facts_; }
    result<sampled_frame> next() override {
      MV_TRY(edit::clip::sampled s, s_->next());
      sampled_frame out;
      out.end = s.end;
      out.keyframe = s.keyframe;
      out.pts_ms = s.pts_ms;
      out.pts_tb = s.pts_tb;
      out.tb_num = s.tb_num;
      out.tb_den = s.tb_den;
      out.image.width = s.image.width;
      out.image.height = s.image.height;
      out.image.rgb = std::move(s.image.rgb);
      return out;
    }

   private:
    std::unique_ptr<edit::clip::frame_sampler> s_;
    video_facts facts_;
  };
  edit::clip::sample_options o;
  o.min_gap_ms = options.min_gap_ms;
  o.max_gap_ms = options.max_gap_ms;
  o.max_long_edge = options.max_long_edge;
  o.start_ms = options.start_ms;
  MV_TRY(auto s, edit::clip::frame_sampler::open(path, o));
  return std::unique_ptr<video_sampler>(new adapter(std::move(s)));
}

result<rgb_image> video_frame(const std::string& path, std::int64_t pts_ms,
                              std::uint32_t max_long_edge) {
  MV_TRY(auto f, edit::clip::frame_rgb_at(path, pts_ms, max_long_edge));
  rgb_image out;
  out.width = f.width;
  out.height = f.height;
  out.rgb = std::move(f.rgb);
  return out;
}

result<std::unique_ptr<audio_stream>> open_audio(const std::string& path, std::uint32_t sample_rate,
                                                 std::int64_t start_ms) {
  class adapter final : public audio_stream {
   public:
    explicit adapter(std::unique_ptr<edit::clip::audio_reader> r) : r_(std::move(r)) {}
    std::int64_t duration_ms() const noexcept override { return r_->facts().duration_ms; }
    result<std::vector<float>> read(std::size_t max_samples, std::int64_t& start_ms) override {
      return r_->read(max_samples, start_ms);
    }

   private:
    std::unique_ptr<edit::clip::audio_reader> r_;
  };
  MV_TRY(auto r, edit::clip::audio_reader::open(path, sample_rate, start_ms));
  return std::unique_ptr<audio_stream>(new adapter(std::move(r)));
}

result<std::string> moment_thumbnail(const std::string& path, std::int64_t pts_ms,
                                     const rgb_image* image) {
  auto& store = thumbs();
  if (!store.is_open()) {
    MV_TRY(std::string dir, io::thumb_cache_dir());
    MV_TRY_VOID(store.open(dir));
  }
  MV_TRY(image::thumb_key key, moment_key(path, pts_ms));
  if (!image) {
    MV_TRY(std::string hit, store.lookup(key));
    if (hit.empty()) return err(status::io);
    return hit;
  }
  if (image->width == 0 || image->height == 0 ||
      image->rgb.size() < static_cast<std::size_t>(image->width) * image->height * 3) {
    return err(status::invalid_arg);
  }
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(image->width) * image->height * 4);
  for (std::size_t i = 0, n = static_cast<std::size_t>(image->width) * image->height; i < n; ++i) {
    rgba[i * 4] = image->rgb[i * 3];
    rgba[i * 4 + 1] = image->rgb[i * 3 + 1];
    rgba[i * 4 + 2] = image->rgb[i * 3 + 2];
    rgba[i * 4 + 3] = 255;
  }
  std::uint32_t w = image->width, h = image->height;
  if (std::max(w, h) > image::kThumbLongEdge) {
    rgb_image small = fit_rgb(rgba.data(), w, h, image::kThumbLongEdge);
    w = small.width;
    h = small.height;
    rgba.assign(static_cast<std::size_t>(w) * h * 4, 255);
    for (std::size_t i = 0, n = static_cast<std::size_t>(w) * h; i < n; ++i) {
      rgba[i * 4] = small.rgb[i * 3];
      rgba[i * 4 + 1] = small.rgb[i * 3 + 1];
      rgba[i * 4 + 2] = small.rgb[i * 3 + 2];
    }
  }
  MV_TRY(auto jpeg, image::encode_thumb_rgba(rgba, w, h));
  return store.store(key, jpeg);
}

}  // namespace mv::addon::media
