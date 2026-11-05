// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Host table v2 pixels and sound (addon_media.h) over the viewer's own
// decoders: the still first-pixel path (image/pipeline.h), the clip sampler
// (edit/clip_sample.h) and the JPEG-512 cache (image/thumb.h). Portable; both
// hosts install it.
#include "abi/addon_media.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <mutex>

#include "codec/decode.h"
#include "core/json.h"
#include "edit/bake.h"
#include "edit/clip_sample.h"
#include "edit/export.h"
#include "edit/eyes.h"
#include "edit/picture_io.h"
#include "edit/sound_writer.h"
#include "image/half.h"
#include "image/linear.h"
#include "image/pipeline.h"
#include "image/thumb.h"
#include "io/file.h"
#include "io/file_port.h"
#include "io/pairing.h"
#include "io/paths.h"
#include "io/collision_name.h"
#include "io/replace.h"
#include "meta/location.h"
#include "meta/meta.h"

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

// Opened on first use by whichever pack worker gets here first; several call
// moment_thumbnail at once. The check-then-open is one step under a lock, so
// no worker can see it half-done; lookups and stores are the store's own.
result<image::thumb_store*> thumbs() {
  static image::thumb_store store;
  static std::mutex open_mutex;
  std::lock_guard<std::mutex> lock(open_mutex);
  if (!store.is_open()) {
    MV_TRY(std::string dir, io::thumb_cache_dir());
    MV_TRY_VOID(store.open(dir));
  }
  return &store;
}

result<image::thumb_key> moment_key(const std::string& path, std::int64_t pts_ms) {
  MV_TRY(io::file_stat st, io::stat_path(path));
  // A moment is its own cache row beside the file's poster (image/thumb.h).
  return image::moment_thumb_key(path, pts_ms, st.mtime_unix, st.size);
}

// A still's own row (the viewer's key) or a moment's, under the file's stamp now.
result<image::thumb_key> cache_key(const std::string& path, std::int64_t pts_ms) {
  if (pts_ms >= 0) return moment_key(path, pts_ms);
  MV_TRY(io::file_stat st, io::stat_path(path));
  return image::thumb_key{path, st.mtime_unix, st.size};
}

// A JPEG-512 row is ~20-80 KB; anything far past that is not one.
constexpr std::size_t kMaxThumbBytes = 2u << 20;

// Straight-alpha RGBA from packed RGB, what the JPEG-512 encoder takes.
std::vector<std::uint8_t> rgba_of(const std::uint8_t* rgb, std::uint32_t w, std::uint32_t h) {
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4);
  for (std::size_t i = 0, n = static_cast<std::size_t>(w) * h; i < n; ++i) {
    rgba[i * 4] = rgb[i * 3];
    rgba[i * 4 + 1] = rgb[i * 3 + 1];
    rgba[i * 4 + 2] = rgb[i * 3 + 2];
    rgba[i * 4 + 3] = 255;
  }
  return rgba;
}

// #354: a document's page, drawn by the viewer's own renderer (the OS's PDF
// renderer, codec/'s DOCX layout) at `max_long_edge`: the page a search
// result opens on, as its tile. `page` is 0-based; past the end is invalid_arg.
result<rgb_image> document_page(const std::string& path, std::int64_t page, std::uint32_t max_long_edge) {
  if (page < 0 || page > static_cast<std::int64_t>(UINT32_MAX)) return err(status::invalid_arg);
  MV_TRY(auto bytes, io::read_all(path));
  const std::uint32_t edge = std::clamp<std::uint32_t>(max_long_edge == 0 ? 512 : max_long_edge, 16, 2048);
  const auto family = codec::probe(bytes);
  result<codec::raster> r = err(status::unsupported_format);
  if (family == codec::format_family::pdf) {
    r = codec::decode_pdf(bytes, static_cast<std::uint32_t>(page), nullptr, edge);
  } else if (family == codec::format_family::docx) {
    r = codec::decode_docx(bytes, static_cast<std::uint32_t>(page), nullptr, edge);
  }
  if (!r) return err(r.error());
  if (r->width == 0 || r->height == 0 ||
      r->rgba.size() < static_cast<std::size_t>(r->width) * r->height * 4) {
    return err(status::corrupt);
  }
  return fit_rgb(r->rgba.data(), r->width, r->height, max_long_edge);
}

}  // namespace

result<rgb_image> decode_still(const std::string& path, std::uint32_t max_long_edge) {
  // #355: an audio file's picture is its cover art, read from the header
  // alone (never the whole file); none is status::not_found.
  if (io::is_audio_name(path)) {
    MV_TRY(edit::clip::sample_rgb art, edit::clip::cover_art_rgb(path, max_long_edge));
    rgb_image out;
    out.width = art.width;
    out.height = art.height;
    out.rgb = std::move(art.rgb);
    return out;
  }
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
  // #354: a document's "moment" is a page (pts_ms is its index).
  if (io::is_document_name(path)) return document_page(path, pts_ms, max_long_edge);
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

result<std::unique_ptr<sound_stream>> open_sound(const std::string& path, std::uint32_t sample_rate,
                                                 std::uint32_t channels) {
  class adapter final : public sound_stream {
   public:
    adapter(std::unique_ptr<edit::clip::audio_reader> r, std::uint32_t rate) : r_(std::move(r)) {
      const auto& a = r_->facts();
      f_.channels = a.channels;
      f_.sample_rate = rate;
      f_.source_rate = a.source_rate;
      f_.source_channels = a.source_channels;
      f_.has_picture = a.has_picture;
      f_.duration_ms = a.duration_ms;
    }
    const sound_facts& facts() const noexcept override { return f_; }
    result<std::vector<float>> read(std::size_t max_frames) override {
      std::int64_t start_ms = 0;
      auto r = r_->read(max_frames * f_.channels, start_ms);
      if (r && !read_) {
        read_ = true;
        first_ms_ = start_ms;
      }
      return r;
    }
    std::int64_t first_ms() const noexcept override { return first_ms_; }

   private:
    std::unique_ptr<edit::clip::audio_reader> r_;
    sound_facts f_;
    bool read_ = false;
    std::int64_t first_ms_ = 0;
  };
  MV_TRY(auto r, edit::clip::audio_reader::open(path, sample_rate, 0, nullptr, channels));
  return std::unique_ptr<sound_stream>(new adapter(std::move(r), sample_rate));
}

result<std::unique_ptr<sound_sink>> open_sound_writer(const std::string& source, std::uint32_t sample_rate,
                                                      std::uint32_t channels, const std::string& suffix) {
  class adapter final : public sound_sink {
   public:
    explicit adapter(std::unique_ptr<edit::clip::sound_writer> w) : w_(std::move(w)) {}
    bool with_picture() const noexcept override { return w_->with_picture(); }
    expected write(std::span<const float> pcm) override { return w_->write(pcm); }
    result<std::string> finish() override { return w_->finish(); }

   private:
    std::unique_ptr<edit::clip::sound_writer> w_;
  };
  edit::clip::sound_out_options o;
  o.sample_rate = sample_rate;
  o.channels = channels;
  o.suffix = suffix;
  MV_TRY(auto w, edit::clip::sound_writer::open(source, o));
  return std::unique_ptr<sound_sink>(new adapter(std::move(w)));
}

result<std::unique_ptr<picture_source>> open_frames(const std::string& path) {
  class adapter final : public picture_source {
   public:
    explicit adapter(std::unique_ptr<edit::clip::frame_reader> r) : r_(std::move(r)) {
      const auto& a = r_->facts();
      f_.width = a.width;
      f_.height = a.height;
      f_.rotation = a.rotation;
      f_.hdr = a.hdr;
      f_.fps = a.fps;
      f_.duration_ms = a.duration_ms;
      f_.frame_count = a.frame_count;
    }
    const picture_facts& facts() const noexcept override { return f_; }
    result<bool> next(std::vector<std::uint8_t>& rgb, std::int64_t& pts_us) override { return r_->next(rgb, pts_us); }

   private:
    std::unique_ptr<edit::clip::frame_reader> r_;
    picture_facts f_;
  };
  MV_TRY(auto r, edit::clip::frame_reader::open(path));
  return std::unique_ptr<picture_source>(new adapter(std::move(r)));
}

result<std::unique_ptr<picture_sink>> open_clip_writer(const std::string& source, const picture_out& out) {
  class adapter final : public picture_sink {
   public:
    explicit adapter(std::unique_ptr<edit::clip::picture_writer> w) : w_(std::move(w)) {}
    expected write_frame(const std::uint8_t* rgb, std::uint32_t stride, std::int64_t pts_us) override {
      return w_->write_frame(rgb, stride, pts_us);
    }
    expected write_sound(std::span<const float> pcm) override { return w_->write_sound(pcm); }
    result<std::string> finish() override { return w_->finish(); }

   private:
    std::unique_ptr<edit::clip::picture_writer> w_;
  };
  edit::clip::picture_out_options o;
  o.width = out.width;
  o.height = out.height;
  o.sound_rate = out.sound_rate;
  o.sound_channels = out.sound_channels;
  o.suffix = out.suffix;
  MV_TRY(auto w, edit::clip::picture_writer::open(source, o));
  return std::unique_ptr<picture_sink>(new adapter(std::move(w)));
}

namespace {

struct still_edit {
  std::mutex m;
  std::string path;
  edit::geometry g;
  edit::colour c;
  std::string mask_path;
  std::vector<float> mask;
  std::string extend_path;  // edit-refresh S6
  std::array<float, 4> extend{};
  // The geometry of the edit the frame was measured against (identity when
  // the photo had none): an undo of Fill's crop, or any later turn or crop,
  // leaves fractions of another picture, so the frame stops answering.
  edit::geometry extend_g;
};

still_edit& edit_slot() {
  static still_edit s;
  return s;
}

// The edit's geometry still_open applies to `path` now (s.m held).
edit::geometry geometry_now(const still_edit& s, const std::string& path) {
  return !s.path.empty() && s.path == path ? s.g : edit::geometry{};
}

bool same_geometry(const edit::geometry& a, const edit::geometry& b) {
  return a.orient == b.orient && a.straighten == b.straighten && a.crop == b.crop && a.resize == b.resize;
}

}  // namespace

namespace {
struct photos_hooks {
  std::mutex m;
  photos_resolve_fn resolve;
  photos_add_fn add;
};
photos_hooks& hooks() {
  static photos_hooks h;
  return h;
}
}  // namespace

void set_photos_hooks(photos_resolve_fn resolve, photos_add_fn add) {
  photos_hooks& h = hooks();
  const std::lock_guard<std::mutex> lock(h.m);
  h.resolve = std::move(resolve);
  h.add = std::move(add);
}

bool is_photos_key(std::string_view path) noexcept { return path.size() > 7 && path.substr(0, 7) == "photos:"; }

result<std::string> readable_path(const std::string& path, const job_context* ctx) {
  if (!is_photos_key(path)) return path;
  photos_resolve_fn resolve;
  {
    photos_hooks& h = hooks();
    const std::lock_guard<std::mutex> lock(h.m);
    resolve = h.resolve;
  }
  if (!resolve) return err(status::not_found);
  return resolve(path, ctx);
}

result<std::string> staging_dir() {
  std::error_code ec;
  const std::filesystem::path staging = std::filesystem::temp_directory_path(ec) / "MediaViewer Saves";
  if (ec) return err(status::io);
  std::filesystem::create_directories(staging, ec);
  if (ec) return err(status::io);
  const std::u8string u = staging.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size()) + "/";
}

result<std::string> add_to_library(const std::string& file, const std::string& source) {
  photos_add_fn add;
  {
    photos_hooks& h = hooks();
    const std::lock_guard<std::mutex> lock(h.m);
    add = h.add;
  }
  result<std::string> key = add ? add(file, source) : result<std::string>(err(status::unsupported_format));
  // The library has its own copy, or none: ours never stays behind.
  (void)io::remove_file(file);
  return key;
}

result<std::string> save_copy_to_library(const std::string& source, const library_exporter& exporter,
                                         const job_context* ctx) {
  if (!is_photos_key(source) || !exporter) return err(status::invalid_arg);
  MV_TRY(const std::string file, readable_path(source, ctx));
  MV_TRY(const std::string dir, staging_dir());
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  MV_TRY(const std::string written, exporter(file, dir));
  return add_to_library(written, source);
}

std::string result_name(const std::string& written) {
  if (is_photos_key(written)) return "to your Photos library";
  const std::size_t slash = written.find_last_of("/\\");
  return slash == std::string::npos ? written : written.substr(slash + 1);
}

void set_still_edit(const std::string& path, const edit::geometry& g, const edit::colour& c) {
  still_edit& s = edit_slot();
  const std::lock_guard<std::mutex> lock(s.m);
  s.path = path;
  s.g = g;
  s.c = c;
}

void clear_still_edit() {
  still_edit& s = edit_slot();
  const std::lock_guard<std::mutex> lock(s.m);
  s.path.clear();
}

void set_still_mask(const std::string& path, std::vector<float> dabs) {
  still_edit& s = edit_slot();
  const std::lock_guard<std::mutex> lock(s.m);
  s.mask_path = path;
  s.mask = std::move(dabs);
}

std::vector<float> still_mask(const std::string& path) {
  still_edit& s = edit_slot();
  const std::lock_guard<std::mutex> lock(s.m);
  if (path.empty() || s.mask_path != path) return {};
  return s.mask;
}

void set_still_extend(const std::string& path, const std::array<float, 4>& ltrb) {
  still_edit& s = edit_slot();
  const std::lock_guard<std::mutex> lock(s.m);
  s.extend_path = path;
  s.extend = {};
  s.extend_g = geometry_now(s, path);
  if (path.empty()) return;
  for (std::size_t i = 0; i < 4; ++i) {
    const float f = ltrb[i];
    s.extend[i] = f > 0.0f && std::isfinite(f) ? std::min(f, 1.0f) : 0.0f;
  }
}

std::array<float, 4> still_extend(const std::string& path) {
  still_edit& s = edit_slot();
  const std::lock_guard<std::mutex> lock(s.m);
  if (path.empty() || s.extend_path != path) return {};
  if (!same_geometry(s.extend_g, geometry_now(s, path))) return {};
  return s.extend;
}

result<rgb_image> full_still(const std::string& path) {
  edit::geometry g;
  edit::colour c;
  {
    still_edit& s = edit_slot();
    const std::lock_guard<std::mutex> lock(s.m);
    if (!path.empty() && s.path == path) {
      g = s.g;
      c = s.c;
    }
  }
  MV_TRY(const std::string file, readable_path(path));
  MV_TRY(std::vector<std::uint8_t> bytes, io::read_all(file));
  MV_TRY(codec::raster pic, edit::edited_picture(bytes, g, c));
  rgb_image out;
  out.width = pic.width;
  out.height = pic.height;
  const std::size_t px = static_cast<std::size_t>(pic.width) * pic.height;
  out.rgb.resize(px * 3);
  for (std::size_t i = 0; i < px; ++i) {
    out.rgb[i * 3] = pic.rgba[i * 4];
    out.rgb[i * 3 + 1] = pic.rgba[i * 4 + 1];
    out.rgb[i * 3 + 2] = pic.rgba[i * 4 + 2];
  }
  return out;
}

std::vector<std::string> burst_neighbours(const std::string& path) {
  namespace fs = std::filesystem;
  std::vector<std::string> out;
  std::error_code ec;
  // UTF-8 both ways (C++20: char8_t).
  const fs::path self(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
  auto utf8 = [](const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
  };
  const auto t0 = fs::last_write_time(self, ec);
  if (ec) return out;
  std::vector<std::pair<double, std::string>> near;
  for (const fs::directory_entry& e : fs::directory_iterator(self.parent_path(), ec)) {
    std::error_code ec2;
    if (!e.is_regular_file(ec2) || e.path() == self) continue;
    const auto t = e.last_write_time(ec2);
    if (ec2) continue;
    const double dt = std::abs(std::chrono::duration<double>(t - t0).count());
    if (dt <= kBurstSeconds) near.emplace_back(dt, utf8(e.path()));
  }
  std::sort(near.begin(), near.end());
  for (std::size_t i = 0; i < near.size() && i < kBurstFrames; ++i) out.push_back(std::move(near[i].second));
  return out;
}

std::string eyes_note(status pack, const result<eyes_outcome>& r) {
  if (pack == status::not_found) return "Open closed eyes needs Local search (Settings → Add-ons)";
  if (pack == status::invalid_arg) return "Turn on People in Local search to open closed eyes";
  if (pack != status::ok || !r) return "Could not look for open eyes in this photo";
  if (r->faces == 0) return "No faces found";
  if (r->fixed == 0) {
    return "No open eyes to use: no photo of the same moment, and none of this person in People "
           "with their head turned the same way";
  }
  // A library photo's result is a new photo there (S0): "added to your Photos library".
  const std::string verb = is_photos_key(r->written) ? " fixed — added " : " fixed — saved ";
  return (r->fixed == 1 ? std::string("1 face") : std::to_string(r->fixed) + " faces") + verb + result_name(r->written);
}

namespace {

bool read_landmarks(const json::value* lm, edit::face_landmarks& out) {
  if (!lm || lm->k != json::kind::array || lm->a.size() != out.size()) return false;
  for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<float>(lm->a[i].d);
  return true;
}

float json_number(const json::value& v, const char* key, float fallback) {
  const json::value* n = v.find(key);
  if (!n || n->k != json::kind::number) return fallback;
  return static_cast<float>(n->is_integer ? static_cast<double>(n->i) : n->d);
}

// The pack's box (x, y, w, h), or the landmarks' bounds grown to a face when
// an answer carries none (the tests' hand-written ones).
std::array<float, 4> face_box(const json::value& f, const edit::face_landmarks& lm) {
  const json::value* x = f.find("x");
  if (x && x->k == json::kind::number) {
    return {json_number(f, "x", 0.0f), json_number(f, "y", 0.0f), json_number(f, "w", 0.0f),
            json_number(f, "h", 0.0f)};
  }
  float x0 = 1.0f, y0 = 1.0f, x1 = 0.0f, y1 = 0.0f;
  for (std::size_t i = 0; i < 5; ++i) {
    x0 = std::min(x0, lm[2 * i]), x1 = std::max(x1, lm[2 * i]);
    y0 = std::min(y0, lm[2 * i + 1]), y1 = std::max(y1, lm[2 * i + 1]);
  }
  return {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}

// A rectangle of `img` in pixels: x0, y0 inclusive, x1, y1 exclusive.
struct px_rect {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  [[nodiscard]] int w() const noexcept { return x1 - x0; }
  [[nodiscard]] int h() const noexcept { return y1 - y0; }
};

px_rect to_px(const std::array<float, 4>& r, std::uint32_t w, std::uint32_t h) {
  const float fw = static_cast<float>(w), fh = static_cast<float>(h);
  px_rect p;
  p.x0 = std::clamp(static_cast<int>(std::floor(r[0] * fw)), 0, static_cast<int>(w));
  p.y0 = std::clamp(static_cast<int>(std::floor(r[1] * fh)), 0, static_cast<int>(h));
  p.x1 = std::clamp(static_cast<int>(std::ceil((r[0] + r[2]) * fw)), p.x0, static_cast<int>(w));
  p.y1 = std::clamp(static_cast<int>(std::ceil((r[1] + r[3]) * fh)), p.y0, static_cast<int>(h));
  return p;
}

image::linear_image crop_of(const image::linear_image& img, const px_rect& r) {
  image::linear_image out;
  out.width = static_cast<std::uint32_t>(r.w());
  out.height = static_cast<std::uint32_t>(r.h());
  out.format = img.format;
  out.from_raw = img.from_raw;
  out.rgba.resize(std::size_t{out.width} * out.height * 4);
  for (int y = 0; y < r.h(); ++y) {
    const std::uint16_t* src = img.rgba.data() + (static_cast<std::size_t>(r.y0 + y) * img.width + r.x0) * 4;
    std::copy(src, src + std::size_t{out.width} * 4, out.rgba.data() + static_cast<std::size_t>(y) * out.width * 4);
  }
  return out;
}

// One face's fix, kept inside its edit region: the transfer runs on a crop
// (the region and a margin, so its look-matching ring reads real skin), and
// only the region is written back, faded to the original over its outer edge.
// `transfer` gets the crop and the face's landmarks in it.
template <class Transfer>
bool fix_in_region(image::linear_image& target, const edit::face_landmarks& lm, const std::array<float, 4>& box,
                   Transfer transfer) {
  const float tw = static_cast<float>(target.width), th = static_cast<float>(target.height);
  const std::array<float, 4> region = edit::face_region(box, lm, tw, th);
  const px_rect rp = to_px(region, target.width, target.height);
  if (rp.w() < 4 || rp.h() < 4) return false;
  const std::array<float, 4> margin{region[0] - region[2] * 0.5f, region[1] - region[3] * 0.5f, region[2] * 2.0f,
                                    region[3] * 2.0f};
  const px_rect cp = to_px(margin, target.width, target.height);
  image::linear_image crop = crop_of(target, cp);
  edit::face_landmarks local{};
  const float cw = static_cast<float>(crop.width), ch = static_cast<float>(crop.height);
  for (std::size_t i = 0; i < 5; ++i) {
    local[2 * i] = (lm[2 * i] * tw - static_cast<float>(cp.x0)) / cw;
    local[2 * i + 1] = (lm[2 * i + 1] * th - static_cast<float>(cp.y0)) / ch;
  }
  if (!transfer(crop, local)) return false;
  // Back into the region: weight 1 inside, falling to 0 at its border over
  // `fade` pixels, so nothing outside it moves and there is no seam.
  const float fade = std::max(2.0f, 0.08f * static_cast<float>(std::min(rp.w(), rp.h())));
  for (int y = rp.y0; y < rp.y1; ++y) {
    const float dy = std::min(static_cast<float>(y - rp.y0) + 0.5f, static_cast<float>(rp.y1 - y) - 0.5f);
    for (int x = rp.x0; x < rp.x1; ++x) {
      const float dx = std::min(static_cast<float>(x - rp.x0) + 0.5f, static_cast<float>(rp.x1 - x) - 0.5f);
      const float t = std::clamp(std::min(dx, dy) / fade, 0.0f, 1.0f);
      const float wgt = t * t * (3.0f - 2.0f * t);
      if (wgt <= 0.0f) continue;
      std::uint16_t* dst = target.rgba.data() + (static_cast<std::size_t>(y) * target.width + x) * 4;
      const std::uint16_t* src =
          crop.rgba.data() + (static_cast<std::size_t>(y - cp.y0) * crop.width + (x - cp.x0)) * 4;
      if (wgt >= 1.0f) {
        std::copy(src, src + 4, dst);
        continue;
      }
      for (int c = 0; c < 3; ++c) {
        const float a = image::half_to_float(dst[c]), b = image::half_to_float(src[c]);
        dst[c] = image::float_to_half(a + (b - a) * wgt);
      }
    }
  }
  return true;
}

}  // namespace

const char* face_fix_suffix(face_fix fix) noexcept {
  return fix == face_fix::open_eyes ? "eyes" : "faces";
}

result<int> fix_faces_in(image::linear_image& target, std::string_view pack_json, face_fix fix,
                         std::uint64_t faces_mask, const job_context* ctx, int* found) {
  auto doc = json::parse(pack_json);
  if (!doc) return err(status::invalid_arg);
  const json::value* faces = doc->find("faces");
  if (!faces || faces->k != json::kind::array) return err(status::invalid_arg);
  if (found) *found = static_cast<int>(faces->a.size());
  if (!target.valid()) return err(status::invalid_arg);
  std::vector<std::pair<std::string, image::linear_image>> donors;  // decoded once each
  auto donor_image = [&](const std::string& p) -> const image::linear_image* {
    for (const auto& [k, img] : donors) {
      if (k == p) return &img;
    }
    auto readable = readable_path(p, ctx);
    if (!readable) return nullptr;
    auto b = io::read_all(*readable);
    if (!b) return nullptr;
    auto img = image::decode_linear(*b, ctx);
    if (!img) return nullptr;
    donors.emplace_back(p, std::move(*img));
    return &donors.back().second;
  };
  const float tw = static_cast<float>(target.width), th = static_cast<float>(target.height);
  // A donor's eyes for a face: the most open, by kEyesBetter over these. Of
  // the same moment, the three most alike are enough. From the person's other
  // photos (People, "library"), any of them whose head is turned as this one
  // is (pose_distance), since one from another day rarely is.
  auto eyes_donor = [&](const json::value& f, const edit::face_landmarks& t, edit::face_landmarks& out_lm)
      -> const image::linear_image* {
    const json::value* list = f.find("donors");
    if (!list || list->k != json::kind::array) return nullptr;
    const image::linear_image* best = nullptr;
    float best_open = edit::eye_openness(target, t) * edit::kEyesBetter;
    int seen_moment = 0;
    for (const json::value& d : list->a) {
      const bool library = d.find("library") && d.find("library")->k == json::kind::boolean && d.find("library")->b;
      if (!library && ++seen_moment > 3) continue;
      const std::string* dp = d.str("path");
      edit::face_landmarks lm{};
      if (!dp || !read_landmarks(d.find("lm"), lm)) continue;
      const image::linear_image* img = donor_image(*dp);
      if (!img) continue;
      if (library && edit::pose_distance(t, tw, th, lm, static_cast<float>(img->width),
                                         static_cast<float>(img->height)) > edit::kPoseMatch) {
        continue;
      }
      const float open = edit::eye_openness(*img, lm);
      if (open > best_open && open > 0.0f) {
        best_open = open;
        best = img;
        out_lm = lm;
      }
    }
    return best;
  };
  // A clearly better face of the same person: the pack's quality up by
  // kFaceBetter, or the same quality with the eyes open where these are shut.
  auto face_donor = [&](const json::value& f, const edit::face_landmarks& t, edit::face_landmarks& out_lm)
      -> const image::linear_image* {
    const json::value* list = f.find("donors");
    if (!list || list->k != json::kind::array) return nullptr;
    constexpr float kFaceBetter = 0.08f;
    const float here_open = edit::eye_openness(target, t);
    float best_score = json_number(f, "quality", 0.5f) + kFaceBetter;
    const image::linear_image* best = nullptr;
    for (const json::value& d : list->a) {
      const std::string* dp = d.str("path");
      edit::face_landmarks lm{};
      if (!dp || !read_landmarks(d.find("lm"), lm)) continue;
      const image::linear_image* img = donor_image(*dp);
      if (!img) continue;
      if (edit::pose_distance(t, tw, th, lm, static_cast<float>(img->width), static_cast<float>(img->height)) >
          edit::kPoseMatch) {
        continue;
      }
      float score = json_number(d, "quality", 0.5f);
      const float open = edit::eye_openness(*img, lm);
      if (open > here_open * edit::kEyesBetter && open > 0.0f) score += 0.15f;  // shut here, open there
      if (score > best_score) {
        best_score = score;
        best = img;
        out_lm = lm;
      }
    }
    return best;
  };
  int fixed = 0;
  for (std::size_t i = 0; i < faces->a.size(); ++i) {
    if (ctx && ctx->cancelled()) return err(status::cancelled);
    if (i >= 64 || (faces_mask & (std::uint64_t{1} << i)) == 0) continue;
    const json::value& f = faces->a[i];
    edit::face_landmarks t{};
    if (!read_landmarks(f.find("lm"), t)) continue;
    const std::array<float, 4> box = face_box(f, t);
    edit::face_landmarks dlm{};
    bool done = false;
    if (fix == face_fix::improve || fix == face_fix::do_all) {
      if (const image::linear_image* d = face_donor(f, t, dlm)) {
        done = fix_in_region(target, t, box, [&](image::linear_image& crop, const edit::face_landmarks& local) {
          return edit::transfer_face(crop, local, *d, dlm).has_value();
        });
      }
    }
    if (!done && (fix == face_fix::open_eyes || fix == face_fix::do_all)) {
      if (const image::linear_image* d = eyes_donor(f, t, dlm)) {
        done = fix_in_region(target, t, box, [&](image::linear_image& crop, const edit::face_landmarks& local) {
          return edit::transfer_eyes(crop, local, *d, dlm).has_value();
        });
      }
    }
    if (done) ++fixed;
  }
  return fixed;
}

result<eyes_outcome> fix_faces(const std::string& path, std::string_view pack_json, face_fix fix,
                               std::uint64_t faces_mask, const job_context* ctx) {
  // The answer first: a malformed one reads nothing.
  {
    auto doc = json::parse(pack_json);
    if (!doc) return err(status::invalid_arg);
    const json::value* faces = doc->find("faces");
    if (!faces || faces->k != json::kind::array) return err(status::invalid_arg);
    eyes_outcome none;
    none.faces = static_cast<int>(faces->a.size());
    if (faces->a.empty() || faces_mask == 0) return none;
  }
  MV_TRY(const std::string file, readable_path(path, ctx));
  MV_TRY(std::vector<std::uint8_t> bytes, io::read_all(file));
  MV_TRY(image::linear_image target, image::decode_linear(bytes, ctx));
  eyes_outcome r;
  MV_TRY(r.fixed, fix_faces_in(target, pack_json, fix, faces_mask, ctx, &r.faces));
  if (r.fixed == 0) return r;
  // Upright 8-bit sRGB, as the export bake writes it, then beside the original.
  MV_TRY(codec::raster out,
         edit::bake(target, edit::place(edit::geometry{}, edit::size2{target.width, target.height}),
                    edit::uniforms_of(edit::colour{}), ctx));
  rgb_image pixels;
  pixels.width = out.width;
  pixels.height = out.height;
  const std::size_t px = static_cast<std::size_t>(out.width) * out.height;
  pixels.rgb.resize(px * 3);
  for (std::size_t i = 0; i < px; ++i) {
    pixels.rgb[i * 3] = out.rgba[i * 4];
    pixels.rgb[i * 3 + 1] = out.rgba[i * 4 + 1];
    pixels.rgb[i * 3 + 2] = out.rgba[i * 4 + 2];
  }
  MV_TRY(r.written, write_still(path, pixels, face_fix_suffix(fix)));
  return r;
}

result<eyes_outcome> open_closed_eyes(const std::string& path, std::string_view pack_json, const job_context* ctx,
                                      std::uint64_t faces_mask) {
  return fix_faces(path, pack_json, face_fix::open_eyes, faces_mask, ctx);
}

result<eyes_outcome> improve_faces(const std::string& path, std::string_view pack_json, const job_context* ctx,
                                   std::uint64_t faces_mask) {
  return fix_faces(path, pack_json, face_fix::improve, faces_mask, ctx);
}

std::string fix_note(face_fix fix, status pack, const result<eyes_outcome>& r) {
  if (fix == face_fix::open_eyes) return eyes_note(pack, r);
  if (fix == face_fix::do_all && pack == status::ok && r && r->faces > 0 && r->fixed == 0) {
    return "Nothing to fix: no clearly better face of these people, and no open eyes to use";
  }
  return faces_note(pack, r);
}

std::uint64_t faces_mask_of(std::string_view pack_json, std::span<const float> boxes) {
  auto doc = json::parse(pack_json);
  if (!doc) return 0;
  const json::value* faces = doc->find("faces");
  if (!faces || faces->k != json::kind::array) return 0;
  std::vector<std::array<float, 4>> here;
  for (const json::value& f : faces->a) {
    edit::face_landmarks lm{};
    (void)read_landmarks(f.find("lm"), lm);
    here.push_back(face_box(f, lm));
  }
  auto iou = [](const std::array<float, 4>& a, const float* b) {
    const float ix = std::max(0.0f, std::min(a[0] + a[2], b[0] + b[2]) - std::max(a[0], b[0]));
    const float iy = std::max(0.0f, std::min(a[1] + a[3], b[1] + b[3]) - std::max(a[1], b[1]));
    const float inter = ix * iy, uni = a[2] * a[3] + b[2] * b[3] - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
  };
  std::uint64_t mask = 0;
  for (std::size_t b = 0; b + 4 <= boxes.size(); b += 4) {
    int best = -1;
    float best_iou = 0.3f;
    for (std::size_t i = 0; i < here.size() && i < 64; ++i) {
      if (mask & (std::uint64_t{1} << i)) continue;
      const float v = iou(here[i], boxes.data() + b);
      if (v >= best_iou) {
        best_iou = v;
        best = static_cast<int>(i);
      }
    }
    if (best >= 0) mask |= std::uint64_t{1} << best;
  }
  return mask;
}

std::string faces_note(status pack, const result<eyes_outcome>& r) {
  if (pack == status::not_found) return "Improving faces needs Local search (Settings → Add-ons)";
  if (pack == status::invalid_arg) return "Turn on People in Local search to improve faces";
  if (pack != status::ok || !r) return "Could not look for better faces for this photo";
  if (r->faces == 0) return "No faces found";
  if (r->fixed == 0) {
    return "No clearly better photo of these people with their head turned the same way";
  }
  const std::string verb = is_photos_key(r->written) ? " improved — added " : " improved — saved ";
  return (r->fixed == 1 ? std::string("1 face") : std::to_string(r->fixed) + " faces") + verb + result_name(r->written);
}

result<std::string> write_still(const std::string& source, const rgb_image& pixels, const std::string& suffix) {
  if (suffix.empty() || suffix.size() > 32 ||
      !std::all_of(suffix.begin(), suffix.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
      })) {
    return err(status::invalid_arg);
  }
  // A Photos item: read its file, write beside nothing of the library's, and
  // hand the new file to the library as a new photo (docs/design/26).
  const bool to_library = is_photos_key(source);
  MV_TRY(const std::string source_file, readable_path(source));
  MV_TRY(std::vector<std::uint8_t> bytes, io::read_all(source_file));
  const codec::format_family family = codec::probe(bytes);
  // HEIC, TIFF, RAW, WebP: their metadata from Exiv2 (meta/), as an export does.
  edit::metadata_blobs carried;
  if (family != codec::format_family::jpeg && family != codec::format_family::png) {
    meta::carried_metadata m = meta::read_carried(bytes);
    carried.exif = std::move(m.exif);
    carried.xmp = std::move(m.xmp);
  }
  codec::raster pic;
  pic.width = pixels.width;
  pic.height = pixels.height;
  pic.tagged_srgb = true;
  const std::size_t px = static_cast<std::size_t>(pixels.width) * pixels.height;
  if (pixels.rgb.size() < px * 3) return err(status::invalid_arg);
  pic.rgba.assign(px * 4, 255);
  for (std::size_t i = 0; i < px; ++i) {
    pic.rgba[i * 4] = pixels.rgb[i * 3];
    pic.rgba[i * 4 + 1] = pixels.rgb[i * 3 + 1];
    pic.rgba[i * 4 + 2] = pixels.rgb[i * 3 + 2];
  }
  edit::export_options opt;
  opt.encode.format = family == codec::format_family::png ? edit::image_format::png : edit::image_format::jpeg;
  opt.encode.quality = 92;
  MV_TRY(edit::export_result r, edit::export_pixels(bytes, pic, opt, &carried));
  pic = codec::raster{};

  const std::size_t sep = source_file.find_last_of("/\\");
  std::string dir = sep == std::string::npos ? std::string() : source_file.substr(0, sep + 1);
  const std::string name = sep == std::string::npos ? source_file : source_file.substr(sep + 1);
  if (to_library) {
    MV_TRY(std::string staging, staging_dir());
    dir = std::move(staging);
  }
  std::size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0) dot = name.size();
  const std::string wanted =
      name.substr(0, dot) + "-" + suffix + (opt.encode.format == edit::image_format::png ? ".png" : ".jpg");
  // write_new refuses an existing name, so a race moves on to the next number.
  for (int attempt = 0; attempt < 8; ++attempt) {
    const std::string file =
        io::unique_name(wanted, [&](std::string_view candidate) { return io::file_exists(dir + std::string(candidate)); });
    if (file.empty()) return err(status::io);
    const std::string out = dir + file;
    if (io::write_new(out, r.bytes)) {
      if (!to_library) return out;
      return add_to_library(out, source);  // the new photo's key; our copy is gone
    }
    if (!io::file_exists(out)) return err(status::io);
  }
  return err(status::io);
}

result<std::string> moment_thumbnail(const std::string& path, std::int64_t pts_ms,
                                     const rgb_image* image) {
  MV_TRY(image::thumb_store* opened, thumbs());
  image::thumb_store& store = *opened;
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
  std::vector<std::uint8_t> rgba = rgba_of(image->rgb.data(), image->width, image->height);
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

result<std::vector<std::uint8_t>> thumbnail_jpeg(const std::string& path, std::int64_t pts_ms) {
  MV_TRY(image::thumb_store* store, thumbs());
  MV_TRY(image::thumb_key key, cache_key(path, pts_ms));
  // The read is the existence check (issue #168): a row whose file has gone
  // is dropped here, the same miss lookup() would have reported.
  MV_TRY(std::string hit, store->lookup_unverified(key));
  if (hit.empty()) return err(status::io);
  auto bytes = io::read_all(hit);
  if (!bytes) store->forget(key);
  return bytes;
}

expected store_thumbnail_jpeg(const std::string& path, std::int64_t pts_ms,
                              std::span<const std::uint8_t> jpeg) {
  if (jpeg.size() < 4 || jpeg.size() > kMaxThumbBytes) return err(status::invalid_arg);
  if (jpeg[0] != 0xFF || jpeg[1] != 0xD8) return err(status::unsupported_format);
  // Another machine's bytes: our own (fuzzed) decoder says what they are
  // before the gallery ever does.
  MV_TRY(image::display_image img, image::decode_bytes(jpeg, nullptr, 1));
  if (img.format != codec::format_family::jpeg) return err(status::unsupported_format);
  if (img.width == 0 || img.height == 0 || std::max(img.width, img.height) > image::kThumbLongEdge) {
    return err(status::invalid_arg);
  }
  MV_TRY(image::thumb_store* store, thumbs());
  MV_TRY(image::thumb_key key, cache_key(path, pts_ms));
  MV_TRY_VOID(store->store(key, jpeg));
  return {};
}

result<std::vector<std::uint8_t>> encode_moment_thumb(const std::string& path, std::int64_t pts_ms,
                                                      const std::atomic<bool>* cancel) {
  if (io::is_document_name(path)) {
    MV_TRY(rgb_image page, document_page(path, pts_ms, image::kThumbLongEdge));
    const std::vector<std::uint8_t> rgba = rgba_of(page.rgb.data(), page.width, page.height);
    return image::encode_thumb_rgba(rgba, page.width, page.height);
  }
  MV_TRY(auto f, edit::clip::frame_rgb_at(path, pts_ms, image::kThumbLongEdge, cancel));
  if (f.width == 0 || f.height == 0 ||
      f.rgb.size() < static_cast<std::size_t>(f.width) * f.height * 3) {
    return err(status::corrupt);
  }
  const std::vector<std::uint8_t> rgba = rgba_of(f.rgb.data(), f.width, f.height);
  return image::encode_thumb_rgba(rgba, f.width, f.height);
}

expected document_text(const std::string& path, const std::function<bool(const document_line&)>& visit) {
  // PDF text is the AI pack's own (PDFium in the pack, docs/plans/document-search.md
  // §2): the host answers for the documents it lays out itself.
  if (!io::is_document_name(path)) return err(status::unsupported_format);
  MV_TRY(auto bytes, io::read_all(path));
  // A password-protected DOCX is an OLE compound file around the encrypted
  // package, not a zip: locked, not broken.
  static constexpr std::uint8_t kOle[] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
  if (bytes.size() >= sizeof kOle && std::equal(std::begin(kOle), std::end(kOle), bytes.begin())) {
    return err(status::permission_denied);
  }
  if (codec::probe(bytes) != codec::format_family::docx) return err(status::unsupported_format);
  MV_TRY(auto paragraphs, codec::docx_text(bytes));
  std::uint32_t pages = 1;
  for (const codec::docx_paragraph& p : paragraphs) pages = std::max(pages, p.page + 1);
  for (const codec::docx_paragraph& p : paragraphs) {
    if (!visit(document_line{p.page, pages, p.text})) return err(status::cancelled);
  }
  return {};
}

result<std::vector<mv_addon_route_point>> route(const std::string& path, std::uint32_t max_points) {
  std::vector<mv_addon_route_point> out;
  for (const meta::geo_point& p : meta::read_route(path, max_points)) out.push_back({p.lat, p.lon});
  return out;
}

}  // namespace mv::addon::media
