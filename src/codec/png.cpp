// SPDX-License-Identifier: GPL-2.0-or-later
#include "codec/decode.h"

#include <spng.h>

namespace mv::codec {
namespace {

constexpr std::uint32_t kMaxDim = 65535;
constexpr std::uint64_t kMaxPixels = 256ull * 1000ull * 1000ull;

struct spng_guard {
  spng_ctx* ctx = nullptr;
  explicit spng_guard(spng_ctx* c) : ctx(c) {}
  ~spng_guard() {
    if (ctx) spng_ctx_free(ctx);
  }
  spng_guard(const spng_guard&) = delete;
  spng_guard& operator=(const spng_guard&) = delete;
};

}  // namespace

result<raster> decode_png(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  if (probe(bytes) != format_family::png) return err(status::unsupported_format);
  if (ctx && ctx->cancelled()) return err(status::cancelled);

  spng_guard g(spng_ctx_new(0));
  if (!g.ctx) return err(status::out_of_memory);

  if (spng_set_png_buffer(g.ctx, bytes.data(), bytes.size()) != 0) {
    return err(status::corrupt);
  }

  spng_ihdr ihdr{};
  if (spng_get_ihdr(g.ctx, &ihdr) != 0) return err(status::corrupt);
  if (ihdr.width == 0 || ihdr.height == 0 || ihdr.width > kMaxDim || ihdr.height > kMaxDim ||
      static_cast<std::uint64_t>(ihdr.width) * ihdr.height > kMaxPixels) {
    return err(status::unsupported_format);
  }

  raster out;
  out.format = format_family::png;
  out.intent = transfer_intent::display_referred;
  out.width = ihdr.width;
  out.height = ihdr.height;

  spng_iccp iccp{};
  if (spng_get_iccp(g.ctx, &iccp) == 0 && iccp.profile && iccp.profile_len > 0) {
    const auto* profile = reinterpret_cast<const std::uint8_t*>(iccp.profile);
    out.icc.assign(profile, profile + iccp.profile_len);
  } else {
    unsigned char rendering_intent = 0;
    if (spng_get_srgb(g.ctx, &rendering_intent) == 0) out.tagged_srgb = true;
  }

  std::size_t out_size = 0;
  if (spng_decoded_image_size(g.ctx, SPNG_FMT_RGBA8, &out_size) != 0) {
    return err(status::corrupt);
  }

  try {
    out.rgba.resize(out_size);
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }

  if (ctx && ctx->cancelled()) return err(status::cancelled);

  if (spng_decode_image(g.ctx, nullptr, 0, SPNG_FMT_RGBA8, SPNG_DECODE_PROGRESSIVE) != 0) {
    return err(status::corrupt);
  }
  const std::size_t row_bytes = static_cast<std::size_t>(ihdr.width) * 4;
  for (std::uint32_t y = 0; y < ihdr.height; ++y) {
    if (ctx && ctx->cancelled()) return err(status::cancelled);
    const int rc = spng_decode_row(g.ctx, out.rgba.data() + static_cast<std::size_t>(y) * row_bytes,
                                   row_bytes);
    if (rc == SPNG_EOI) break;
    if (rc != 0) return err(status::corrupt);
  }
  return out;
}

}  // namespace mv::codec
