// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// HEIC/HEIF (libheif + libde265), AVIF (libavif + dav1d) and the D3 OS-codec
// probe. Fixtures are tiny generated files in tests/data/{heif,avif}/ (see
// tests/data/README.md); real iPhone samples are fetched, not committed, and
// their tests skip visibly.
#include <catch2/catch_test_macros.hpp>

#include <lcms2.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "codec/cicp.h"
#include "codec/decode.h"
#include "codec/os_decode.h"
#include "corpus.h"
#include "image/colour.h"

using namespace mv::codec;

namespace {

std::filesystem::path data_dir() {
  std::filesystem::path dir = std::filesystem::current_path();
  for (int up = 0; up < 8; ++up) {
    const std::filesystem::path candidate = dir / "tests" / "data";
    if (std::filesystem::is_directory(candidate / "heif")) return candidate;
    if (!dir.has_parent_path() || dir.parent_path() == dir) break;
    dir = dir.parent_path();
  }
  return {};
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::uint8_t> fixture(const char* rel) {
  const std::filesystem::path dir = data_dir();
  REQUIRE_FALSE(dir.empty());
  auto bytes = read_file(dir / rel);
  INFO(rel);
  REQUIRE(bytes.size() > 16);
  return bytes;
}

struct rgb {
  int r, g, b;
};

rgb px(const raster& r, std::uint32_t x, std::uint32_t y) {
  const std::size_t i = (static_cast<std::size_t>(y) * r.width + x) * 4;
  return {r.rgba[i], r.rgba[i + 1], r.rgba[i + 2]};
}

bool near(rgb a, rgb b, int tol) {
  return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol;
}

constexpr rgb kRed{255, 0, 0};
constexpr rgb kGreen{0, 255, 0};
constexpr rgb kBlue{0, 0, 255};

// 64x32 pattern: left half red, right half blue, top-left 8x8 green.
void check_pattern(const raster& r, int tol) {
  REQUIRE(r.width == 64);
  REQUIRE(r.height == 32);
  REQUIRE(r.rgba.size() == 64u * 32u * 4u);
  CHECK(near(px(r, 3, 3), kGreen, tol));
  CHECK(near(px(r, 20, 20), kRed, tol));
  CHECK(near(px(r, 50, 20), kBlue, tol));
}

// EXIF orientation 6 (rotate 90° clockwise to display): 32x64, red on top,
// green in the top-right corner.
void check_rotated_cw(const raster& r, int tol) {
  REQUIRE(r.width == 32);
  REQUIRE(r.height == 64);
  CHECK(near(px(r, 28, 3), kGreen, tol));
  CHECK(near(px(r, 12, 20), kRed, tol));
  CHECK(near(px(r, 12, 50), kBlue, tol));
}

class env_override {
 public:
  env_override(const char* name, const char* value) : name_(name) {
    if (const char* old = std::getenv(name)) {
      had_ = true;
      old_ = old;
    }
    set(value);
  }
  ~env_override() { set(had_ ? old_.c_str() : ""); }
  env_override(const env_override&) = delete;
  env_override& operator=(const env_override&) = delete;

 private:
  void set(const char* value) {
#ifdef _WIN32
    _putenv_s(name_.c_str(), value);
#else
    setenv(name_.c_str(), value, 1);
#endif
  }
  std::string name_;
  std::string old_;
  bool had_ = false;
};

// ICC → sRGB of a fixed colour ramp, for comparing two profiles' effect.
std::vector<std::uint8_t> ramp_through(const std::vector<std::uint8_t>& icc) {
  raster r;
  r.width = 6;
  r.height = 1;
  r.format = format_family::heic;
  r.icc = icc;
  r.rgba = {200, 100, 50, 255, 30, 180, 90, 255, 128, 128, 128, 255,
            240, 20,  20, 255, 20, 60,  220, 255, 250, 250, 10, 255};
  auto shown = mv::image::to_display(std::move(r));
  REQUIRE(shown);
  return shown.value().rgba;
}

std::vector<std::uint8_t> patch_ispe(std::vector<std::uint8_t> bytes, std::uint32_t w,
                                     std::uint32_t h) {
  for (std::size_t i = 0; i + 16 <= bytes.size(); ++i) {
    if (std::memcmp(bytes.data() + i, "ispe", 4) == 0) {
      const std::uint8_t* p = bytes.data() + i + 8;  // after version/flags
      auto put = [&](std::size_t o, std::uint32_t v) {
        bytes[o] = static_cast<std::uint8_t>(v >> 24);
        bytes[o + 1] = static_cast<std::uint8_t>(v >> 16);
        bytes[o + 2] = static_cast<std::uint8_t>(v >> 8);
        bytes[o + 3] = static_cast<std::uint8_t>(v);
      };
      const std::size_t off = static_cast<std::size_t>(p - bytes.data());
      put(off, w);
      put(off + 4, h);
    }
  }
  return bytes;
}

}  // namespace

// --- HEIC -------------------------------------------------------------------

TEST_CASE("HEIC 8-bit sRGB decodes through libheif + libde265", "[codec][heif]") {
  const auto bytes = fixture("heif/srgb_8bit.heic");
  REQUIRE(probe(bytes) == format_family::heic);
  auto r = decode_heic(bytes);
  REQUIRE(r);
  check_pattern(r.value(), 6);
  CHECK(r.value().format == format_family::heic);
  CHECK(r.value().icc.empty());  // nclx sRGB: copied through
  CHECK(r.value().rgba[3] == 255);
  CHECK(mv::image::to_display(std::move(r.value())));
}

TEST_CASE("HEIC irot is applied from the container", "[codec][heif]") {
  auto r = decode_heic(fixture("heif/irot.heic"));
  REQUIRE(r);
  check_rotated_cw(r.value(), 6);
}

TEST_CASE("HEIC 10-bit is rounded to 8 bits, not truncated or wrapped", "[codec][heif]") {
  auto r = decode_heic(fixture("heif/gradient_10bit.heic"));
  REQUIRE(r);
  const raster& img = r.value();
  REQUIRE(img.width == 64);
  REQUIRE(img.height == 32);
  CHECK(near(px(img, 0, 16), rgb{0, 128, 255}, 3));
  CHECK(near(px(img, 63, 16), rgb{255, 128, 0}, 3));
  // Monotonic ramp across the row.
  for (std::uint32_t x = 1; x < 64; ++x) CHECK(px(img, x, 16).r >= px(img, x - 1, 16).r);
}

TEST_CASE("HEIC Display P3, as ICC or as nclx, is converted — never shown as sRGB",
          "[codec][heif]") {
  auto icc = decode_heic(fixture("heif/p3_icc.heic"));
  auto nclx = decode_heic(fixture("heif/p3_nclx.heic"));
  REQUIRE(icc);
  REQUIRE(nclx);
  REQUIRE_FALSE(icc.value().icc.empty());
  REQUIRE_FALSE(nclx.value().icc.empty());  // synthesised from CICP 12/13
  CHECK_FALSE(mv::image::is_srgb_icc(nclx.value().icc));

  // The embedded profile (written by the fixture script) and the synthesised
  // one (cicp.h) are independent implementations of Display P3: same effect.
  const auto via_file = ramp_through(icc.value().icc);
  const auto via_cicp = ramp_through(nclx.value().icc);
  for (std::size_t i = 0; i < via_file.size(); ++i) {
    INFO("sample " << i);
    CHECK(std::abs(int(via_file[i]) - int(via_cicp[i])) <= 2);
  }
  // And the conversion is not the identity the D6 bug would be.
  const std::vector<std::uint8_t> naive = {200, 100, 50, 255, 30, 180, 90, 255};
  int moved = 0;
  for (std::size_t i = 0; i < naive.size(); ++i) moved += std::abs(int(via_cicp[i]) - int(naive[i]));
  CHECK(moved > 20);

  // The pixels themselves are the stored red.
  CHECK(near(px(nclx.value(), 8, 8), kRed, 4));
}

TEST_CASE("HEIC PQ is tone-mapped to SDR with the video shader's curve", "[codec][heif]") {
  auto r = decode_heic(fixture("heif/pq_10bit.heic"));
  REQUIRE(r);
  const raster& img = r.value();
  CHECK(img.icc.empty());
  CHECK(img.intent == transfer_intent::display_referred);  // already mapped
  const rgb white203 = px(img, 2, 8);
  const rgb white1000 = px(img, 13, 8);
  CHECK(std::abs(white203.r - white203.g) <= 2);
  CHECK(std::abs(white203.b - white203.g) <= 2);
  // Reinhard-extended with peak 10000/203: 203 nits lands near linear 0.5.
  CHECK(white203.r >= 175);
  CHECK(white203.r <= 200);
  CHECK(white1000.r > white203.r + 20);
  CHECK(white1000.r < 255);  // highlights roll off, they do not clip
}

TEST_CASE("HEIC: truncated and hostile files fail without crashing", "[codec][heif]") {
  const auto good = fixture("heif/srgb_8bit.heic");
  for (std::size_t len : {std::size_t{12}, std::size_t{40}, good.size() / 3, good.size() / 2,
                          good.size() - 16, good.size() - 1}) {
    INFO("length " << len);
    std::vector<std::uint8_t> cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(len));
    auto r = decode(cut);
    if (r) {
      CHECK(r.value().rgba.size() == std::size_t{r.value().width} * r.value().height * 4);
    }
  }
  std::vector<std::uint8_t> cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(good.size() / 2));
  CHECK_FALSE(decode_heic(cut));

  // Bit flips in the coded payload.
  auto flipped = good;
  for (std::size_t i = flipped.size() / 2; i < flipped.size(); i += 7) flipped[i] ^= 0x5A;
  (void)decode(flipped);

  // Absurd ispe: refused from the header, before any pixel allocation.
  auto huge = patch_ispe(good, 0x7FFFFFFFu, 0x7FFFFFFFu);
  auto r = decode_heic(huge);
  REQUIRE_FALSE(r);
  CHECK((r.error() == mv::status::unsupported_format || r.error() == mv::status::corrupt));
  auto wide = patch_ispe(good, 70000u, 4u);
  CHECK_FALSE(decode_heic(wide));
}

TEST_CASE("a HEIC still is not an animation", "[codec][heif]") {
  const auto bytes = fixture("heif/srgb_8bit.heic");
  auto shared = std::make_shared<const std::vector<std::uint8_t>>(bytes);
  auto anim = open_animation(shared);
  REQUIRE_FALSE(anim);
  CHECK(anim.error() == mv::status::unsupported_format);
}

// --- AVIF -------------------------------------------------------------------

TEST_CASE("AVIF 8-bit sRGB still decodes", "[codec][avif]") {
  const auto bytes = fixture("avif/srgb_8bit.avif");
  REQUIRE(probe(bytes) == format_family::avif);
  auto r = decode(bytes);
  REQUIRE(r);
  check_pattern(r.value(), 6);
  CHECK(r.value().format == format_family::avif);
  CHECK(r.value().icc.empty());
  CHECK(r.value().tagged_srgb);  // explicit CICP 1/13
}

TEST_CASE("AVIF 10-bit decodes to 8 bits", "[codec][avif]") {
  auto r = decode_avif(fixture("avif/gradient_10bit.avif"));
  REQUIRE(r);
  check_pattern(r.value(), 6);
}

TEST_CASE("AVIF irot and imir are applied (libavif leaves them to us)", "[codec][avif]") {
  auto rot = decode_avif(fixture("avif/irot.avif"));
  REQUIRE(rot);
  check_rotated_cw(rot.value(), 12);

  // Orientation 5 = transpose: green stays top-left, red on top.
  auto both = decode_avif(fixture("avif/irot_imir.avif"));
  REQUIRE(both);
  const raster& t = both.value();
  REQUIRE(t.width == 32);
  REQUIRE(t.height == 64);
  CHECK(near(px(t, 3, 3), kGreen, 12));
  CHECK(near(px(t, 20, 20), kRed, 12));
  CHECK(near(px(t, 20, 50), kBlue, 12));

  // Same displayed orientation as libheif gives the HEIC made from the same
  // EXIF orientation.
  auto heic = decode_heic(fixture("heif/irot.heic"));
  REQUIRE(heic);
  CHECK(near(px(heic.value(), 28, 3), px(rot.value(), 28, 3), 16));
}

TEST_CASE("AVIF Display P3 as ICC or CICP converts like the HEIC does", "[codec][avif]") {
  auto icc = decode_avif(fixture("avif/p3_icc.avif"));
  auto nclx = decode_avif(fixture("avif/p3_nclx.avif"));
  REQUIRE(icc);
  REQUIRE(nclx);
  REQUIRE_FALSE(icc.value().icc.empty());
  REQUIRE_FALSE(nclx.value().icc.empty());
  const auto a = ramp_through(icc.value().icc);
  const auto b = ramp_through(nclx.value().icc);
  for (std::size_t i = 0; i < a.size(); ++i) CHECK(std::abs(int(a[i]) - int(b[i])) <= 2);
  CHECK(near(px(nclx.value(), 8, 8), kRed, 6));
}

TEST_CASE("animated AVIF: frame count, browser delays, loops, rewind", "[codec][avif]") {
  const auto bytes = fixture("avif/anim.avif");
  auto frames = decode_animation(bytes);
  REQUIRE(frames);
  const animation_frames& f = frames.value();
  REQUIRE(f.frames.size() == 4);
  CHECK(f.width == 32);
  CHECK(f.height == 16);
  CHECK(f.format == format_family::avif);
  REQUIRE(f.delays_ms.size() == 4);
  CHECK(f.delays_ms[0] == 40);
  CHECK(f.delays_ms[1] == 100);
  CHECK(f.delays_ms[2] == 200);
  CHECK(f.delays_ms[3] == 100);  // a zero delay plays as 100 ms, as in a browser
  // The blue bar moves one 8-pixel slot per frame.
  for (std::size_t i = 0; i < 4; ++i) {
    raster r;
    r.width = 32;
    r.height = 16;
    r.rgba = f.frames[i];
    INFO("frame " << i);
    CHECK(near(px(r, static_cast<std::uint32_t>(i * 8 + 4), 8), kBlue, 24));
    CHECK(near(px(r, static_cast<std::uint32_t>((i + 2) % 4 * 8 + 4), 8), kRed, 24));
  }

  auto shared = std::make_shared<const std::vector<std::uint8_t>>(bytes);
  auto opened = open_animation(shared);
  REQUIRE(opened);
  animation_source& src = *opened.value();
  INFO("loops " << src.info().loops);
  CHECK(src.info().frame_count == 4);
  canvas_frame a, b;
  REQUIRE(src.next(a, nullptr).value());
  REQUIRE(src.next(b, nullptr).value());
  REQUIRE(src.rewind());
  canvas_frame again;
  REQUIRE(src.next(again, nullptr).value());
  CHECK(again.index == 0);
  const bool rewound_same = again.rgba == a.rgba;
  CHECK(rewound_same);

  // The still of an animated AVIF is its first frame.
  auto still = decode_avif(bytes);
  REQUIRE(still);
  const bool still_is_frame0 = still.value().rgba == f.frames[0];
  CHECK(still_is_frame0);
}

TEST_CASE("AVIF: truncated and hostile files fail without crashing", "[codec][avif]") {
  for (const char* name : {"avif/srgb_8bit.avif", "avif/anim.avif"}) {
    const auto good = fixture(name);
    for (std::size_t len = 12; len < good.size(); len += std::max<std::size_t>(1, good.size() / 11)) {
      std::vector<std::uint8_t> cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(len));
      (void)decode(cut);
      auto shared = std::make_shared<const std::vector<std::uint8_t>>(cut);
      if (auto anim = open_animation(shared)) {
        canvas_frame frame;
        for (int i = 0; i < 8; ++i) {
          auto more = anim.value()->next(frame, nullptr);
          if (!more || !more.value()) break;
        }
      }
    }
    std::vector<std::uint8_t> half(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(good.size() / 2));
    CHECK_FALSE(decode_avif(half));
    auto huge = patch_ispe(good, 0x7FFFFFFFu, 0x7FFFFFFFu);
    CHECK_FALSE(decode_avif(huge));
  }
}

// --- CICP → ICC ---------------------------------------------------------------

TEST_CASE("synthesised Display P3 ICC matches LittleCMS's own P3 profile", "[codec][heif][avif]") {
  cicp::chromaticities p3{};
  REQUIRE(cicp::primaries_of(cicp::kPrimariesDisplayP3, p3));
  const auto icc = cicp::matrix_shaper_icc(p3, cicp::curve::srgb, "P3");
  REQUIRE_FALSE(icc.empty());
  cmsHPROFILE ours = cmsOpenProfileFromMem(icc.data(), static_cast<cmsUInt32Number>(icc.size()));
  REQUIRE(ours != nullptr);
  CHECK(cmsIsMatrixShaper(ours));

  cmsCIExyY d65 = {0.3127, 0.3290, 1.0};
  cmsCIExyYTRIPLE prim = {{0.680, 0.320, 1.0}, {0.265, 0.690, 1.0}, {0.150, 0.060, 1.0}};
  cmsFloat64Number params[5] = {2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045};
  cmsToneCurve* curve = cmsBuildParametricToneCurve(nullptr, 4, params);
  cmsToneCurve* curves[3] = {curve, curve, curve};
  cmsHPROFILE ref = cmsCreateRGBProfile(&d65, &prim, curves);
  cmsFreeToneCurve(curve);
  REQUIRE(ref != nullptr);
  for (cmsTagSignature sig : {cmsSigRedColorantTag, cmsSigGreenColorantTag, cmsSigBlueColorantTag}) {
    const auto* a = static_cast<cmsCIEXYZ*>(cmsReadTag(ours, sig));
    const auto* b = static_cast<cmsCIEXYZ*>(cmsReadTag(ref, sig));
    REQUIRE(a);
    REQUIRE(b);
    CHECK(std::abs(a->X - b->X) < 0.002);
    CHECK(std::abs(a->Y - b->Y) < 0.002);
    CHECK(std::abs(a->Z - b->Z) < 0.002);
  }
  cmsCloseProfile(ours);
  cmsCloseProfile(ref);

  // BT.709 + sRGB transfer is not a profile at all.
  CHECK(cicp::tag_sdr(1, 13).icc.empty());
  CHECK(cicp::tag_sdr(2, 2).icc.empty());
  CHECK_FALSE(cicp::tag_sdr(12, 13).icc.empty());
  CHECK_FALSE(cicp::tag_sdr(9, 1).icc.empty());
}

// --- D3 OS-codec probe --------------------------------------------------------

TEST_CASE("the OS codec is only offered HEIC stills it can render identically",
          "[codec][heif][os]") {
  // Never JPEG/AVIF/etc., regardless of what WIC could do.
  const auto avif = fixture("avif/srgb_8bit.avif");
  auto r = try_os_decode(avif);
  REQUIRE_FALSE(r);
  CHECK(r.error() == mv::status::unsupported_format);
  // Display P3 nclx, 10-bit and PQ always take the bundled path.
  for (const char* name : {"heif/p3_nclx.heic", "heif/gradient_10bit.heic", "heif/pq_10bit.heic"}) {
    INFO(name);
    auto os = try_os_decode(fixture(name));
    REQUIRE_FALSE(os);
    CHECK(os.error() == mv::status::unsupported_format);
  }
  // MV_OS_CODEC=0 stands in for a clean VM.
  {
    env_override off("MV_OS_CODEC", "0");
    CHECK_FALSE(os_codec_enabled());
    auto os = try_os_decode(fixture("heif/iphone_like.heic"));
    REQUIRE_FALSE(os);
    CHECK(os.error() == mv::status::unsupported_format);
  }
  CHECK(os_codec_enabled());
}

TEST_CASE("MV_OS_CODEC=0 forces libheif and matches the OS path's pixels", "[codec][heif][os]") {
  const auto bytes = fixture("heif/iphone_like.heic");
  raster bundled;
  {
    env_override off("MV_OS_CODEC", "0");
    auto r = decode(bytes);
    REQUIRE(r);
    bundled = std::move(r.value());
  }
  auto direct = decode_heic(bytes);
  REQUIRE(direct);
  // A bool, not the vectors: under -s Catch2 stringifies both 12 KB buffers.
  const bool same_pixels = direct.value().rgba == bundled.rgba;
  CHECK(same_pixels);
  REQUIRE(bundled.width == 48);  // irot applied: 64x48 stored
  REQUIRE(bundled.height == 64);
  REQUIRE_FALSE(bundled.icc.empty());

  auto os = try_os_decode(bytes);
  if (!os) {
    WARN("OS HEIF/HEVC path not available on this machine (no Store HEVC/HEIF extension) — "
         "the OS-vs-bundled comparison proved NOTHING here; decode() used libheif.");
    return;
  }
  const raster& wic = os.value();
  REQUIRE(wic.width == bundled.width);
  REQUIRE(wic.height == bundled.height);
  const bool same_icc = wic.icc == bundled.icc;
  CHECK(same_icc);
  // 4:2:0 lossy: the two chroma upsampling filters differ, which moves a few
  // samples on hard colour edges a long way. Judge the bulk, not the worst.
  std::vector<int> diffs(wic.rgba.size());
  std::uint64_t total = 0;
  for (std::size_t i = 0; i < wic.rgba.size(); ++i) {
    diffs[i] = std::abs(int(wic.rgba[i]) - int(bundled.rgba[i]));
    total += static_cast<std::uint64_t>(diffs[i]);
  }
  std::sort(diffs.begin(), diffs.end());
  const double mean = static_cast<double>(total) / static_cast<double>(wic.rgba.size());
  const int p99 = diffs[diffs.size() * 99 / 100];
  INFO("mean abs diff " << mean << ", p99 " << p99 << ", worst " << diffs.back());
  CHECK(mean < 3.0);
  CHECK(p99 < 16);
  // Orientation: the green marker is in the same corner on both paths.
  CHECK(near(px(wic, 44, 3), px(bundled, 44, 3), 24));
}

// --- Real-world samples (fetched, never committed) -----------------------------

TEST_CASE("fetched real-world HEIC (libheif example, MIT) decodes", "[codec][heif][corpus]") {
  MV_REQUIRE_CLIP(path, "heif/libheif-example.heic");
  const auto bytes = read_file(path);
  env_override off("MV_OS_CODEC", "0");
  auto r = decode(bytes);
  REQUIRE(r);
  CHECK(r.value().width > 0);
  CHECK(mv::image::to_display(std::move(r.value())));
}
