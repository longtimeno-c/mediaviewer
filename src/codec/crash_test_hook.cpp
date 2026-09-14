// SPDX-License-Identifier: GPL-2.0-or-later
#include "codec/crash_test_hook.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <avif/avif.h>
#include <gif_lib.h>
#include <jconfig.h>
#include <libheif/heif.h>
#include <libraw/libraw.h>
#include <spng.h>
#include <tiffio.h>
#include <webp/decode.h>

#include "codec/format.h"
#include "core/crash_context.h"

namespace mv::codec {
namespace {

// Library versions, formatted once. Static storage: the crash-context slot
// keeps the pointer, never a copy of caller data.
struct versions {
  char jpeg[16]{};
  char png[16]{};
  char gif[16]{};
  char webp[16]{};
  char tiff[16]{};
  char heif[16]{};
  char avif[16]{};
  char raw[16]{};
};

const versions& lib_versions() noexcept {
  static const versions v = [] {
    versions out{};
    auto copy = [](char* dst, const char* src) {
      if (!src) return;
      std::size_t n = 0;
      // First token only: TIFFGetVersion() is a multi-line banner.
      while (src[n] && src[n] != '\n' && n + 1 < 16) ++n;
      std::memcpy(dst, src, n);
      dst[n] = '\0';
    };
    (void)std::snprintf(out.jpeg, sizeof(out.jpeg), "%d.%d.%d",
                        LIBJPEG_TURBO_VERSION_NUMBER / 1000000,
                        (LIBJPEG_TURBO_VERSION_NUMBER / 1000) % 1000,
                        LIBJPEG_TURBO_VERSION_NUMBER % 1000);
    copy(out.png, spng_version_string());
    (void)std::snprintf(out.gif, sizeof(out.gif), "%d.%d.%d", GIFLIB_MAJOR, GIFLIB_MINOR,
                        GIFLIB_RELEASE);
    const int w = WebPGetDecoderVersion();
    (void)std::snprintf(out.webp, sizeof(out.webp), "%d.%d.%d", (w >> 16) & 0xff, (w >> 8) & 0xff,
                        w & 0xff);
    const char* tiff = TIFFGetVersion();  // "LIBTIFF, Version 4.7.0\n..."
    if (tiff) {
      const char* ver = std::strstr(tiff, "Version ");
      copy(out.tiff, ver ? ver + 8 : tiff);
    }
    copy(out.heif, heif_get_version());
    copy(out.avif, avifVersion());
    copy(out.raw, libraw_version());
    return out;
  }();
  return v;
}

struct decoder_label {
  const char* name;
  const char* version;
};

decoder_label label_for(format_family f) noexcept {
  const versions& v = lib_versions();
  switch (f) {
    case format_family::jpeg: return {"libjpeg-turbo", v.jpeg};
    case format_family::png:  return {"libspng", v.png};
    case format_family::bmp:  return {"mv-bmp", ""};
    case format_family::gif:  return {"giflib", v.gif};
    case format_family::webp: return {"libwebp", v.webp};
    // TIFF-container RAWs reclassify to LibRaw after this scope opens; both
    // are named rather than calling looks_like_raw() a second time.
    case format_family::tiff: return {"libtiff|libraw", v.tiff};
    case format_family::ico:  return {"mv-ico", ""};
    case format_family::heic: return {"libheif", v.heif};
    case format_family::avif: return {"libavif", v.avif};
    case format_family::raw:  return {"libraw", v.raw};
    case format_family::unknown: break;
  }
  return {"none", ""};
}

std::atomic<int> g_armed{-1};

[[noreturn]] void deliberate_crash() noexcept {
  // A real access violation on the decode worker, not a CRT abort: this is
  // the failure mode a hostile file produces.
  volatile int* volatile target = nullptr;
  *target = 0x4D56;  // NOLINT
  std::abort();      // unreachable; keeps [[noreturn]] honest
}

}  // namespace

bool crash_test_armed() noexcept {
  int armed = g_armed.load(std::memory_order_relaxed);
  if (armed < 0) {
    const char* env = std::getenv("MV_CRASH_TEST");
    armed = (env && std::strcmp(env, "decode") == 0) ? 1 : 0;
    g_armed.store(armed, std::memory_order_relaxed);
  }
  return armed == 1;
}

bool crash_test_marker_present(std::span<const std::uint8_t> bytes) noexcept {
  const auto head = bytes.first(std::min(bytes.size(), kCrashTestScanBytes));
  const auto it = std::search(head.begin(), head.end(), kCrashTestMarker.begin(),
                              kCrashTestMarker.end(), [](std::uint8_t a, char b) {
                                return a == static_cast<std::uint8_t>(b);
                              });
  return it != head.end();
}

decode_crash_scope::decode_crash_scope(std::span<const std::uint8_t> bytes,
                                       const job_context* ctx) noexcept {
  (void)ctx;
  const format_family family = probe(bytes);
  const decoder_label label = label_for(family);
  crash_context::begin_decode({format_name(family), label.name, label.version, 0});
  if (crash_test_armed() && crash_test_marker_present(bytes)) deliberate_crash();
}

decode_crash_scope::~decode_crash_scope() { crash_context::end_decode(); }

}  // namespace mv::codec
