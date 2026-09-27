// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// HEIC/HEIF via libheif + libde265 (LGPL, dynamic). No x265 — the vcpkg
// `hevc` feature is an encoder and is forbidden (plan/11, plan/12).
//
// libheif applies the container transforms (clap, irot, imir) by default; that
// is kept, so the displayed orientation comes from the container, as on an
// iPhone. Gain-map auxiliary images are ignored (HDR output is v1.1).
//
// Colour (D6, codec/cicp.h): an embedded ICC wins for SDR; nclx P3 gets a
// synthesised ICC; PQ/HLG are tone-mapped to SDR here with the video shader's
// curves. >8-bit SDR is decoded at 16-bit interleaved and rounded to 8 bits
// here (libheif's own down-conversion truncates).
//
// Threading: one heif_context per call / per animation source. libheif's
// plugin registry is initialised once, below, and not touched afterwards.
#include "codec/decode.h"

#include <libheif/heif.h>
#include <libheif/heif_items.h>
#include <libheif/heif_security.h>
#include <libheif/heif_sequences.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>

#include "codec/cicp.h"
#include "codec/os_decode.h"

namespace mv::codec {
namespace {

constexpr std::uint32_t kMaxDim = 65535;
constexpr std::uint64_t kMaxPixels = 256ull * 1000ull * 1000ull;
constexpr std::uint64_t kMaxTotalMemory = 4ull * 1024ull * 1024ull * 1024ull;

constexpr std::uint32_t fourcc(char a, char b, char c, char d) noexcept {
  return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8) |
         static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}

// heif_init loads built-in plugins (libde265) into a process-wide registry.
// Reference counted and not meant to race; do it once and never deinit, so no
// worker ever sees the registry change under it.
bool ensure_heif_init() noexcept {
  static std::once_flag once;
  static bool hevc = false;
  std::call_once(once, [] {
    heif_init(nullptr);
    hevc = heif_have_decoder_for_format(heif_compression_HEVC) != 0;
  });
  return hevc;
}

status map_error(const heif_error& e) noexcept {
  switch (e.code) {
    case heif_error_Ok: return status::ok;
    case heif_error_Canceled: return status::cancelled;
    case heif_error_Memory_allocation_error:
      return e.subcode == heif_suberror_Security_limit_exceeded ? status::unsupported_format
                                                                : status::out_of_memory;
    case heif_error_Unsupported_filetype:
    case heif_error_Unsupported_feature:
    case heif_error_Plugin_loading_error:
      return status::unsupported_format;
    default: return status::corrupt;
  }
}

struct context_deleter {
  void operator()(heif_context* c) const noexcept { heif_context_free(c); }
};
struct handle_deleter {
  void operator()(heif_image_handle* h) const noexcept { heif_image_handle_release(h); }
};
struct image_deleter {
  void operator()(heif_image* i) const noexcept { heif_image_release(i); }
};
struct nclx_deleter {
  void operator()(heif_color_profile_nclx* n) const noexcept { heif_nclx_color_profile_free(n); }
};
struct options_deleter {
  void operator()(heif_decoding_options* o) const noexcept { heif_decoding_options_free(o); }
};
struct track_deleter {
  void operator()(heif_track* t) const noexcept { heif_track_release(t); }
};
using context_ptr = std::unique_ptr<heif_context, context_deleter>;
using handle_ptr = std::unique_ptr<heif_image_handle, handle_deleter>;
using image_ptr = std::unique_ptr<heif_image, image_deleter>;
using nclx_ptr = std::unique_ptr<heif_color_profile_nclx, nclx_deleter>;
using options_ptr = std::unique_ptr<heif_decoding_options, options_deleter>;
using track_ptr = std::unique_ptr<heif_track, track_deleter>;

bool dims_ok(std::uint64_t w, std::uint64_t h) noexcept {
  return w > 0 && h > 0 && w <= kMaxDim && h <= kMaxDim && w * h <= kMaxPixels;
}

// A context over `bytes` (not copied: the caller keeps them alive) with our
// limits. Nothing is decoded yet.
result<context_ptr> open_context(std::span<const std::uint8_t> bytes) {
  context_ptr ctx(heif_context_alloc());
  if (!ctx) return err(status::out_of_memory);
  if (const heif_security_limits* current = heif_context_get_security_limits(ctx.get())) {
    heif_security_limits limits = *current;
    limits.max_image_size_pixels = kMaxPixels;
    if (limits.max_total_memory == 0 || limits.max_total_memory > kMaxTotalMemory) {
      limits.max_total_memory = kMaxTotalMemory;
    }
    (void)heif_context_set_security_limits(ctx.get(), &limits);
  }
  // Already on a pool worker: decode grid tiles on this thread.
  heif_context_set_max_decoding_threads(ctx.get(), 0);
  const heif_error e =
      heif_context_read_from_memory_without_copy(ctx.get(), bytes.data(), bytes.size(), nullptr);
  if (e.code != heif_error_Ok) return err(map_error(e) == status::ok ? status::corrupt : map_error(e));
  return ctx;
}

int cancel_cb(void* user) {
  const auto* job = static_cast<const job_context*>(user);
  return job && job->cancelled() ? 1 : 0;
}

options_ptr make_options(const job_context* job, bool sequence) {
  options_ptr opts(heif_decoding_options_alloc());
  if (!opts) return opts;
  opts->ignore_transformations = 0;  // container orientation on the display path
  opts->convert_hdr_to_8bit = 0;
  opts->progress_user_data = const_cast<job_context*>(job);
  opts->cancel_decoding = cancel_cb;
  if (sequence) opts->ignore_sequence_editlist = 1;  // loops are the animation clock's job
  return opts;
}

// Colour description from the handle's boxes (colr prof/rICC/nclx), falling
// back to the decoded image (an nclx that only exists in the HEVC VUI).
struct colour_info {
  std::vector<std::uint8_t> icc;
  bool have_nclx = false;
  std::uint16_t primaries = cicp::kPrimariesUnspecified;
  std::uint16_t transfer = 2;
};

void read_nclx(heif_color_profile_nclx* raw, colour_info& out) {
  nclx_ptr nclx(raw);
  if (!nclx) return;
  out.have_nclx = true;
  out.primaries = static_cast<std::uint16_t>(nclx->color_primaries);
  out.transfer = static_cast<std::uint16_t>(nclx->transfer_characteristics);
}

result<colour_info> handle_colour(const heif_image_handle* handle) {
  colour_info out;
  const heif_color_profile_type type = heif_image_handle_get_color_profile_type(handle);
  if (type == heif_color_profile_type_prof || type == heif_color_profile_type_rICC) {
    const std::size_t size = heif_image_handle_get_raw_color_profile_size(handle);
    if (size > 0) {
      out.icc.resize(size);
      if (heif_image_handle_get_raw_color_profile(handle, out.icc.data()).code != heif_error_Ok) {
        return err(status::corrupt);  // a tagged file with an unreadable profile is not sRGB
      }
    }
  }
  heif_color_profile_nclx* raw = nullptr;
  if (heif_image_handle_get_nclx_color_profile(handle, &raw).code == heif_error_Ok) {
    read_nclx(raw, out);
  }
  return out;
}

void image_colour_fallback(const heif_image* img, colour_info& info) {
  if (info.have_nclx || !info.icc.empty()) return;
  heif_color_profile_nclx* raw = nullptr;
  if (heif_image_get_nclx_color_profile(img, &raw).code == heif_error_Ok) read_nclx(raw, info);
}

// Decoded interleaved image → source-encoded RGBA8 + colour tag.
result<raster> to_raster(const heif_image* img, const colour_info& colour, bool high_bit,
                         const job_context* job) {
  const int w = heif_image_get_width(img, heif_channel_interleaved);
  const int h = heif_image_get_height(img, heif_channel_interleaved);
  if (w <= 0 || h <= 0 || !dims_ok(static_cast<std::uint64_t>(w), static_cast<std::uint64_t>(h))) {
    return err(status::corrupt);
  }
  std::size_t stride = 0;
  const std::uint8_t* plane = heif_image_get_plane_readonly2(img, heif_channel_interleaved, &stride);
  const std::size_t width = static_cast<std::size_t>(w);
  const std::size_t bpp = high_bit ? 8 : 4;
  if (!plane || stride < width * bpp) return err(status::corrupt);

  raster out;
  out.width = static_cast<std::uint32_t>(w);
  out.height = static_cast<std::uint32_t>(h);
  out.format = format_family::heic;
  out.intent = transfer_intent::display_referred;
  out.rgba.resize(width * static_cast<std::size_t>(h) * 4);

  const bool hdr = colour.have_nclx && cicp::is_hdr(colour.transfer);
  if (!high_bit) {
    for (int y = 0; y < h; ++y) {
      std::memcpy(out.rgba.data() + static_cast<std::size_t>(y) * width * 4,
                  plane + static_cast<std::size_t>(y) * stride, width * 4);
    }
  } else {
    int bits = heif_image_get_bits_per_pixel_range(img, heif_channel_interleaved);
    if (bits < 8 || bits > 16) bits = 16;
    cicp::hdr_to_sdr map;
    if (hdr && !map.init(colour.primaries, colour.transfer, static_cast<std::uint32_t>(bits))) {
      return err(status::unsupported_format);
    }
    std::vector<std::uint16_t> row(width * 4);
    for (int y = 0; y < h; ++y) {
      if ((y & 63) == 0 && job && job->cancelled()) return err(status::cancelled);
      const std::uint8_t* src = plane + static_cast<std::size_t>(y) * stride;
      for (std::size_t i = 0; i < width * 4; ++i) {
        row[i] = static_cast<std::uint16_t>(src[i * 2] | (src[i * 2 + 1] << 8));  // _LE
      }
      std::uint8_t* dst = out.rgba.data() + static_cast<std::size_t>(y) * width * 4;
      if (hdr) {
        map.row(row.data(), width, dst);
      } else {
        cicp::samples_to_8bit(row.data(), width * 4, static_cast<std::uint32_t>(bits), dst);
      }
    }
  }

  // Tagging. HDR output is now sRGB-encoded BT.709: untagged. An ICC beats
  // an SDR nclx (the profile is the more specific statement).
  if (hdr) return out;
  if (!colour.icc.empty()) {
    out.icc = colour.icc;
  } else if (colour.have_nclx) {
    cicp::sdr_tag tag = cicp::tag_sdr(colour.primaries, colour.transfer);
    out.icc = std::move(tag.icc);
    out.tagged_srgb = tag.tagged_srgb;
  }
  return out;
}

result<raster> decode_handle(heif_image_handle* handle, const job_context* job) {
  const std::uint64_t iw = static_cast<std::uint64_t>(std::max(0, heif_image_handle_get_ispe_width(handle)));
  const std::uint64_t ih = static_cast<std::uint64_t>(std::max(0, heif_image_handle_get_ispe_height(handle)));
  const std::uint64_t dw = static_cast<std::uint64_t>(std::max(0, heif_image_handle_get_width(handle)));
  const std::uint64_t dh = static_cast<std::uint64_t>(std::max(0, heif_image_handle_get_height(handle)));
  // Checked before any pixel buffer exists: an absurd ispe costs nothing.
  if (!dims_ok(iw, ih) || !dims_ok(dw, dh)) return err(status::unsupported_format);

  auto colour = handle_colour(handle);
  if (!colour) return err(colour.error());
  const bool high_bit = heif_image_handle_get_luma_bits_per_pixel(handle) > 8 ||
                        (colour.value().have_nclx && cicp::is_hdr(colour.value().transfer));

  options_ptr opts = make_options(job, false);
  if (!opts) return err(status::out_of_memory);
  heif_image* raw = nullptr;
  const heif_error e = heif_decode_image(
      handle, &raw, heif_colorspace_RGB,
      high_bit ? heif_chroma_interleaved_RRGGBBAA_LE : heif_chroma_interleaved_RGBA, opts.get());
  image_ptr img(raw);
  if (job && job->cancelled()) return err(status::cancelled);
  if (e.code != heif_error_Ok || !img) {
    const status s = map_error(e);
    return err(s == status::ok ? status::corrupt : s);
  }
  image_colour_fallback(img.get(), colour.value());
  return to_raster(img.get(), colour.value(), high_bit, job);
}

// --- sequences -----------------------------------------------------------------

class heic_source final : public animation_source {
 public:
  explicit heic_source(std::shared_ptr<const std::vector<std::uint8_t>> bytes) noexcept
      : bytes_(std::move(bytes)) {}

  [[nodiscard]] expected open() {
    auto ctx = open_context(*bytes_);
    if (!ctx) return err(ctx.error());
    ctx_ = std::move(ctx.value());
    if (!heif_context_has_sequence(ctx_.get())) return err(status::unsupported_format);
    track_.reset(heif_context_get_track(ctx_.get(), 0));
    if (!track_) return err(status::unsupported_format);
    std::uint16_t w = 0, h = 0;
    if (heif_track_get_image_resolution(track_.get(), &w, &h).code != heif_error_Ok ||
        !dims_ok(w, h)) {
      return err(status::unsupported_format);
    }
    timescale_ = heif_track_get_timescale(track_.get());
    const std::uint32_t reps = heif_track_get_number_of_repetitions(track_.get());
    info_.loops = reps == heif_sequence_track_number_of_repetitions_infinite ? 0
                  : reps == 0                                                ? 1
                                                                             : reps;
    info_.format = format_family::heic;
    info_.frame_count = 0;  // not known without walking the samples

    // Frame 0 now: it fixes the canvas size and the colour tag the session
    // needs before any frame is shown, and proves the track decodes.
    auto first = decode_next(nullptr);
    if (!first) return err(first.error());
    if (!first.value()) return err(status::corrupt);
    info_.width = pending_.width;
    info_.height = pending_.height;
    info_.icc = pending_.icc;
    info_.tagged_srgb = pending_.tagged_srgb;
    have_pending_ = true;
    return {};
  }

  [[nodiscard]] const animation_info& info() const noexcept override { return info_; }

  [[nodiscard]] result<bool> next(canvas_frame& out, const job_context* ctx) override {
    if (ctx && ctx->cancelled()) return err(status::cancelled);
    try {
      if (!have_pending_) {
        auto got = decode_next(ctx);
        if (!got) {
          if (got.error() == status::cancelled || next_index_ == 0) return err(got.error());
          return false;  // a broken sample ends the play at the last good frame
        }
        if (!got.value()) return false;
      }
      have_pending_ = false;
      if (pending_.width != info_.width || pending_.height != info_.height) return false;
      out.rgba = std::move(pending_.rgba);
      out.delay_ms = pending_delay_;
      out.index = next_index_++;
      return true;
    } catch (const std::bad_alloc&) {
      return err(status::out_of_memory);
    }
  }

  // The track's sample cursor has no reset in the C API: reopen the context.
  [[nodiscard]] expected rewind() override {
    track_.reset();
    ctx_.reset();
    auto ctx = open_context(*bytes_);
    if (!ctx) return err(ctx.error());
    ctx_ = std::move(ctx.value());
    track_.reset(heif_context_get_track(ctx_.get(), 0));
    if (!track_) return err(status::corrupt);
    have_pending_ = false;
    next_index_ = 0;
    return {};
  }

 private:
  result<bool> decode_next(const job_context* job) {
    options_ptr opts = make_options(job, true);
    if (!opts) return err(status::out_of_memory);
    // Tracks have no handle to ask for a bit depth: always ask for 16-bit
    // interleaved. An 8-bit track reports an 8-bit range and round-trips exactly.
    heif_image* raw = nullptr;
    const heif_error e = heif_track_decode_next_image(
        track_.get(), &raw, heif_colorspace_RGB, heif_chroma_interleaved_RRGGBBAA_LE, opts.get());
    image_ptr img(raw);
    if (e.code == heif_error_End_of_sequence) return false;
    if (job && job->cancelled()) return err(status::cancelled);
    if (e.code != heif_error_Ok || !img) {
      const status s = map_error(e);
      return err(s == status::ok ? status::corrupt : s);
    }
    colour_info colour;
    const heif_color_profile_type type = heif_image_get_color_profile_type(img.get());
    if (type == heif_color_profile_type_prof || type == heif_color_profile_type_rICC) {
      const std::size_t size = heif_image_get_raw_color_profile_size(img.get());
      colour.icc.resize(size);
      if (size && heif_image_get_raw_color_profile(img.get(), colour.icc.data()).code != heif_error_Ok) {
        return err(status::corrupt);
      }
    }
    image_colour_fallback(img.get(), colour);
    auto r = to_raster(img.get(), colour, true, job);
    if (!r) return err(r.error());
    pending_ = std::move(r.value());
    const std::uint32_t ticks = heif_image_get_duration(img.get());
    const std::uint64_t ms =
        timescale_ ? (static_cast<std::uint64_t>(ticks) * 1000u + timescale_ / 2) / timescale_ : 0;
    pending_delay_ = browser_frame_delay_ms(static_cast<std::uint32_t>(std::min<std::uint64_t>(ms, 0xFFFFFFFFu)));
    return true;
  }

  std::shared_ptr<const std::vector<std::uint8_t>> bytes_;
  // Destroyed in reverse: the track before the context that owns its data.
  context_ptr ctx_;
  track_ptr track_;
  animation_info info_{};
  raster pending_{};
  std::uint32_t pending_delay_ = 0;
  std::uint32_t timescale_ = 0;
  std::uint32_t next_index_ = 0;
  bool have_pending_ = false;
};

}  // namespace

result<raster> decode_heic(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  if (probe(bytes) != format_family::heic) return err(status::unsupported_format);
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  // libde265 must be the HEVC decoder in the build; without one there is no
  // bundled HEIC at all (a packaging error, not a file error).
  if (!ensure_heif_init()) return err(status::unsupported_format);
  try {
    auto opened = open_context(bytes);
    if (!opened) return err(opened.error());
    heif_context* hctx = opened.value().get();

    heif_image_handle* raw = nullptr;
    const heif_error e = heif_context_get_primary_image_handle(hctx, &raw);
    handle_ptr handle(raw);
    if (e.code == heif_error_Ok && handle) return decode_handle(handle.get(), ctx);

    // msf1-only files have no primary item: the still is the first frame.
    if (heif_context_has_sequence(hctx)) {
      auto shared = std::make_shared<const std::vector<std::uint8_t>>(bytes.begin(), bytes.end());
      heic_source source(std::move(shared));
      if (auto ok = source.open(); !ok) return err(ok.error());
      canvas_frame frame;
      auto got = source.next(frame, ctx);
      if (!got) return err(got.error());
      if (!got.value()) return err(status::corrupt);
      raster out;
      out.width = source.info().width;
      out.height = source.info().height;
      out.format = format_family::heic;
      out.icc = source.info().icc;
      out.tagged_srgb = source.info().tagged_srgb;
      out.rgba = std::move(frame.rgba);
      return out;
    }
    const status s = map_error(e);
    return err(s == status::ok ? status::corrupt : s);
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
}

result<std::unique_ptr<animation_source>> open_heic_animation(
    std::shared_ptr<const std::vector<std::uint8_t>> bytes) {
  if (!bytes) return err(status::invalid_arg);
  if (probe(*bytes) != format_family::heic) return err(status::unsupported_format);
  if (!ensure_heif_init()) return err(status::unsupported_format);
  try {
    auto source = std::make_unique<heic_source>(std::move(bytes));
    if (auto opened = source->open(); !opened) return err(opened.error());
    return std::unique_ptr<animation_source>(std::move(source));
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
}

result<heic_still_info> inspect_heic(std::span<const std::uint8_t> bytes) {
  if (probe(bytes) != format_family::heic) return err(status::unsupported_format);
  if (!ensure_heif_init()) return err(status::unsupported_format);
  try {
    auto opened = open_context(bytes);
    if (!opened) return err(opened.error());
    heif_context* hctx = opened.value().get();
    heic_still_info info;
    info.sequence = heif_context_has_sequence(hctx) != 0;

    heif_image_handle* raw = nullptr;
    if (heif_context_get_primary_image_handle(hctx, &raw).code != heif_error_Ok || !raw) {
      return err(status::unsupported_format);
    }
    handle_ptr handle(raw);
    const std::uint32_t type =
        heif_item_get_item_type(hctx, heif_image_handle_get_item_id(handle.get()));
    info.hevc = type == fourcc('h', 'v', 'c', '1') || type == fourcc('g', 'r', 'i', 'd');
    info.width = static_cast<std::uint32_t>(std::max(0, heif_image_handle_get_width(handle.get())));
    info.height = static_cast<std::uint32_t>(std::max(0, heif_image_handle_get_height(handle.get())));
    info.luma_bits = static_cast<std::uint32_t>(
        std::max(0, heif_image_handle_get_luma_bits_per_pixel(handle.get())));
    info.has_alpha = heif_image_handle_has_alpha_channel(handle.get()) != 0;
    auto colour = handle_colour(handle.get());
    if (!colour) return err(colour.error());
    info.has_icc = !colour.value().icc.empty();
    if (colour.value().have_nclx) {
      info.hdr = cicp::is_hdr(colour.value().transfer);
      info.srgb_in_effect =
          !info.hdr && cicp::tag_sdr(colour.value().primaries, colour.value().transfer).icc.empty();
    }
    return info;
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
}

}  // namespace mv::codec
