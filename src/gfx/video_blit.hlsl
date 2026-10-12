// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 5a video blit (gfx/video_blit.cpp): NV12/P010 -> linear RGB with the
// stream's real matrix, plus HLG/PQ -> SDR. Compiled at build time by fxc into
// byte-array headers (CMakeLists.txt, mv_fxc; issue #163). MSL twin:
// gfx/video_blit_metal.mm. Enum values MUST match gfx/colour_desc.h (checked by
// static_asserts in gfx/video_blit.cpp).
cbuffer VideoCamera : register(b0) {
  float2 pan;
  float  zoom;
  float  _pad0;
  float2 window_size;
  float2 image_size;    // the VISIBLE frame, in pixels
  float2 texture_size;  // the decoder's ALLOCATION, padded up to its alignment
  float2 origin;
  uint4  colour_a;      // x matrix, y transfer, z range, w bit_depth
  uint4  colour_b;      // x primaries, y quarter turns clockwise, zw unused
};

Texture2D<float>  luma_tex   : register(t0);
Texture2D<float2> chroma_tex : register(t1);
SamplerState samp_linear : register(s0);

struct VSOut { float4 pos : SV_Position; };

VSOut vs_main(uint id : SV_VertexID) {
  VSOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return o;
}

// ---------------------------------------------------------------------------
// Transfer functions. All of these return LINEAR light.
// ---------------------------------------------------------------------------

// BT.1886: the EOTF a display-referred SDR video signal is authored against.
// Not sRGB — close, but the toe differs, and video is not sRGB-encoded.
float3 eotf_bt1886(float3 v) { return pow(max(v, 0.0), 2.4); }

float3 eotf_srgb(float3 v) {
  return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

// SMPTE ST 2084 (PQ). Returns 0..1 where 1.0 is 10000 nits.
float3 eotf_pq(float3 e) {
  const float m1 = 0.1593017578125, m2 = 78.84375;
  const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
  float3 p = pow(max(e, 0.0), 1.0 / m2);
  float3 num = max(p - c1, 0.0);
  float3 den = c2 - c3 * p;
  return pow(num / max(den, 1e-6), 1.0 / m1);
}

// ARIB STD-B67 (HLG) inverse OETF: signal -> SCENE linear, 0..1.
float3 inverse_oetf_hlg(float3 e) {
  const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
  float3 lo = (e * e) / 3.0;
  float3 hi = (exp((e - c) / a) + b) / 12.0;
  return e <= 0.5 ? lo : hi;
}

// ---------------------------------------------------------------------------
// Gamut. We output BT.709 primaries because the swapchain is 8-bit sRGB (D6).
// ---------------------------------------------------------------------------
float3 to_bt709(float3 rgb, uint primaries) {
  if (primaries == 4) {  // BT.2020 -> BT.709
    return float3(
      dot(rgb, float3( 1.6605, -0.5876, -0.0728)),
      dot(rgb, float3(-0.1246,  1.1329, -0.0083)),
      dot(rgb, float3(-0.0182, -0.1006,  1.1187)));
  }
  if (primaries == 2) {  // SMPTE-C (BT.601 525-line) -> BT.709
    return float3(
      dot(rgb, float3( 0.9395,  0.0502,  0.0103)),
      dot(rgb, float3( 0.0178,  0.9658,  0.0164)),
      dot(rgb, float3(-0.0016, -0.0044,  1.0060)));
  }
  if (primaries == 3) {  // EBU (BT.601 625-line) -> BT.709
    return float3(
      dot(rgb, float3( 1.0440, -0.0440,  0.0000)),
      dot(rgb, float3( 0.0000,  1.0000,  0.0000)),
      dot(rgb, float3( 0.0000,  0.0118,  0.9882)));
  }
  return rgb;  // already BT.709, or unspecified and resolved upstream
}

// ---------------------------------------------------------------------------
// HDR -> SDR. docs/design/03: "HDR video -> SDR tone-mapping is a v1 correctness
// requirement, not a v1.1 feature." Reference white is 203 nits (ITU-R
// BT.2408), so an HDR clip's diffuse white lands where SDR white is rather than
// at the top of the range — that mapping is the difference between "correct"
// and "washed out" in the verify line.
//
// Reinhard-extended, on luminance with the ratio applied to RGB, so hue is
// preserved instead of each channel clipping on its own.
// ---------------------------------------------------------------------------
float3 tone_map(float3 linear_scene, float peak_over_white) {
  float l = max(dot(linear_scene, float3(0.2126, 0.7152, 0.0722)), 1e-6);
  float w2 = peak_over_white * peak_over_white;
  float mapped = l * (1.0 + l / w2) / (1.0 + l);
  return linear_scene * (mapped / l);
}

float4 ps_main(VSOut vin) : SV_Target {
  uint matrix_id = colour_a.x;
  uint transfer  = colour_a.y;
  uint range_id  = colour_a.z;
  uint depth     = colour_a.w;
  uint primaries = colour_b.x;
  uint turns     = colour_b.y & 3;

  // The camera frames the DISPLAYED picture: the coded frame turned by the
  // container's matrix (a portrait phone clip is coded landscape).
  float2 shown_size = ((turns & 1) != 0) ? image_size.yx : image_size;
  float2 image_px = pan + (vin.pos.xy - (origin + window_size * 0.5)) / zoom;
  float2 uv = image_px / shown_size;
  if (any(uv < 0.0) || any(uv > 1.0)) {
    return float4(0.016, 0.018, 0.024, 1.0);
  }
  // Back from the displayed picture to the coded frame: the inverse of a
  // clockwise turn. 90: shown (x, y) came from coded (y, 1 - x).
  if (turns == 1)      uv = float2(uv.y, 1.0 - uv.x);
  else if (turns == 2) uv = float2(1.0 - uv.x, 1.0 - uv.y);
  else if (turns == 3) uv = float2(1.0 - uv.y, uv.x);
  // Sample the VISIBLE rect, not the allocation: a decoder surface is padded up
  // to its alignment, and sampling the full texture shows garbage down the
  // right and bottom edges.
  float2 uv_tex = uv * (image_size / texture_size);

  float  y  = luma_tex.SampleLevel(samp_linear, uv_tex, 0);
  float2 cc = chroma_tex.SampleLevel(samp_linear, uv_tex, 0);

  // P010 keeps its 10 bits in the HIGH bits of a 16-bit word, so an R16_UNORM
  // read returns code*64/65535, not code/1023. Rescale, or every value is 64x
  // too large and it reads as a colour bug rather than a scaling one.
  if (depth > 8) {
    const float p010_scale = 65535.0 / 65472.0;  // 65472 = 1023 * 64
    y  *= p010_scale;
    cc *= p010_scale;
  }

  // Range. Limited is the common case but never the assumption: an explicit
  // full-range tag has to win, or PC-range phone video comes out crushed.
  float y_min, y_span, c_mid, c_span;
  if (range_id == 2) {
    y_min = 0.0; y_span = 1.0; c_mid = 0.5; c_span = 1.0;
  } else if (depth > 8) {
    y_min = 64.0 / 1023.0; y_span = 876.0 / 1023.0;
    c_mid = 512.0 / 1023.0; c_span = 896.0 / 1023.0;
  } else {
    y_min = 16.0 / 255.0; y_span = 219.0 / 255.0;
    c_mid = 128.0 / 255.0; c_span = 224.0 / 255.0;
  }
  float yy = (y - y_min) / y_span;
  float cb = (cc.x - c_mid) / c_span;
  float cr = (cc.y - c_mid) / c_span;

  // YCbCr -> R'G'B' from the stream's real matrix. docs/design/05: "Do not assume
  // BT.709 limited range; phone video is frequently BT.2020."
  float kr, kb;
  if (matrix_id == 2)      { kr = 0.299;  kb = 0.114;  }  // BT.601
  else if (matrix_id == 3) { kr = 0.2627; kb = 0.0593; }  // BT.2020 NCL
  else if (matrix_id == 4) { kr = 0.212;  kb = 0.087;  }  // SMPTE 240M
  else                     { kr = 0.2126; kb = 0.0722; }  // BT.709
  float kg = 1.0 - kr - kb;

  float3 rgb;
  rgb.r = yy + 2.0 * (1.0 - kr) * cr;
  rgb.b = yy + 2.0 * (1.0 - kb) * cb;
  rgb.g = yy - (2.0 * (kr * (1.0 - kr) * cr + kb * (1.0 - kb) * cb)) / kg;

  float3 linear_rgb;
  if (transfer == 3) {
    // PQ. EOTF gives 0..1 of 10000 nits; normalise so 203 nits reads as 1.0.
    linear_rgb = eotf_pq(saturate(rgb)) * (10000.0 / 203.0);
    linear_rgb = to_bt709(linear_rgb, primaries);
    linear_rgb = tone_map(linear_rgb, 10000.0 / 203.0);
  } else if (transfer == 4) {
    // HLG. Inverse OETF gives SCENE light; the OOTF (gamma 1.2 for a 1000-nit
    // reference display) turns it into display light. Skipping the OOTF is what
    // makes an iPhone HLG clip look flat and washed out.
    float3 scene = inverse_oetf_hlg(saturate(rgb));
    float ys = max(dot(scene, float3(0.2627, 0.6780, 0.0593)), 1e-6);  // BT.2020 luma
    linear_rgb = scene * pow(ys, 0.2) * (1000.0 / 203.0);
    linear_rgb = to_bt709(linear_rgb, primaries);
    linear_rgb = tone_map(linear_rgb, 1000.0 / 203.0);
  } else if (transfer == 2) {
    linear_rgb = to_bt709(eotf_srgb(saturate(rgb)), primaries);
  } else {
    // SDR video. NOT tone-mapped: display-referred content that is already
    // graded for an SDR display, exactly like a camera JPEG (D6).
    linear_rgb = to_bt709(eotf_bt1886(saturate(rgb)), primaries);
  }

  // The RTV is _SRGB, so the hardware encodes on write and this stays linear.
  return float4(saturate(linear_rgb), 1.0);
}
