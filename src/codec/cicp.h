// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// CICP (ITU-T H.273) colour descriptions — a HEIF `nclx` box, an AV1 sequence
// header — turned into what the colour stage already understands (plan/04
// "Color", D6):
//
//   * SDR with BT.709 / unspecified primaries and an sRGB-like curve: no ICC,
//     the display path copies it through.
//   * SDR with any other primaries or curve (Display P3 above all — every
//     modern phone): a synthesised ICC v4 matrix/shaper profile, so LittleCMS
//     converts it. Treating P3 as sRGB is the D6 bug.
//   * PQ / HLG (HDR stills): tone-mapped to SDR sRGB here, with the same curves
//     and constants as the video shader (gfx/video_blit.cpp), because the
//     colour stage has no scene-referred path in v1 (image/colour.cpp refuses
//     `scene_referred`). HDR *output* is v1.1.
//
// Shared by heif.cpp and avif.cpp. Portable (D9): no OS headers.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace mv::codec::cicp {

// H.273 codes named where the decoders branch on them.
inline constexpr std::uint16_t kPrimariesBt709 = 1;
inline constexpr std::uint16_t kPrimariesUnspecified = 2;
inline constexpr std::uint16_t kPrimariesBt2020 = 9;
inline constexpr std::uint16_t kPrimariesDisplayP3 = 12;
inline constexpr std::uint16_t kTransferSrgb = 13;
inline constexpr std::uint16_t kTransferPq = 16;
inline constexpr std::uint16_t kTransferHlg = 18;

enum class curve : std::uint8_t { srgb, gamma22, gamma28, linear, pq, hlg };

// TransferCharacteristics → curve. 13 (sRGB), 2 (unspecified) and the SDR
// camera OETFs (1 BT.709, 6 BT.601, 14/15 BT.2020) are shown with the sRGB
// curve: a still image carrying a video OETF code was, in practice, encoded
// from sRGB pixels (ffmpeg / libavif defaults), and that is how browsers show
// it. Codes with no still-image meaning (7, 9–12, 17) fall back the same way.
[[nodiscard]] constexpr curve classify_transfer(std::uint16_t code) noexcept {
  switch (code) {
    case 4: return curve::gamma22;
    case 5: return curve::gamma28;
    case 8: return curve::linear;
    case kTransferPq: return curve::pq;
    case kTransferHlg: return curve::hlg;
    default: return curve::srgb;
  }
}

[[nodiscard]] constexpr bool is_hdr(std::uint16_t transfer) noexcept {
  const curve c = classify_transfer(transfer);
  return c == curve::pq || c == curve::hlg;
}

struct chromaticities {
  double rx, ry, gx, gy, bx, by, wx, wy;
};

// ColourPrimaries → CIE xy. False for unspecified / reserved codes and for
// 10 (CIE XYZ, not an RGB space).
[[nodiscard]] constexpr bool primaries_of(std::uint16_t code, chromaticities& out) noexcept {
  constexpr double d65x = 0.3127, d65y = 0.3290;
  constexpr double illum_c_x = 0.310, illum_c_y = 0.316;
  switch (code) {
    case 1: out = {0.640, 0.330, 0.300, 0.600, 0.150, 0.060, d65x, d65y}; return true;
    case 4: out = {0.670, 0.330, 0.210, 0.710, 0.140, 0.080, illum_c_x, illum_c_y}; return true;
    case 5: out = {0.640, 0.330, 0.290, 0.600, 0.150, 0.060, d65x, d65y}; return true;
    case 6:
    case 7: out = {0.630, 0.340, 0.310, 0.595, 0.155, 0.070, d65x, d65y}; return true;
    case 8: out = {0.681, 0.319, 0.243, 0.692, 0.145, 0.049, illum_c_x, illum_c_y}; return true;
    case 9: out = {0.708, 0.292, 0.170, 0.797, 0.131, 0.046, d65x, d65y}; return true;
    case 11: out = {0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.314, 0.351}; return true;
    case 12: out = {0.680, 0.320, 0.265, 0.690, 0.150, 0.060, d65x, d65y}; return true;
    case 22: out = {0.630, 0.340, 0.295, 0.605, 0.155, 0.077, d65x, d65y}; return true;
    default: return false;
  }
}

// --- 3x3 matrices (row-major) ------------------------------------------------

using mat3 = std::array<double, 9>;

[[nodiscard]] inline mat3 mul(const mat3& a, const mat3& b) noexcept {
  mat3 r{};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      double s = 0;
      for (int k = 0; k < 3; ++k) s += a[i * 3 + k] * b[k * 3 + j];
      r[i * 3 + j] = s;
    }
  }
  return r;
}

[[nodiscard]] inline bool invert(const mat3& m, mat3& out) noexcept {
  const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7],
               i = m[8];
  const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  if (!(std::fabs(det) > 1e-12)) return false;
  out = {(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det,
         (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det,
         (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det};
  return true;
}

[[nodiscard]] inline std::array<double, 3> xy_to_xyz(double x, double y) noexcept {
  return {x / y, 1.0, (1.0 - x - y) / y};
}

// Linear RGB → XYZ (the primaries' own white).
[[nodiscard]] inline bool rgb_to_xyz(const chromaticities& c, mat3& out) noexcept {
  const auto r = xy_to_xyz(c.rx, c.ry);
  const auto g = xy_to_xyz(c.gx, c.gy);
  const auto b = xy_to_xyz(c.bx, c.by);
  const auto w = xy_to_xyz(c.wx, c.wy);
  const mat3 m = {r[0], g[0], b[0], r[1], g[1], b[1], r[2], g[2], b[2]};
  mat3 inv{};
  if (!invert(m, inv)) return false;
  const double sr = inv[0] * w[0] + inv[1] * w[1] + inv[2] * w[2];
  const double sg = inv[3] * w[0] + inv[4] * w[1] + inv[5] * w[2];
  const double sb = inv[6] * w[0] + inv[7] * w[1] + inv[8] * w[2];
  out = {m[0] * sr, m[1] * sg, m[2] * sb, m[3] * sr, m[4] * sg, m[5] * sb,
         m[6] * sr, m[7] * sg, m[8] * sb};
  return true;
}

// Bradford chromatic adaptation from `src` XYZ white to `dst` XYZ white.
[[nodiscard]] inline mat3 bradford(const std::array<double, 3>& src,
                                   const std::array<double, 3>& dst) noexcept {
  constexpr mat3 kB = {0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296};
  constexpr mat3 kBinv = {0.9869929, -0.1470543, 0.1599627, 0.4323053, 0.5183603,
                          0.0492912, -0.0085287, 0.0400428, 0.9684867};
  auto cone = [&](const std::array<double, 3>& v) {
    return std::array<double, 3>{kB[0] * v[0] + kB[1] * v[1] + kB[2] * v[2],
                                 kB[3] * v[0] + kB[4] * v[1] + kB[5] * v[2],
                                 kB[6] * v[0] + kB[7] * v[1] + kB[8] * v[2]};
  };
  const auto s = cone(src);
  const auto d = cone(dst);
  const mat3 scale = {d[0] / s[0], 0, 0, 0, d[1] / s[1], 0, 0, 0, d[2] / s[2]};
  return mul(kBinv, mul(scale, kB));
}

inline constexpr std::array<double, 3> kD50 = {0.9642, 1.0, 0.8249};

// --- ICC v4 matrix/shaper writer -----------------------------------------------

namespace detail {

inline void put_u32(std::vector<std::uint8_t>& v, std::uint32_t x) {
  v.push_back(static_cast<std::uint8_t>(x >> 24));
  v.push_back(static_cast<std::uint8_t>(x >> 16));
  v.push_back(static_cast<std::uint8_t>(x >> 8));
  v.push_back(static_cast<std::uint8_t>(x));
}

inline void put_u16(std::vector<std::uint8_t>& v, std::uint16_t x) {
  v.push_back(static_cast<std::uint8_t>(x >> 8));
  v.push_back(static_cast<std::uint8_t>(x));
}

inline void put_sig(std::vector<std::uint8_t>& v, std::string_view four) {
  for (std::size_t i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(four[i]));
}

inline void put_s15(std::vector<std::uint8_t>& v, double x) {
  put_u32(v, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(x * 65536.0))));
}

inline std::vector<std::uint8_t> xyz_tag(double x, double y, double z) {
  std::vector<std::uint8_t> t;
  put_sig(t, "XYZ ");
  put_u32(t, 0);
  put_s15(t, x);
  put_s15(t, y);
  put_s15(t, z);
  return t;
}

inline std::vector<std::uint8_t> mluc_tag(std::string_view ascii) {
  std::vector<std::uint8_t> t;
  put_sig(t, "mluc");
  put_u32(t, 0);
  put_u32(t, 1);   // one record
  put_u32(t, 12);  // record size
  put_sig(t, "enUS");
  put_u32(t, static_cast<std::uint32_t>(ascii.size() * 2));
  put_u32(t, 28);  // offset of the string from the tag start
  for (char ch : ascii) put_u16(t, static_cast<std::uint16_t>(static_cast<unsigned char>(ch)));
  return t;
}

inline std::vector<std::uint8_t> para_tag(curve c) {
  std::vector<std::uint8_t> t;
  put_sig(t, "para");
  put_u32(t, 0);
  if (c == curve::srgb) {
    put_u16(t, 3);  // Y = (aX+b)^g for X >= d, else cX
    put_u16(t, 0);
    put_s15(t, 2.4);
    put_s15(t, 1.0 / 1.055);
    put_s15(t, 0.055 / 1.055);
    put_s15(t, 1.0 / 12.92);
    put_s15(t, 0.04045);
  } else {
    put_u16(t, 0);  // Y = X^g
    put_u16(t, 0);
    put_s15(t, c == curve::gamma22 ? 2.2 : c == curve::gamma28 ? 2.8 : 1.0);
  }
  return t;
}

}  // namespace detail

// A display-class RGB matrix/shaper profile for `c` with one SDR curve on all
// three channels. Colorants are Bradford-adapted to the D50 PCS and the
// adaptation is recorded in `chad`, as ICC v4 requires. Empty if the
// primaries are degenerate or `trc` is an HDR curve (no ICC shaper for those).
[[nodiscard]] inline std::vector<std::uint8_t> matrix_shaper_icc(const chromaticities& c, curve trc,
                                                                 std::string_view description) {
  if (trc == curve::pq || trc == curve::hlg) return {};
  mat3 to_xyz{};
  if (!rgb_to_xyz(c, to_xyz)) return {};
  const mat3 chad = bradford(xy_to_xyz(c.wx, c.wy), kD50);
  const mat3 m = mul(chad, to_xyz);

  struct tag {
    std::string_view sig;
    std::vector<std::uint8_t> body;
  };
  const std::vector<std::uint8_t> shaper = detail::para_tag(trc);
  std::vector<std::uint8_t> chad_body;
  detail::put_sig(chad_body, "sf32");
  detail::put_u32(chad_body, 0);
  for (double v : chad) detail::put_s15(chad_body, v);

  const tag tags[] = {
      {"desc", detail::mluc_tag(description)},
      {"cprt", detail::mluc_tag("No copyright, use freely")},
      {"wtpt", detail::xyz_tag(kD50[0], kD50[1], kD50[2])},
      {"rXYZ", detail::xyz_tag(m[0], m[3], m[6])},
      {"gXYZ", detail::xyz_tag(m[1], m[4], m[7])},
      {"bXYZ", detail::xyz_tag(m[2], m[5], m[8])},
      {"rTRC", shaper},
      {"gTRC", shaper},
      {"bTRC", shaper},
      {"chad", chad_body},
  };
  constexpr std::uint32_t kHeader = 128;
  const std::uint32_t count = static_cast<std::uint32_t>(std::size(tags));
  std::uint32_t offset = kHeader + 4 + 12 * count;

  std::vector<std::uint8_t> table;
  std::vector<std::uint8_t> data;
  detail::put_u32(table, count);
  for (const tag& t : tags) {
    detail::put_sig(table, t.sig);
    detail::put_u32(table, offset + static_cast<std::uint32_t>(data.size()));
    detail::put_u32(table, static_cast<std::uint32_t>(t.body.size()));
    data.insert(data.end(), t.body.begin(), t.body.end());
    while (data.size() % 4) data.push_back(0);
  }

  std::vector<std::uint8_t> icc;
  icc.reserve(kHeader + table.size() + data.size());
  detail::put_u32(icc, static_cast<std::uint32_t>(kHeader + table.size() + data.size()));
  detail::put_u32(icc, 0);           // preferred CMM
  detail::put_u32(icc, 0x04300000);  // v4.3
  detail::put_sig(icc, "mntr");
  detail::put_sig(icc, "RGB ");
  detail::put_sig(icc, "XYZ ");
  for (int i = 0; i < 12; ++i) icc.push_back(0);  // date
  detail::put_sig(icc, "acsp");
  for (int i = 0; i < 24; ++i) icc.push_back(0);  // platform, flags, manufacturer, model
  for (int i = 0; i < 8; ++i) icc.push_back(0);   // attributes
  detail::put_u32(icc, 0);                        // perceptual
  detail::put_s15(icc, kD50[0]);
  detail::put_s15(icc, kD50[1]);
  detail::put_s15(icc, kD50[2]);
  icc.resize(kHeader, 0);  // creator + profile ID (zero = not computed) + reserved
  icc.insert(icc.end(), table.begin(), table.end());
  icc.insert(icc.end(), data.begin(), data.end());
  return icc;
}

// --- SDR tagging -------------------------------------------------------------

struct sdr_tag {
  std::vector<std::uint8_t> icc;  // empty: sRGB in effect
  bool tagged_srgb = false;       // explicitly BT.709 primaries + sRGB transfer
};

// Colour tag for an SDR CICP description. Callers check is_hdr() first.
[[nodiscard]] inline sdr_tag tag_sdr(std::uint16_t primaries, std::uint16_t transfer) {
  const curve trc = classify_transfer(transfer);
  chromaticities c{};
  const bool known = primaries_of(primaries, c);
  sdr_tag out;
  if ((!known || primaries == kPrimariesBt709) && trc == curve::srgb) {
    out.tagged_srgb = primaries == kPrimariesBt709 && transfer == kTransferSrgb;
    return out;
  }
  if (!known) (void)primaries_of(kPrimariesBt709, c);  // unspecified primaries, non-sRGB curve
  out.icc = matrix_shaper_icc(c, trc, primaries == kPrimariesDisplayP3 ? "Display P3 (CICP)"
                                                                        : "CICP RGB");
  return out;
}

// --- HDR (PQ / HLG) → SDR sRGB -----------------------------------------------

// The HLG and PQ branches of gfx/video_blit.cpp's pixel shader, on the CPU:
// EOTF (PQ) or inverse OETF + OOTF (HLG), reference white 203 nits (BT.2408),
// source primaries → BT.709, Reinhard-extended on luminance, clip, sRGB
// encode. The output is display-referred 8-bit sRGB — an HDR still is shown
// the way the same frame of an HDR clip is.
class hdr_to_sdr {
 public:
  // `bits` is the sample code range (9..16). Unknown primaries on an HDR
  // transfer are taken as BT.2020, which is what PQ/HLG content uses.
  [[nodiscard]] bool init(std::uint16_t primaries, std::uint16_t transfer, std::uint32_t bits) {
    if (bits < 8 || bits > 16) return false;
    const curve trc = classify_transfer(transfer);
    if (trc != curve::pq && trc != curve::hlg) return false;
    hlg_ = trc == curve::hlg;
    max_code_ = (1u << bits) - 1u;
    peak_ = hlg_ ? 1000.0f / 203.0f : 10000.0f / 203.0f;

    chromaticities src{};
    if (!primaries_of(primaries, src)) (void)primaries_of(kPrimariesBt2020, src);
    chromaticities bt709{};
    (void)primaries_of(kPrimariesBt709, bt709);
    mat3 src_xyz{}, dst_xyz{}, dst_inv{};
    if (!rgb_to_xyz(src, src_xyz) || !rgb_to_xyz(bt709, dst_xyz) || !invert(dst_xyz, dst_inv)) {
      return false;
    }
    const mat3 adapt = bradford(xy_to_xyz(src.wx, src.wy), xy_to_xyz(bt709.wx, bt709.wy));
    const mat3 m = mul(dst_inv, mul(adapt, src_xyz));
    for (std::size_t i = 0; i < 9; ++i) to709_[i] = static_cast<float>(m[i]);

    eotf_.resize(static_cast<std::size_t>(max_code_) + 1);
    for (std::uint32_t code = 0; code <= max_code_; ++code) {
      const double e = static_cast<double>(code) / max_code_;
      eotf_[code] = static_cast<float>(hlg_ ? inverse_oetf_hlg(e) : eotf_pq(e) * (10000.0 / 203.0));
    }
    encode_.resize(kEncodeSize);
    for (std::size_t i = 0; i < kEncodeSize; ++i) {
      const double v = static_cast<double>(i) / (kEncodeSize - 1);
      const double s = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
      encode_[i] = static_cast<std::uint8_t>(std::clamp(std::lround(s * 255.0), 0L, 255L));
    }
    return true;
  }

  // `in` is `pixels` RGBA samples in [0, 2^bits - 1]; `out` receives RGBA8.
  void row(const std::uint16_t* in, std::size_t pixels, std::uint8_t* out) const noexcept {
    const float w2 = peak_ * peak_;
    for (std::size_t p = 0; p < pixels; ++p, in += 4, out += 4) {
      float r = eotf_[std::min<std::uint32_t>(in[0], max_code_)];
      float g = eotf_[std::min<std::uint32_t>(in[1], max_code_)];
      float b = eotf_[std::min<std::uint32_t>(in[2], max_code_)];
      if (hlg_) {
        // OOTF, gamma 1.2 for a 1000-nit reference display, on BT.2020 luma.
        const float ys = std::max(0.2627f * r + 0.6780f * g + 0.0593f * b, 1e-6f);
        const float k = std::pow(ys, 0.2f) * (1000.0f / 203.0f);
        r *= k;
        g *= k;
        b *= k;
      }
      const float lr = to709_[0] * r + to709_[1] * g + to709_[2] * b;
      const float lg = to709_[3] * r + to709_[4] * g + to709_[5] * b;
      const float lb = to709_[6] * r + to709_[7] * g + to709_[8] * b;
      const float l = std::max(0.2126f * lr + 0.7152f * lg + 0.0722f * lb, 1e-6f);
      const float scale = (l * (1.0f + l / w2) / (1.0f + l)) / l;
      out[0] = encode(lr * scale);
      out[1] = encode(lg * scale);
      out[2] = encode(lb * scale);
      out[3] = static_cast<std::uint8_t>(
          (std::min<std::uint32_t>(in[3], max_code_) * 255u + max_code_ / 2) / max_code_);
    }
  }

 private:
  static constexpr std::size_t kEncodeSize = 65536;

  [[nodiscard]] std::uint8_t encode(float linear) const noexcept {
    const float v = std::clamp(linear, 0.0f, 1.0f);
    return encode_[static_cast<std::size_t>(v * static_cast<float>(kEncodeSize - 1) + 0.5f)];
  }

  [[nodiscard]] static double eotf_pq(double e) noexcept {
    constexpr double m1 = 0.1593017578125, m2 = 78.84375;
    constexpr double c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const double p = std::pow(std::max(e, 0.0), 1.0 / m2);
    const double num = std::max(p - c1, 0.0);
    const double den = std::max(c2 - c3 * p, 1e-6);
    return std::pow(num / den, 1.0 / m1);
  }

  [[nodiscard]] static double inverse_oetf_hlg(double e) noexcept {
    constexpr double a = 0.17883277, b = 0.28466892, c = 0.55991073;
    return e <= 0.5 ? (e * e) / 3.0 : (std::exp((e - c) / a) + b) / 12.0;
  }

  std::vector<float> eotf_;
  std::vector<std::uint8_t> encode_;
  std::array<float, 9> to709_{};
  float peak_ = 1.0f;
  std::uint32_t max_code_ = 255;
  bool hlg_ = false;
};

// High-bit-depth SDR samples → 8-bit, rounding (not truncating) the rescale.
inline void samples_to_8bit(const std::uint16_t* in, std::size_t samples, std::uint32_t bits,
                            std::uint8_t* out) noexcept {
  const std::uint32_t max_code = (1u << bits) - 1u;
  for (std::size_t i = 0; i < samples; ++i) {
    out[i] = static_cast<std::uint8_t>(
        (std::min<std::uint32_t>(in[i], max_code) * 255u + max_code / 2) / max_code);
  }
}

}  // namespace mv::codec::cicp
