// SPDX-License-Identifier: GPL-2.0-or-later
#include "image/colour.h"

#include <algorithm>
#include <cmath>
#include <lcms2.h>

namespace mv::image {
namespace {

constexpr std::size_t kTile = 256 * 256;

void lcms_silence(cmsContext, cmsUInt32Number, const char*) {}

cmsHPROFILE linear_rec709(cmsContext ctx) {
  cmsCIExyY d65 = {0.3127, 0.3290, 1.0};
  cmsCIExyYTRIPLE primaries = {
      {0.640, 0.330, 1.0},
      {0.300, 0.600, 1.0},
      {0.150, 0.060, 1.0},
  };
  cmsToneCurve* g = cmsBuildGamma(ctx, 1.0);
  if (!g) return nullptr;
  cmsToneCurve* curves[3] = {g, g, g};
  cmsHPROFILE profile = cmsCreateRGBProfileTHR(ctx, &d65, &primaries, curves);
  cmsFreeToneCurve(g);
  return profile;
}

std::uint8_t srgb_encode(float linear) noexcept {
  linear = std::clamp(linear, 0.0f, 1.0f);
  const float s = (linear <= 0.0031308f) ? 12.92f * linear
                                         : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
  return static_cast<std::uint8_t>(std::lround(std::clamp(s, 0.0f, 1.0f) * 255.0f));
}

result<display_image> transform_icc(codec::raster& src, const job_context* ctx) {
  cmsContext lcms = cmsCreateContext(nullptr, nullptr);
  if (!lcms) return err(status::internal);
  cmsSetLogErrorHandlerTHR(lcms, lcms_silence);

  cmsHPROFILE in =
      cmsOpenProfileFromMemTHR(lcms, src.icc.data(), static_cast<cmsUInt32Number>(src.icc.size()));
  if (!in) {
    cmsDeleteContext(lcms);
    return err(status::corrupt);
  }

  cmsHPROFILE out = linear_rec709(lcms);
  if (!out) {
    cmsCloseProfile(in);
    cmsDeleteContext(lcms);
    return err(status::internal);
  }

  cmsHTRANSFORM xform =
      cmsCreateTransformTHR(lcms, in, TYPE_RGB_8, out, TYPE_RGB_FLT, INTENT_RELATIVE_COLORIMETRIC,
                            cmsFLAGS_NOOPTIMIZE);
  cmsCloseProfile(in);
  cmsCloseProfile(out);
  if (!xform) {
    cmsDeleteContext(lcms);
    return err(status::corrupt);
  }

  display_image dst;
  dst.width = src.width;
  dst.height = src.height;
  dst.format = src.format;
  dst.intent = src.intent;
  dst.icc_tagged = true;
  dst.rgba = std::move(src.rgba);

  const std::size_t pixels = static_cast<std::size_t>(dst.width) * dst.height;
  std::vector<std::uint8_t> rgb(kTile * 3);
  std::vector<float> linear(kTile * 3);

  for (std::size_t i = 0; i < pixels; i += kTile) {
    if (ctx && ctx->cancelled()) {
      cmsDeleteTransform(xform);
      cmsDeleteContext(lcms);
      return err(status::cancelled);
    }
    const std::size_t n = std::min(kTile, pixels - i);
    for (std::size_t p = 0; p < n; ++p) {
      const std::uint8_t* s = dst.rgba.data() + (i + p) * 4;
      rgb[p * 3 + 0] = s[0];
      rgb[p * 3 + 1] = s[1];
      rgb[p * 3 + 2] = s[2];
    }
    cmsDoTransform(xform, rgb.data(), linear.data(), static_cast<cmsUInt32Number>(n));
    for (std::size_t p = 0; p < n; ++p) {
      std::uint8_t* d = dst.rgba.data() + (i + p) * 4;
      d[0] = srgb_encode(linear[p * 3 + 0]);
      d[1] = srgb_encode(linear[p * 3 + 1]);
      d[2] = srgb_encode(linear[p * 3 + 2]);
    }
  }

  cmsDeleteTransform(xform);
  cmsDeleteContext(lcms);
  return dst;
}

}  // namespace

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
  // display tagged bytes as sRGB (plan/04, plan/12).
  return transform_icc(src, ctx);
}

}  // namespace mv::image
