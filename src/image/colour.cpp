// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "image/colour.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <lcms2.h>

namespace mv::image {
namespace {

constexpr std::size_t kTile = 256 * 256;

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

}  // namespace

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
    xform = cmsCreateTransformTHR(lcms, in, TYPE_RGBA_8, out, TYPE_RGBA_8,
                                  INTENT_RELATIVE_COLORIMETRIC,
                                  cmsFLAGS_COPY_ALPHA | cmsFLAGS_HIGHRESPRECALC);
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
  if ((!transform_ && !passthrough_) || src.width == 0 || src.height == 0 ||
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

  const auto xform = static_cast<cmsHTRANSFORM>(transform_);
  const std::size_t pixels = static_cast<std::size_t>(dst.width) * dst.height;
  std::vector<std::uint8_t> tile(kTile * 4);
  for (std::size_t i = 0; i < pixels; i += kTile) {
    if (ctx && ctx->cancelled()) return err(status::cancelled);
    const std::size_t n = std::min(kTile, pixels - i);
    std::uint8_t* row = dst.rgba.data() + i * 4;
    cmsDoTransform(xform, row, tile.data(), static_cast<cmsUInt32Number>(n));
    std::memcpy(row, tile.data(), n * 4);
  }
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
  // display tagged bytes as sRGB (plan/04, plan/12). One transform per call,
  // with its own cmsContext: the default context is not thread-safe.
  auto transform = display_transform::create(src.icc);
  if (!transform) return err(transform.error());
  return transform.value()->apply(std::move(src), ctx);
}

}  // namespace mv::image
