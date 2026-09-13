// SPDX-License-Identifier: GPL-2.0-or-later
#include "codec/apng.h"

#include <array>
#include <cstring>
#include <new>

#include "codec/decode.h"
#include "codec/format.h"

namespace mv::codec {
namespace {

constexpr std::uint8_t kSignature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
constexpr std::uint32_t kMaxFrames = 100000;

std::uint32_t be32(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

std::uint16_t be16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

bool is_type(const std::uint8_t* t, const char* name) noexcept { return std::memcmp(t, name, 4) == 0; }

// Chunks a frame needs to decode by itself. Anything else (text, time, EXIF)
// stays out of the per-frame PNG.
bool shared_type(const std::uint8_t* t) noexcept {
  return is_type(t, "PLTE") || is_type(t, "tRNS") || is_type(t, "iCCP") || is_type(t, "sRGB") ||
         is_type(t, "gAMA") || is_type(t, "cHRM") || is_type(t, "sBIT");
}

constexpr std::array<std::uint32_t, 256> kCrcTable = [] {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t n = 0; n < 256; ++n) {
    std::uint32_t c = n;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    table[n] = c;
  }
  return table;
}();

std::uint32_t crc32(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) noexcept {
  std::uint32_t c = 0xFFFFFFFFu;
  for (const std::uint8_t byte : a) c = kCrcTable[(c ^ byte) & 0xFF] ^ (c >> 8);
  for (const std::uint8_t byte : b) c = kCrcTable[(c ^ byte) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  out.push_back(static_cast<std::uint8_t>(v >> 24));
  out.push_back(static_cast<std::uint8_t>(v >> 16));
  out.push_back(static_cast<std::uint8_t>(v >> 8));
  out.push_back(static_cast<std::uint8_t>(v));
}

void put_chunk(std::vector<std::uint8_t>& out, const char* type,
               std::span<const std::uint8_t> data) {
  put32(out, static_cast<std::uint32_t>(data.size()));
  const auto* t = reinterpret_cast<const std::uint8_t*>(type);
  out.insert(out.end(), t, t + 4);
  out.insert(out.end(), data.begin(), data.end());
  put32(out, crc32(std::span<const std::uint8_t>(t, 4), data));
}

class apng_source final : public animation_source {
 public:
  apng_source(std::span<const std::uint8_t> bytes, std::shared_ptr<const void> keepalive) noexcept
      : bytes_(bytes), keepalive_(std::move(keepalive)) {}

  [[nodiscard]] expected open() {
    auto parsed = parse_apng(bytes_);
    if (!parsed) return err(parsed.error());
    parsed_ = std::move(parsed).value();
    info_.width = parsed_.width;
    info_.height = parsed_.height;
    info_.loops = parsed_.plays;
    info_.frame_count = static_cast<std::uint32_t>(parsed_.frames.size());
    info_.format = format_family::png;
    return rewind();
  }

  [[nodiscard]] const animation_info& info() const noexcept override { return info_; }

  [[nodiscard]] result<bool> next(canvas_frame& out, const job_context* ctx) override {
    if (next_index_ >= parsed_.frames.size()) return false;
    if (ctx && ctx->cancelled()) return err(status::cancelled);
    try {
      const apng_frame& frame = parsed_.frames[next_index_];
      auto png = apng_frame_png(parsed_, frame);
      if (!png) return err(png.error());
      auto decoded = decode_png(png.value(), ctx);
      if (!decoded) {
        return err(decoded.error() == status::cancelled ? status::cancelled : status::corrupt);
      }
      const raster& r = decoded.value();
      if (r.width != frame.region.width || r.height != frame.region.height) {
        return err(status::corrupt);
      }
      if (next_index_ == 0) {
        info_.icc = r.icc;
        info_.tagged_srgb = r.tagged_srgb;
      }
      if (!canvas_.draw(frame.region, r.rgba)) return err(status::corrupt);
      out.rgba.assign(canvas_.pixels().begin(), canvas_.pixels().end());
      out.delay_ms = frame.delay_ms;
      out.index = static_cast<std::uint32_t>(next_index_++);
      return true;
    } catch (const std::bad_alloc&) {
      return err(status::out_of_memory);
    }
  }

  [[nodiscard]] expected rewind() override {
    if (!canvas_.reset(info_.width, info_.height)) return err(status::out_of_memory);
    next_index_ = 0;
    return {};
  }

 private:
  std::span<const std::uint8_t> bytes_;
  std::shared_ptr<const void> keepalive_;
  apng_info parsed_{};  // spans into bytes_
  animation_info info_{};
  compositor canvas_;
  std::size_t next_index_ = 0;
};

}  // namespace

result<apng_info> parse_apng(std::span<const std::uint8_t> png) {
  if (png.size() < 8 || std::memcmp(png.data(), kSignature, 8) != 0) {
    return err(status::unsupported_format);
  }
  try {
    apng_info info;
    bool seen_ihdr = false;
    bool seen_actl = false;
    bool seen_idat = false;
    std::uint32_t declared_frames = 0;
    std::uint32_t next_sequence = 0;
    // The IDAT data belongs to the most recent frame when that frame's fcTL
    // came before IDAT (the default image is frame 0).
    bool idat_is_frame = false;

    std::size_t pos = 8;
    while (pos + 12 <= png.size()) {
      const std::uint32_t len = be32(png.data() + pos);
      if (len > png.size() - pos - 12) return err(status::corrupt);
      const std::uint8_t* type = png.data() + pos + 4;
      const std::span<const std::uint8_t> data = png.subspan(pos + 8, len);
      const std::span<const std::uint8_t> whole =
          png.subspan(pos, static_cast<std::size_t>(len) + 12);
      pos += static_cast<std::size_t>(len) + 12;

      if (!seen_ihdr) {
        if (!is_type(type, "IHDR") || len != 13) return err(status::corrupt);
        info.ihdr = data;
        info.width = be32(data.data());
        info.height = be32(data.data() + 4);
        if (info.width == 0 || info.height == 0) return err(status::corrupt);
        seen_ihdr = true;
        continue;
      }

      if (is_type(type, "acTL")) {
        if (len != 8 || seen_actl || seen_idat) return err(status::corrupt);
        declared_frames = be32(data.data());
        info.plays = be32(data.data() + 4);
        if (declared_frames == 0 || declared_frames > kMaxFrames) return err(status::corrupt);
        seen_actl = true;
      } else if (is_type(type, "fcTL")) {
        if (!seen_actl || len != 26) return err(status::corrupt);
        if (be32(data.data()) != next_sequence++) return err(status::corrupt);
        apng_frame frame;
        frame.region.width = be32(data.data() + 4);
        frame.region.height = be32(data.data() + 8);
        frame.region.x = be32(data.data() + 12);
        frame.region.y = be32(data.data() + 16);
        const std::uint8_t dispose = data[24];
        const std::uint8_t blend = data[25];
        if (frame.region.width == 0 || frame.region.height == 0 || dispose > 2 || blend > 1 ||
            static_cast<std::uint64_t>(frame.region.x) + frame.region.width > info.width ||
            static_cast<std::uint64_t>(frame.region.y) + frame.region.height > info.height) {
          return err(status::corrupt);
        }
        // A frame before IDAT *is* the default image, so it must cover it.
        if (!seen_idat &&
            (frame.region.x != 0 || frame.region.y != 0 || frame.region.width != info.width ||
             frame.region.height != info.height)) {
          return err(status::corrupt);
        }
        frame.region.dispose = static_cast<dispose_op>(dispose);
        frame.region.blend = static_cast<blend_op>(blend);
        frame.delay_ms = apng_delay_ms(be16(data.data() + 20), be16(data.data() + 22));
        if (info.frames.size() >= declared_frames) return err(status::corrupt);
        idat_is_frame = !seen_idat;
        info.frames.push_back(std::move(frame));
      } else if (is_type(type, "IDAT")) {
        seen_idat = true;
        if (seen_actl && idat_is_frame && !info.frames.empty()) {
          info.frames.back().data.push_back(data);
        }
      } else if (is_type(type, "fdAT")) {
        // fdAT must follow an fcTL of its own, after IDAT.
        if (!(seen_actl && seen_idat && len >= 4 && !info.frames.empty() && !idat_is_frame)) {
          return err(status::corrupt);
        }
        if (be32(data.data()) != next_sequence++) return err(status::corrupt);
        info.frames.back().data.push_back(data.subspan(4));
      } else if (is_type(type, "IEND")) {
        break;
      } else if (!seen_idat && shared_type(type)) {
        info.shared_chunks.push_back(whole);
      }

      // After IDAT, the next fcTL starts a frame that is not the default image.
      if (is_type(type, "IDAT")) idat_is_frame = idat_is_frame && info.frames.size() == 1;
      if (is_type(type, "fcTL") && seen_idat) idat_is_frame = false;
    }

    if (!seen_ihdr) return err(status::corrupt);
    if (!seen_actl) return err(status::unsupported_format);  // a still PNG
    if (!seen_idat || info.frames.size() != declared_frames) return err(status::corrupt);
    for (const auto& frame : info.frames) {
      if (frame.data.empty()) return err(status::corrupt);
    }
    return info;
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
}

result<std::vector<std::uint8_t>> apng_frame_png(const apng_info& info, const apng_frame& frame) {
  if (info.ihdr.size() != 13 || frame.data.empty()) return err(status::invalid_arg);
  try {
    std::size_t data_bytes = 0;
    for (const auto& d : frame.data) data_bytes += d.size();
    std::vector<std::uint8_t> out;
    out.reserve(8 + 25 + data_bytes + 12 + 12 + 64 * info.shared_chunks.size());
    out.insert(out.end(), kSignature, kSignature + 8);

    std::array<std::uint8_t, 13> ihdr{};
    std::memcpy(ihdr.data(), info.ihdr.data(), 13);
    const auto set32 = [&ihdr](std::size_t at, std::uint32_t v) {
      ihdr[at] = static_cast<std::uint8_t>(v >> 24);
      ihdr[at + 1] = static_cast<std::uint8_t>(v >> 16);
      ihdr[at + 2] = static_cast<std::uint8_t>(v >> 8);
      ihdr[at + 3] = static_cast<std::uint8_t>(v);
    };
    set32(0, frame.region.width);
    set32(4, frame.region.height);
    put_chunk(out, "IHDR", ihdr);

    for (const auto& chunk : info.shared_chunks) out.insert(out.end(), chunk.begin(), chunk.end());

    std::vector<std::uint8_t> idat;
    idat.reserve(data_bytes);
    for (const auto& d : frame.data) idat.insert(idat.end(), d.begin(), d.end());
    put_chunk(out, "IDAT", idat);
    put_chunk(out, "IEND", {});
    return out;
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
}

result<std::unique_ptr<animation_source>> open_apng_animation(
    std::shared_ptr<const std::vector<std::uint8_t>> bytes) {
  if (!bytes) return err(status::invalid_arg);
  if (probe(*bytes) != format_family::png) return err(status::unsupported_format);
  try {
    const std::span<const std::uint8_t> view(*bytes);
    auto source = std::make_unique<apng_source>(view, std::move(bytes));
    if (auto opened = source->open(); !opened) return err(opened.error());
    if (source->info().frame_count < 2) return err(status::unsupported_format);  // a still
    return std::unique_ptr<animation_source>(std::move(source));
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
}

}  // namespace mv::codec
