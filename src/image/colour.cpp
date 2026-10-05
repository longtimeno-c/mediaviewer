// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "image/colour.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>
#include <lcms2.h>

#include "core/parallel.h"

namespace mv::image {
namespace {

// Below this a transform stays on the calling worker (thumbnails, previews of
// small files); above it the bands are worth a thread each.
constexpr std::size_t kPixelsPerBand = 1u << 20;

// An 8-bit step is 1/255 ≈ 0.0039; half of that keeps a match invisible.
constexpr double kSrgbTolerance = 0.0015;

void lcms_silence(cmsContext, cmsUInt32Number, const char*) {}

bool same_xyz(const cmsCIEXYZ* a, const cmsCIEXYZ* b) noexcept {
  return a && b && std::fabs(a->X - b->X) < kSrgbTolerance &&
         std::fabs(a->Y - b->Y) < kSrgbTolerance && std::fabs(a->Z - b->Z) < kSrgbTolerance;
}

bool same_curve(const cmsToneCurve* a, const cmsToneCurve* b) noexcept {
  if (!a || !b) return false;
  for (int i = 0; i <= 64; ++i) {
    const float x = static_cast<float>(i) / 64.0f;
    if (std::fabs(cmsEvalToneCurveFloat(a, x) - cmsEvalToneCurveFloat(b, x)) > kSrgbTolerance) {
      return false;
    }
  }
  return true;
}

// Matrix/shaper RGB with sRGB's (D50-adapted) colorants and sRGB's curve on
// every channel: relative-colorimetric to sRGB is the identity within 8 bits.
bool is_srgb_profile(cmsContext lcms, cmsHPROFILE in) {
  if (cmsGetColorSpace(in) != cmsSigRgbData || !cmsIsMatrixShaper(in)) return false;
  cmsHPROFILE ref = cmsCreate_sRGBProfileTHR(lcms);
  if (!ref) return false;
  bool same = true;
  for (const cmsTagSignature tag :
       {cmsSigRedColorantTag, cmsSigGreenColorantTag, cmsSigBlueColorantTag}) {
    same = same && same_xyz(static_cast<const cmsCIEXYZ*>(cmsReadTag(in, tag)),
                            static_cast<const cmsCIEXYZ*>(cmsReadTag(ref, tag)));
  }
  for (const cmsTagSignature tag : {cmsSigRedTRCTag, cmsSigGreenTRCTag, cmsSigBlueTRCTag}) {
    same = same && same_curve(static_cast<const cmsToneCurve*>(cmsReadTag(in, tag)),
                              static_cast<const cmsToneCurve*>(cmsReadTag(ref, tag)));
  }
  cmsCloseProfile(ref);
  return same;
}

// A camera dump carries the same few profiles on every file (Display P3 on a
// phone, Adobe RGB on a camera set to it). Building one is milliseconds, and
// the preview and the full decode each asked for it, so keep the last few.
// Keyed by the profile bytes, compared in full. Transforms are NOCACHE, so a
// cached one serves several workers at once; the shared_ptr keeps one alive
// for a worker still applying it after it has been evicted.
struct transform_cache {
  struct entry {
    std::vector<std::uint8_t> icc;
    std::shared_ptr<const display_transform> transform;
  };
  std::mutex mutex;
  std::vector<entry> entries;  // most recent last
  static constexpr std::size_t kMax = 8;
};

transform_cache& cache() {
  static transform_cache c;
  return c;
}

}  // namespace

result<std::shared_ptr<const display_transform>> cached_transform(
    std::span<const std::uint8_t> icc) {
  transform_cache& c = cache();
  {
    std::lock_guard lock(c.mutex);
    for (std::size_t i = c.entries.size(); i-- > 0;) {
      auto& e = c.entries[i];
      if (e.icc.size() == icc.size() && std::equal(icc.begin(), icc.end(), e.icc.begin())) {
        auto hit = e.transform;
        const auto at = c.entries.begin() + static_cast<std::ptrdiff_t>(i);
        std::rotate(at, at + 1, c.entries.end());
        return hit;
      }
    }
  }
  // Built outside the lock: two workers may both build the same profile once;
  // the second insert is dropped.
  auto made = display_transform::create(icc);
  if (!made) return err(made.error());
  std::shared_ptr<const display_transform> shared(std::move(made).value());
  try {
    std::lock_guard lock(c.mutex);
    for (const auto& e : c.entries) {
      if (e.icc.size() == icc.size() && std::equal(icc.begin(), icc.end(), e.icc.begin())) return e.transform;
    }
    if (c.entries.size() >= transform_cache::kMax) c.entries.erase(c.entries.begin());
    c.entries.push_back({std::vector<std::uint8_t>(icc.begin(), icc.end()), shared});
  } catch (...) {
    // Out of memory for the cache entry: still return the transform.
  }
  return shared;
}

bool is_srgb_icc(std::span<const std::uint8_t> icc) noexcept {
  if (icc.empty()) return false;
  cmsContext lcms = cmsCreateContext(nullptr, nullptr);
  if (!lcms) return false;
  cmsSetLogErrorHandlerTHR(lcms, lcms_silence);
  bool srgb = false;
  if (cmsHPROFILE in =
          cmsOpenProfileFromMemTHR(lcms, icc.data(), static_cast<cmsUInt32Number>(icc.size()))) {
    srgb = is_srgb_profile(lcms, in);
    cmsCloseProfile(in);
  }
  cmsDeleteContext(lcms);
  return srgb;
}

result<std::unique_ptr<display_transform>> display_transform::create(
    std::span<const std::uint8_t> icc) {
  if (icc.empty()) return err(status::invalid_arg);
  cmsContext lcms = cmsCreateContext(nullptr, nullptr);
  if (!lcms) return err(status::internal);
  cmsSetLogErrorHandlerTHR(lcms, lcms_silence);

  cmsHPROFILE in =
      cmsOpenProfileFromMemTHR(lcms, icc.data(), static_cast<cmsUInt32Number>(icc.size()));
  if (!in) {
    cmsDeleteContext(lcms);
    return err(status::corrupt);
  }

  if (cmsGetColorSpace(in) == cmsSigGrayData) {
    // An RGBA transform cannot take a grey profile; tabulate grey → sRGB once.
    cmsHPROFILE out = cmsCreate_sRGBProfileTHR(lcms);
    cmsHTRANSFORM grey = out ? cmsCreateTransformTHR(lcms, in, TYPE_GRAY_8, out, TYPE_RGB_8,
                                                     INTENT_RELATIVE_COLORIMETRIC, 0)
                             : nullptr;
    if (out) cmsCloseProfile(out);
    cmsCloseProfile(in);
    if (!grey) {
      cmsDeleteContext(lcms);
      return err(status::corrupt);
    }
    std::array<std::uint8_t, 256> ramp{};
    for (std::size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<std::uint8_t>(i);
    std::array<std::uint8_t, 256 * 3> lut{};
    cmsDoTransform(grey, ramp.data(), lut.data(), 256);
    cmsDeleteTransform(grey);
    cmsDeleteContext(lcms);
    try {
      auto made = std::unique_ptr<display_transform>(new display_transform());
      made->grey_ = true;
      made->grey_lut_ = lut;
      return made;
    } catch (...) {
      return err(status::out_of_memory);
    }
  }

  cmsHTRANSFORM xform = nullptr;
  const bool passthrough = is_srgb_profile(lcms, in);
  if (!passthrough) {
    cmsHPROFILE out = cmsCreate_sRGBProfileTHR(lcms);
    if (!out) {
      cmsCloseProfile(in);
      cmsDeleteContext(lcms);
      return err(status::internal);
    }
    // Review note 43: 8-bit in, 8-bit out, optimisation allowed, so LCMS
    // precomputes its device link (a matrix-shaper pair stays a matrix with
    // curve tables; a LUT profile gets a high-resolution grid) instead of
    // evaluating a float pipeline per pixel. Relative colorimetric to sRGB is
    // the same ICC → linear → sRGB encode as before, within an 8-bit step.
    // NOCACHE: no one-pixel cache, so the transform is safe to run from several
    // threads at once (apply's bands, and the shared cache in to_display).
    xform = cmsCreateTransformTHR(lcms, in, TYPE_RGBA_8, out, TYPE_RGBA_8,
                                  INTENT_RELATIVE_COLORIMETRIC,
                                  cmsFLAGS_COPY_ALPHA | cmsFLAGS_HIGHRESPRECALC | cmsFLAGS_NOCACHE);
    cmsCloseProfile(out);
  }
  cmsCloseProfile(in);
  if (!passthrough && !xform) {
    cmsDeleteContext(lcms);
    return err(status::corrupt);
  }
  try {
    auto made = std::unique_ptr<display_transform>(new display_transform());
    made->context_ = lcms;
    made->transform_ = xform;
    made->passthrough_ = passthrough;
    return made;
  } catch (...) {
    if (xform) cmsDeleteTransform(xform);
    cmsDeleteContext(lcms);
    return err(status::out_of_memory);
  }
}

display_transform::~display_transform() {
  if (transform_) cmsDeleteTransform(static_cast<cmsHTRANSFORM>(transform_));
  if (context_) cmsDeleteContext(static_cast<cmsContext>(context_));
}

result<display_image> display_transform::apply(codec::raster&& src, const job_context* ctx) const {
  if ((!transform_ && !passthrough_ && !grey_) || src.width == 0 || src.height == 0 ||
      src.rgba.size() != static_cast<std::size_t>(src.width) * src.height * 4) {
    return err(status::corrupt);
  }

  display_image dst;
  dst.width = src.width;
  dst.height = src.height;
  dst.format = src.format;
  dst.intent = src.intent;
  dst.icc_tagged = true;
  dst.rgba = std::move(src.rgba);
  if (passthrough_) {
    if (ctx && ctx->cancelled()) return err(status::cancelled);
    return dst;
  }
  if (grey_) {
    // R = G = B from the decoder's grey expansion; alpha stays. Colour pixels
    // under a grey profile are a broken file: fail, never guess a colour space.
    const std::size_t pixels = static_cast<std::size_t>(dst.width) * dst.height;
    for (std::size_t i = 0; i < pixels; ++i) {
      if ((i & 0xFFFF) == 0 && ctx && ctx->cancelled()) return err(status::cancelled);
      std::uint8_t* px = dst.rgba.data() + i * 4;
      if (px[0] != px[1] || px[0] != px[2]) return err(status::corrupt);
      const std::uint8_t* rgb = grey_lut_.data() + static_cast<std::size_t>(px[0]) * 3;
      px[0] = rgb[0];
      px[1] = rgb[1];
      px[2] = rgb[2];
    }
    return dst;
  }

  // In place (same pixel size in and out, which LCMS allows), in row-aligned
  // bands across a few threads. A 24 MP Adobe RGB JPEG spent ~160 ms here on
  // one core, through a scratch tile and a memcpy back.
  const auto xform = static_cast<cmsHTRANSFORM>(transform_);
  const std::size_t pixels = static_cast<std::size_t>(dst.width) * dst.height;
  constexpr std::size_t kChunk = 64 * 1024;  // cancellation granularity
  std::uint8_t* base = dst.rgba.data();
  core::parallel_bands(pixels, kPixelsPerBand, [&](std::size_t begin, std::size_t end) {
    for (std::size_t i = begin; i < end; i += kChunk) {
      if (ctx && ctx->cancelled()) return;
      const std::size_t n = std::min(kChunk, end - i);
      cmsDoTransform(xform, base + i * 4, base + i * 4, static_cast<cmsUInt32Number>(n));
    }
  });
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  return dst;
}

result<display_image> to_display(codec::raster&& src, const job_context* ctx) {
  if (src.width == 0 || src.height == 0 ||
      src.rgba.size() != static_cast<std::size_t>(src.width) * src.height * 4) {
    return err(status::corrupt);
  }
  if (src.intent != codec::transfer_intent::display_referred) {
    // PR 2 has no scene-referred source. Refusing beats silently tone-mapping.
    return err(status::unsupported_format);
  }

  if (src.icc.empty()) {
    display_image dst;
    dst.width = src.width;
    dst.height = src.height;
    dst.format = src.format;
    dst.intent = src.intent;
    dst.icc_tagged = src.tagged_srgb;
    dst.rgba = std::move(src.rgba);
    return dst;
  }

  // D6: a broken profile is still a tagged file. Fail-open as sRGB would
  // display tagged bytes as sRGB (docs/design/04, docs/design/12).
  auto transform = cached_transform(src.icc);
  if (!transform) return err(transform.error());
  return transform.value()->apply(std::move(src), ctx);
}

}  // namespace mv::image
