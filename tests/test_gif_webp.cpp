// SPDX-License-Identifier: GPL-2.0-or-later
// GIF and WebP decode, still and animated. Fixtures are encoded in the test
// with giflib's and libwebp's own encoders, so no binary corpus lives in git.
#include <catch2/catch_test_macros.hpp>

#include <gif_lib.h>
#include <lcms2.h>
#include <webp/encode.h>
#include <webp/mux.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "codec/decode.h"
#include "fixtures.h"
#include "image/colour.h"

using namespace mv::codec;

namespace {

// --- GIF fixtures ---------------------------------------------------------

struct gif_writer {
  std::vector<std::uint8_t> bytes;
};

int write_gif(GifFileType* gif, const GifByteType* data, int len) {
  auto* w = static_cast<gif_writer*>(gif->UserData);
  w->bytes.insert(w->bytes.end(), data, data + len);
  return len;
}

struct gif_frame_spec {
  int left = 0;
  int top = 0;
  int width = 2;
  int height = 2;
  std::uint8_t index = 1;  // palette: 0 black, 1 red, 2 green, 3 blue
  int delay_cs = 0;
  int disposal = DISPOSAL_UNSPECIFIED;
  int transparent = NO_TRANSPARENT_COLOR;
};

std::vector<std::uint8_t> make_gif(const std::vector<gif_frame_spec>& frames, int loops,
                                   int screen = 2) {
  gif_writer w;
  int error = 0;
  GifFileType* gif = EGifOpen(&w, write_gif, &error);
  REQUIRE(gif != nullptr);
  EGifSetGifVersion(gif, true);
  const std::array<GifColorType, 4> colours = {
      GifColorType{0, 0, 0}, GifColorType{255, 0, 0}, GifColorType{0, 255, 0},
      GifColorType{0, 0, 255}};
  ColorMapObject* map = GifMakeMapObject(4, colours.data());
  REQUIRE(EGifPutScreenDesc(gif, screen, screen, 2, 0, map) == GIF_OK);
  if (loops >= 0) {
    REQUIRE(EGifPutExtensionLeader(gif, APPLICATION_EXT_FUNC_CODE) == GIF_OK);
    REQUIRE(EGifPutExtensionBlock(gif, 11, "NETSCAPE2.0") == GIF_OK);
    const std::array<std::uint8_t, 3> loop = {1, static_cast<std::uint8_t>(loops & 0xFF),
                                              static_cast<std::uint8_t>(loops >> 8)};
    REQUIRE(EGifPutExtensionBlock(gif, 3, loop.data()) == GIF_OK);
    REQUIRE(EGifPutExtensionTrailer(gif) == GIF_OK);
  }
  for (const auto& f : frames) {
    GraphicsControlBlock gcb{};
    gcb.DisposalMode = f.disposal;
    gcb.UserInputFlag = false;
    gcb.DelayTime = f.delay_cs;
    gcb.TransparentColor = f.transparent;
    GifByteType ext[4];
    const auto n = EGifGCBToExtension(&gcb, ext);
    REQUIRE(EGifPutExtension(gif, GRAPHICS_EXT_FUNC_CODE, static_cast<int>(n), ext) == GIF_OK);
    REQUIRE(EGifPutImageDesc(gif, f.left, f.top, f.width, f.height, false, nullptr) == GIF_OK);
    std::vector<GifByteType> row(static_cast<std::size_t>(f.width), f.index);
    for (int y = 0; y < f.height; ++y) REQUIRE(EGifPutLine(gif, row.data(), f.width) == GIF_OK);
  }
  REQUIRE(EGifCloseFile(gif, &error) == GIF_OK);
  GifFreeMapObject(map);
  return w.bytes;
}

std::array<std::uint8_t, 4> pixel(const std::vector<std::uint8_t>& rgba, std::uint32_t width,
                                  std::uint32_t x, std::uint32_t y) {
  const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
  return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
}

// --- WebP fixtures --------------------------------------------------------

std::vector<std::uint8_t> make_animated_webp(int loops) {
  WebPAnimEncoderOptions options;
  REQUIRE(WebPAnimEncoderOptionsInit(&options));
  options.anim_params.loop_count = loops;
  WebPAnimEncoder* encoder = WebPAnimEncoderNew(2, 2, &options);
  REQUIRE(encoder != nullptr);
  WebPConfig config;
  REQUIRE(WebPConfigInit(&config));
  config.lossless = 1;
  WebPPicture picture;
  REQUIRE(WebPPictureInit(&picture));
  picture.width = 2;
  picture.height = 2;
  picture.use_argb = 1;
  REQUIRE(WebPPictureAlloc(&picture));
  for (int i = 0; i < 4; ++i) picture.argb[i] = 0xFFFF0000u;  // red
  REQUIRE(WebPAnimEncoderAdd(encoder, &picture, 0, &config));
  for (int i = 0; i < 4; ++i) picture.argb[i] = 0xFF0000FFu;  // blue
  REQUIRE(WebPAnimEncoderAdd(encoder, &picture, 5, &config));    // red lasted 5 ms
  REQUIRE(WebPAnimEncoderAdd(encoder, nullptr, 205, nullptr));    // blue lasts 200 ms
  WebPData assembled;
  WebPDataInit(&assembled);
  REQUIRE(WebPAnimEncoderAssemble(encoder, &assembled));
  std::vector<std::uint8_t> out(assembled.bytes, assembled.bytes + assembled.size);
  WebPDataClear(&assembled);
  WebPAnimEncoderDelete(encoder);
  WebPPictureFree(&picture);
  return out;
}

std::vector<std::uint8_t> make_still_webp() {
  const std::array<std::uint8_t, 16> rgba = {0, 255, 0, 255, 0, 255, 0, 255,
                                             0, 255, 0, 255, 0, 255, 0, 255};
  std::uint8_t* encoded = nullptr;
  const std::size_t size = WebPEncodeLosslessRGBA(rgba.data(), 2, 2, 8, &encoded);
  REQUIRE(size > 0);
  std::vector<std::uint8_t> out(encoded, encoded + size);
  WebPFree(encoded);
  return out;
}

}  // namespace

TEST_CASE("GIF and WebP are recognised by magic bytes", "[codec][gif][webp]") {
  const auto gif = make_gif({gif_frame_spec{}}, -1);
  REQUIRE(probe(gif) == format_family::gif);
  REQUIRE(probe(make_still_webp()) == format_family::webp);
  const std::array<std::uint8_t, 12> riff_wave = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
  REQUIRE(probe(riff_wave) == format_family::unknown);
}

TEST_CASE("an animated GIF composites frames with disposal, delays and loops",
          "[codec][gif]") {
  gif_frame_spec red;  // full canvas, restored to background afterwards
  red.delay_cs = 0;
  red.disposal = DISPOSE_BACKGROUND;
  gif_frame_spec blue;
  blue.left = 1;
  blue.top = 1;
  blue.width = 1;
  blue.height = 1;
  blue.index = 3;
  blue.delay_cs = 2;
  const auto bytes = make_gif({red, blue}, 3);

  auto anim = decode_animation(bytes);
  REQUIRE(anim);
  REQUIRE(anim->format == format_family::gif);
  REQUIRE(anim->width == 2);
  REQUIRE(anim->loops == 3);
  REQUIRE(anim->frames.size() == 2);
  REQUIRE(anim->delays_ms == std::vector<std::uint32_t>{100, 20});  // 0 cs clamps like a browser
  REQUIRE(pixel(anim->frames[0], 2, 0, 0) == std::array<std::uint8_t, 4>{255, 0, 0, 255});
  // Frame 0 was restored to background (transparent), then blue drawn at 1,1.
  REQUIRE(pixel(anim->frames[1], 2, 0, 0) == std::array<std::uint8_t, 4>{0, 0, 0, 0});
  REQUIRE(pixel(anim->frames[1], 2, 1, 1) == std::array<std::uint8_t, 4>{0, 0, 255, 255});

  // As a still: frame 0.
  auto still = decode(bytes);
  REQUIRE(still);
  REQUIRE(still->format == format_family::gif);
  REQUIRE(pixel(still->rgba, 2, 1, 1) == std::array<std::uint8_t, 4>{255, 0, 0, 255});

  // Over budget: not animated here; the still path shows frame 0.
  REQUIRE(decode_animation(bytes, nullptr, 16).error() == mv::status::unsupported_format);
}

TEST_CASE("GIF edge cases: play once, transparency, clipping, one frame, junk", "[codec][gif]") {
  gif_frame_spec a;
  gif_frame_spec b;
  b.index = 2;
  b.transparent = 2;  // every pixel of frame 1 is transparent: frame 0 shows through
  auto once = decode_animation(make_gif({a, b}, -1));
  REQUIRE(once);
  REQUIRE(once->loops == 1);  // no NETSCAPE2.0: play once
  REQUIRE(pixel(once->frames[1], 2, 0, 0) == std::array<std::uint8_t, 4>{255, 0, 0, 255});

  // A frame that runs past the logical screen is clipped, not written past it.
  gif_frame_spec wide;
  wide.left = 1;
  wide.width = 4;
  wide.index = 3;
  auto clipped = decode_animation(make_gif({a, wide}, 0));
  REQUIRE(clipped);
  REQUIRE(clipped->loops == 0);
  REQUIRE(pixel(clipped->frames[1], 2, 1, 0) == std::array<std::uint8_t, 4>{0, 0, 255, 255});
  REQUIRE(pixel(clipped->frames[1], 2, 0, 0) == std::array<std::uint8_t, 4>{255, 0, 0, 255});

  // One frame is a still, not an animation.
  const auto single = make_gif({a}, 0);
  REQUIRE(decode_animation(single).error() == mv::status::unsupported_format);
  REQUIRE(decode(single));

  // Junk after the signature, and a truncated file, fail cleanly.
  std::vector<std::uint8_t> junk = {'G', 'I', 'F', '8', '9', 'a', 1, 2, 3};
  REQUIRE_FALSE(decode(junk));
  auto cut = make_gif({a, b}, 0);
  cut.resize(cut.size() / 2);
  (void)decode(cut);           // must not crash; a partial frame 0 may still decode
  (void)decode_animation(cut);
}

TEST_CASE("an animated WebP decodes with browser delays and its loop count", "[codec][webp]") {
  const auto bytes = make_animated_webp(2);
  auto anim = decode_animation(bytes);
  REQUIRE(anim);
  REQUIRE(anim->format == format_family::webp);
  REQUIRE(anim->loops == 2);
  REQUIRE(anim->frames.size() == 2);
  REQUIRE(anim->delays_ms[0] == 100);  // 5 ms clamps like a browser
  REQUIRE(anim->delays_ms[1] == 200);
  REQUIRE(pixel(anim->frames[0], 2, 0, 0) == std::array<std::uint8_t, 4>{255, 0, 0, 255});
  REQUIRE(pixel(anim->frames[1], 2, 1, 1) == std::array<std::uint8_t, 4>{0, 0, 255, 255});

  auto first = decode(bytes);
  REQUIRE(first);
  REQUIRE(pixel(first->rgba, 2, 0, 0) == std::array<std::uint8_t, 4>{255, 0, 0, 255});
  REQUIRE(decode_animation(bytes, nullptr, 16).error() == mv::status::unsupported_format);
}

TEST_CASE("an animation source yields one frame at a time and rewinds", "[codec][anim]") {
  // 200 frames, alternating red / green: decoded on demand, never all at once.
  std::vector<gif_frame_spec> frames(200);
  for (std::size_t i = 0; i < frames.size(); ++i) {
    frames[i].index = i % 2 == 0 ? 1 : 2;
    frames[i].delay_cs = 3;
  }
  auto shared = std::make_shared<const std::vector<std::uint8_t>>(make_gif(frames, 0));
  auto opened = open_animation(shared);
  REQUIRE(opened);
  animation_source& gif = *opened.value();
  REQUIRE(gif.info().width == 2);
  REQUIRE(gif.info().frame_count == 0);  // a GIF learns its length by reaching the end

  canvas_frame frame;
  auto first = gif.next(frame, nullptr);
  REQUIRE(first);
  REQUIRE(first.value());
  REQUIRE(frame.index == 0);
  REQUIRE(frame.delay_ms == 30);
  const std::vector<std::uint8_t> frame0 = frame.rgba;
  REQUIRE(frame.rgba.size() == 2u * 2u * 4u);  // one canvas, not the animation

  REQUIRE(gif.next(frame, nullptr).value());
  REQUIRE(frame.index == 1);
  REQUIRE(frame.rgba != frame0);
  std::size_t seen = 2;
  while (gif.next(frame, nullptr).value()) ++seen;
  REQUIRE(seen == 200);
  REQUIRE(gif.info().frame_count == 200);
  REQUIRE(gif.info().loops == 0);

  // A loop wrap: back to frame 0, the same pixels.
  REQUIRE(gif.rewind());
  REQUIRE(gif.next(frame, nullptr).value());
  REQUIRE(frame.index == 0);
  REQUIRE(frame.rgba == frame0);

  // WebP: the frame count is known up front; rewind replays from frame 0.
  auto webp_bytes = std::make_shared<const std::vector<std::uint8_t>>(make_animated_webp(0));
  auto webp_opened = open_animation(webp_bytes);
  REQUIRE(webp_opened);
  animation_source& webp = *webp_opened.value();
  REQUIRE(webp.info().frame_count == 2);
  REQUIRE(webp.next(frame, nullptr).value());
  const std::vector<std::uint8_t> webp0 = frame.rgba;
  REQUIRE(webp.next(frame, nullptr).value());
  REQUIRE_FALSE(webp.next(frame, nullptr).value());
  REQUIRE(webp.rewind());
  REQUIRE(webp.next(frame, nullptr).value());
  REQUIRE(frame.rgba == webp0);

  // A still is not an animation source.
  auto still = std::make_shared<const std::vector<std::uint8_t>>(make_still_webp());
  REQUIRE(open_animation(still).error() == mv::status::unsupported_format);
}

// Hidden tool for review note B's manual check: writes a large animated GIF
// (2048 x 2048, 60 frames, looping) to %TEMP%\mv_large_anim.gif so F3's upload
// time and missed frames can be watched while it plays and pans. Run by name:
//   mv_tests "[.make-large-anim]"
TEST_CASE("write a large animated GIF for the pacing check", "[.make-large-anim]") {
  std::vector<gif_frame_spec> frames(60);
  for (std::size_t i = 0; i < frames.size(); ++i) {
    frames[i].width = 2048;
    frames[i].height = 2048;
    frames[i].index = static_cast<std::uint8_t>(1 + i % 3);
    frames[i].delay_cs = 4;  // 25 fps
  }
  const auto bytes = make_gif(frames, 0, 2048);
  const auto path = std::filesystem::temp_directory_path() / "mv_large_anim.gif";
  std::ofstream(path, std::ios::binary)
      .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  REQUIRE(std::filesystem::file_size(path) == bytes.size());
}

namespace {

std::vector<std::uint8_t> srgb_icc() {
  cmsHPROFILE profile = cmsCreate_sRGBProfile();
  REQUIRE(profile != nullptr);
  cmsUInt32Number size = 0;
  REQUIRE(cmsSaveProfileToMem(profile, nullptr, &size));
  std::vector<std::uint8_t> bytes(size);
  REQUIRE(cmsSaveProfileToMem(profile, bytes.data(), &size));
  cmsCloseProfile(profile);
  return bytes;
}

std::vector<std::uint8_t> save_profile(cmsHPROFILE profile) {
  REQUIRE(profile != nullptr);
  cmsUInt32Number size = 0;
  REQUIRE(cmsSaveProfileToMem(profile, nullptr, &size));
  std::vector<std::uint8_t> bytes(size);
  REQUIRE(cmsSaveProfileToMem(profile, bytes.data(), &size));
  cmsCloseProfile(profile);
  return bytes;
}

// Display P3: P3 primaries, D65, the sRGB curve.
std::vector<std::uint8_t> display_p3_icc() {
  cmsCIExyY d65 = {0.3127, 0.3290, 1.0};
  cmsCIExyYTRIPLE p3 = {{0.680, 0.320, 1.0}, {0.265, 0.690, 1.0}, {0.150, 0.060, 1.0}};
  const double params[5] = {2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045};
  cmsToneCurve* curve = cmsBuildParametricToneCurve(nullptr, 4, params);
  REQUIRE(curve != nullptr);
  cmsToneCurve* curves[3] = {curve, curve, curve};
  cmsHPROFILE profile = cmsCreateRGBProfile(&d65, &p3, curves);
  cmsFreeToneCurve(curve);
  return save_profile(profile);
}

// An RGB profile with Rec.709 primaries and a plain gamma 2.2: close to sRGB,
// but not sRGB, so it must not take the copy-through.
std::vector<std::uint8_t> gamma22_icc() {
  cmsCIExyY d65 = {0.3127, 0.3290, 1.0};
  cmsCIExyYTRIPLE rec709 = {{0.640, 0.330, 1.0}, {0.300, 0.600, 1.0}, {0.150, 0.060, 1.0}};
  cmsToneCurve* curve = cmsBuildGamma(nullptr, 2.2);
  REQUIRE(curve != nullptr);
  cmsToneCurve* curves[3] = {curve, curve, curve};
  cmsHPROFILE profile = cmsCreateRGBProfile(&d65, &rec709, curves);
  cmsFreeToneCurve(curve);
  return save_profile(profile);
}

// The pre-note-43 conversion, kept here as the reference: LCMS float pipeline
// to linear Rec.709 with no optimisation, then a per-channel sRGB encode.
std::vector<std::uint8_t> slow_reference(const std::vector<std::uint8_t>& icc,
                                         const std::vector<std::uint8_t>& rgba) {
  cmsHPROFILE in = cmsOpenProfileFromMem(icc.data(), static_cast<cmsUInt32Number>(icc.size()));
  REQUIRE(in != nullptr);
  cmsCIExyY d65 = {0.3127, 0.3290, 1.0};
  cmsCIExyYTRIPLE rec709 = {{0.640, 0.330, 1.0}, {0.300, 0.600, 1.0}, {0.150, 0.060, 1.0}};
  cmsToneCurve* linear = cmsBuildGamma(nullptr, 1.0);
  cmsToneCurve* curves[3] = {linear, linear, linear};
  cmsHPROFILE out = cmsCreateRGBProfile(&d65, &rec709, curves);
  cmsFreeToneCurve(linear);
  cmsHTRANSFORM xform = cmsCreateTransform(in, TYPE_RGB_8, out, TYPE_RGB_FLT,
                                           INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOOPTIMIZE);
  cmsCloseProfile(in);
  cmsCloseProfile(out);
  REQUIRE(xform != nullptr);
  const std::size_t pixels = rgba.size() / 4;
  std::vector<std::uint8_t> rgb(pixels * 3);
  for (std::size_t p = 0; p < pixels; ++p) std::memcpy(&rgb[p * 3], &rgba[p * 4], 3);
  std::vector<float> lin(pixels * 3);
  cmsDoTransform(xform, rgb.data(), lin.data(), static_cast<cmsUInt32Number>(pixels));
  cmsDeleteTransform(xform);
  std::vector<std::uint8_t> result = rgba;
  for (std::size_t i = 0; i < pixels * 3; ++i) {
    const float l = std::clamp(lin[i], 0.0f, 1.0f);
    const float s = l <= 0.0031308f ? 12.92f * l : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
    result[i / 3 * 4 + i % 3] =
        static_cast<std::uint8_t>(std::lround(std::clamp(s, 0.0f, 1.0f) * 255.0f));
  }
  return result;
}

// Every 17th code on each channel (16³ colours, edges included), alpha varied.
mv::codec::raster colour_grid(const std::vector<std::uint8_t>& icc) {
  mv::codec::raster raster;
  raster.width = 64;
  raster.height = 64;
  raster.format = format_family::webp;
  raster.icc = icc;
  raster.rgba.resize(64 * 64 * 4);
  std::size_t p = 0;
  for (int r = 0; r < 16; ++r) {
    for (int g = 0; g < 16; ++g) {
      for (int b = 0; b < 16; ++b, ++p) {
        raster.rgba[p * 4 + 0] = static_cast<std::uint8_t>(r * 17);
        raster.rgba[p * 4 + 1] = static_cast<std::uint8_t>(g * 17);
        raster.rgba[p * 4 + 2] = static_cast<std::uint8_t>(b * 17);
        raster.rgba[p * 4 + 3] = static_cast<std::uint8_t>((p * 7) & 0xFF);
      }
    }
  }
  return raster;
}

}  // namespace

TEST_CASE("sRGB-in-effect profiles are recognised; wide-gamut ones are not", "[image][colour]") {
  REQUIRE(mv::image::is_srgb_icc(srgb_icc()));
  REQUIRE_FALSE(mv::image::is_srgb_icc(display_p3_icc()));
  REQUIRE_FALSE(mv::image::is_srgb_icc(fixtures::adobe_rgb_icc()));
  REQUIRE_FALSE(mv::image::is_srgb_icc(gamma22_icc()));
  REQUIRE_FALSE(mv::image::is_srgb_icc(std::vector<std::uint8_t>{1, 2, 3, 4}));
  REQUIRE_FALSE(mv::image::is_srgb_icc({}));
}

TEST_CASE("the 8-bit display transform matches the float reference within one code",
          "[image][colour][d6]") {
  // Review note 43: the fast LUT path must be the same colour as the float
  // pipeline it replaced. sRGB is a copy-through, so it is held to the same bar.
  const std::pair<const char*, std::vector<std::uint8_t>> profiles[] = {
      {"sRGB", srgb_icc()},
      {"Display P3", display_p3_icc()},
      {"AdobeRGB", fixtures::adobe_rgb_icc()},
      {"Rec.709 gamma 2.2", gamma22_icc()},
  };
  for (const auto& [name, icc] : profiles) {
    CAPTURE(name);
    const auto grid = colour_grid(icc);
    const auto expected = slow_reference(icc, grid.rgba);
    auto got = mv::image::to_display(mv::codec::raster(grid));
    REQUIRE(got);
    REQUIRE(got->icc_tagged);
    int max_error = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
      const int e = std::abs(static_cast<int>(got->rgba[i]) - static_cast<int>(expected[i]));
      max_error = std::max(max_error, e);
    }
    REQUIRE(max_error <= 1);
    for (std::size_t p = 0; p < grid.rgba.size(); p += 4) {
      REQUIRE(got->rgba[p + 3] == grid.rgba[p + 3]);  // alpha is untouched
    }
  }
  // A grey profile on RGB pixels still fails rather than displaying as sRGB.
  auto grey_icc = save_profile(cmsCreateGrayProfile(cmsD50_xyY(), [] {
    static cmsToneCurve* g = cmsBuildGamma(nullptr, 2.2);
    return g;
  }()));
  REQUIRE_FALSE(mv::image::display_transform::create(grey_icc));
}

TEST_CASE("a 2048 x 2048 tagged frame avoids the starvation regression", "[image][colour][perf]") {
  // Review note 43: 755 ms per frame starved a 25 fps animation. Timed in
  // optimised builds only; a Debug LCMS proves nothing about speed.
  mv::codec::raster raster;
  raster.width = 2048;
  raster.height = 2048;
  raster.format = format_family::webp;
  raster.icc = fixtures::adobe_rgb_icc();
  raster.rgba.resize(static_cast<std::size_t>(2048) * 2048 * 4);
  for (std::size_t i = 0; i < raster.rgba.size(); ++i) {
    raster.rgba[i] = static_cast<std::uint8_t>((i * 2654435761u) >> 24);
  }
  auto transform = mv::image::display_transform::create(raster.icc);
  REQUIRE(transform);
  {
    mv::codec::raster warm(raster);
    REQUIRE(transform.value()->apply(std::move(warm)));
  }
  double best_ms = 1e9;
  for (int run = 0; run < 5; ++run) {
    mv::codec::raster copy(raster);
    const auto start = std::chrono::steady_clock::now();
    auto applied = transform.value()->apply(std::move(copy));
    const auto end = std::chrono::steady_clock::now();
    REQUIRE(applied);
    best_ms = std::min(best_ms, std::chrono::duration<double, std::milli>(end - start).count());
  }
  CAPTURE(best_ms);
#ifdef NDEBUG
  // This is a starvation-regression guard, not a single-frame deadline. Allow
  // scheduling noise on shared hosted runners while remaining more than 6x
  // below the measured 755 ms regression.
  REQUIRE(best_ms < 120.0);
#else
  SUCCEED("timing is not asserted in a Debug build");
#endif
}

TEST_CASE("a reused display transform matches the one-shot conversion", "[image][colour]") {
  const auto icc = srgb_icc();
  mv::codec::raster raster;
  raster.width = 16;
  raster.height = 16;
  raster.format = format_family::webp;
  raster.icc = icc;
  raster.rgba.resize(16 * 16 * 4);
  for (std::size_t i = 0; i < raster.rgba.size(); ++i) {
    raster.rgba[i] = static_cast<std::uint8_t>((i * 37) & 0xFF);
  }

  auto reference = mv::image::to_display(mv::codec::raster(raster));
  REQUIRE(reference);
  auto transform = mv::image::display_transform::create(icc);
  REQUIRE(transform);
  // Same transform, many frames (review note 34): identical to a fresh build each time.
  for (int frame = 0; frame < 3; ++frame) {
    auto applied = transform.value()->apply(mv::codec::raster(raster));
    REQUIRE(applied);
    REQUIRE(applied->icc_tagged);
    REQUIRE(applied->rgba == reference->rgba);
  }
  const std::vector<std::uint8_t> broken = {1, 2, 3, 4};
  REQUIRE_FALSE(mv::image::display_transform::create(broken));
  REQUIRE_FALSE(mv::image::display_transform::create({}));
}

// Hidden tool for review note 34's measurement: an ICC-tagged (sRGB ICCP),
// 2048 x 2048, 20-frame, 25 fps looping animated WebP at
// %TEMP%\mv_large_anim_icc.webp. Run by name: mv_tests "[.make-icc-anim]"
TEST_CASE("write a large ICC-tagged animated WebP for the pacing check", "[.make-icc-anim]") {
  constexpr int size = 2048;
  WebPAnimEncoderOptions options;
  REQUIRE(WebPAnimEncoderOptionsInit(&options));
  options.anim_params.loop_count = 0;
  WebPAnimEncoder* encoder = WebPAnimEncoderNew(size, size, &options);
  REQUIRE(encoder != nullptr);
  WebPConfig config;
  REQUIRE(WebPConfigInit(&config));
  config.quality = 50.0f;
  config.method = 0;  // fastest: this is a fixture, not an export
  WebPPicture picture;
  REQUIRE(WebPPictureInit(&picture));
  picture.width = size;
  picture.height = size;
  picture.use_argb = 1;
  REQUIRE(WebPPictureAlloc(&picture));
  int timestamp = 0;
  for (int f = 0; f < 20; ++f) {
    for (int y = 0; y < size; ++y) {
      for (int x = 0; x < size; ++x) {
        const auto r = static_cast<std::uint32_t>((x + f * 40) & 0xFF);
        const auto g = static_cast<std::uint32_t>((y + f * 20) & 0xFF);
        picture.argb[y * size + x] = 0xFF000000u | (r << 16) | (g << 8) | 0x80u;
      }
    }
    REQUIRE(WebPAnimEncoderAdd(encoder, &picture, timestamp, &config));
    timestamp += 40;
  }
  REQUIRE(WebPAnimEncoderAdd(encoder, nullptr, timestamp, nullptr));
  WebPData assembled;
  WebPDataInit(&assembled);
  REQUIRE(WebPAnimEncoderAssemble(encoder, &assembled));
  WebPAnimEncoderDelete(encoder);
  WebPPictureFree(&picture);

  // Add the ICCP chunk. AdobeRGB, not sRGB: an sRGB profile is now a
  // copy-through (review note 43) and would not exercise the transform.
  WebPMux* mux = WebPMuxCreate(&assembled, 1);
  REQUIRE(mux != nullptr);
  const auto icc = fixtures::adobe_rgb_icc();
  const WebPData profile{icc.data(), icc.size()};
  REQUIRE(WebPMuxSetChunk(mux, "ICCP", &profile, 1) == WEBP_MUX_OK);
  WebPData tagged;
  WebPDataInit(&tagged);
  REQUIRE(WebPMuxAssemble(mux, &tagged) == WEBP_MUX_OK);
  WebPMuxDelete(mux);
  WebPDataClear(&assembled);

  const auto path = std::filesystem::temp_directory_path() / "mv_large_anim_icc.webp";
  std::ofstream(path, std::ios::binary)
      .write(reinterpret_cast<const char*>(tagged.bytes), static_cast<std::streamsize>(tagged.size));
  REQUIRE(std::filesystem::file_size(path) == tagged.size);
  WebPDataClear(&tagged);

  // It really is tagged: the decoder reports the profile.
  const std::vector<std::uint8_t> bytes = [&] {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }();
  auto opened = open_animation(std::make_shared<const std::vector<std::uint8_t>>(bytes));
  REQUIRE(opened);
  REQUIRE_FALSE(opened.value()->info().icc.empty());
}

TEST_CASE("a still WebP decodes, and is not an animation", "[codec][webp]") {
  const auto bytes = make_still_webp();
  auto still = decode(bytes);
  REQUIRE(still);
  REQUIRE(still->width == 2);
  REQUIRE(pixel(still->rgba, 2, 1, 1) == std::array<std::uint8_t, 4>{0, 255, 0, 255});
  REQUIRE(decode_animation(bytes).error() == mv::status::unsupported_format);
  std::vector<std::uint8_t> broken = bytes;
  broken.resize(16);
  REQUIRE_FALSE(decode(broken));
}
