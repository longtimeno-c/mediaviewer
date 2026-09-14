// SPDX-License-Identifier: GPL-2.0-or-later
// PR 7 RAW via LibRaw: detection, embedded-preview first pixel, full decode,
// orientation agreement, cancellation, truncation, original bytes unchanged.
//
// Two sources of RAW:
//  - a synthetic DNG built here (uncompressed RGGB CFA + embedded JPEG
//    preview), so a clean clone with no corpus still exercises LibRaw's open,
//    preview and full decode;
//  - real camera files from tools/testmedia/fetch-raw.ps1 (CC0, raw.pixls.us,
//    not in git). Those tests SKIP visibly when absent and FAIL under
//    MV_REQUIRE_CORPUS=1 (tests/corpus.h).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "codec/decode.h"
#include "codec/raw_internal.h"
#include "core/job_system.h"
#include "corpus.h"
#include "fixtures.h"
#include "image/pipeline.h"
#include "io/file.h"

using mv::codec::format_family;
using mv::codec::looks_like_raw;

namespace {

// ---------------------------------------------------------------------------
// Minimal little/big-endian TIFF writer
// ---------------------------------------------------------------------------

enum : std::uint16_t { kByte = 1, kAscii = 2, kShort = 3, kLong = 4, kRational = 5, kSRational = 10 };

struct ifd_entry {
  std::uint16_t tag = 0;
  std::uint16_t type = 0;
  std::uint32_t count = 0;
  std::vector<std::uint8_t> data;  // already in file byte order
};

struct tiff_builder {
  bool le = true;
  std::vector<std::uint8_t> b;

  explicit tiff_builder(bool little = true, bool cr2_marker = false) : le(little) {
    if (le) {
      b = {'I', 'I', 42, 0, 0, 0, 0, 0};
    } else {
      b = {'M', 'M', 0, 42, 0, 0, 0, 0};
    }
    if (cr2_marker) b.insert(b.end(), {'C', 'R', 2, 0, 0, 0, 0, 0});
  }

  void put16(std::vector<std::uint8_t>& v, std::uint16_t x) const {
    if (le) {
      v.push_back(static_cast<std::uint8_t>(x));
      v.push_back(static_cast<std::uint8_t>(x >> 8));
    } else {
      v.push_back(static_cast<std::uint8_t>(x >> 8));
      v.push_back(static_cast<std::uint8_t>(x));
    }
  }
  void put32(std::vector<std::uint8_t>& v, std::uint32_t x) const {
    if (le) {
      for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
    } else {
      for (int i = 3; i >= 0; --i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
    }
  }
  void patch32(std::size_t pos, std::uint32_t x) {
    std::vector<std::uint8_t> tmp;
    put32(tmp, x);
    std::copy(tmp.begin(), tmp.end(), b.begin() + static_cast<std::ptrdiff_t>(pos));
  }

  ifd_entry shorts(std::uint16_t tag, std::initializer_list<std::uint16_t> v) const {
    ifd_entry e{tag, kShort, static_cast<std::uint32_t>(v.size()), {}};
    for (auto x : v) put16(e.data, x);
    return e;
  }
  ifd_entry longs(std::uint16_t tag, std::initializer_list<std::uint32_t> v) const {
    ifd_entry e{tag, kLong, static_cast<std::uint32_t>(v.size()), {}};
    for (auto x : v) put32(e.data, x);
    return e;
  }
  ifd_entry bytes(std::uint16_t tag, std::initializer_list<std::uint8_t> v) const {
    return ifd_entry{tag, kByte, static_cast<std::uint32_t>(v.size()), std::vector<std::uint8_t>(v)};
  }
  ifd_entry ascii(std::uint16_t tag, const char* s) const {
    ifd_entry e{tag, kAscii, 0, {}};
    for (const char* p = s; *p; ++p) e.data.push_back(static_cast<std::uint8_t>(*p));
    e.data.push_back(0);
    e.count = static_cast<std::uint32_t>(e.data.size());
    return e;
  }
  ifd_entry rationals(std::uint16_t tag, bool is_signed, std::initializer_list<double> v) const {
    ifd_entry e{tag, is_signed ? kSRational : kRational, static_cast<std::uint32_t>(v.size()), {}};
    for (double x : v) {
      put32(e.data, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(x * 10000.0))));
      put32(e.data, 10000u);
    }
    return e;
  }

  void align() {
    if (b.size() & 1) b.push_back(0);
  }

  struct written {
    std::uint32_t offset = 0;
    std::vector<std::uint16_t> tags;  // sorted, parallel to entries
    [[nodiscard]] std::size_t value_pos(std::uint16_t tag) const {
      for (std::size_t i = 0; i < tags.size(); ++i) {
        if (tags[i] == tag) return offset + 2 + 12 * i + 8;
      }
      return 0;
    }
  };

  // Writes an IFD at the end of the buffer, its out-of-line data after it.
  written ifd(std::vector<ifd_entry> entries) {
    std::sort(entries.begin(), entries.end(),
              [](const ifd_entry& a, const ifd_entry& c) { return a.tag < c.tag; });
    align();
    written w;
    w.offset = static_cast<std::uint32_t>(b.size());
    std::vector<std::uint8_t> head;
    put16(head, static_cast<std::uint16_t>(entries.size()));
    b.insert(b.end(), head.begin(), head.end());
    const std::size_t table = b.size();
    b.resize(table + 12 * entries.size() + 4, 0);
    for (std::size_t i = 0; i < entries.size(); ++i) {
      const ifd_entry& e = entries[i];
      w.tags.push_back(e.tag);
      std::vector<std::uint8_t> field;
      put16(field, e.tag);
      put16(field, e.type);
      put32(field, e.count);
      std::copy(field.begin(), field.end(), b.begin() + static_cast<std::ptrdiff_t>(table + 12 * i));
      if (e.data.size() <= 4) {
        std::copy(e.data.begin(), e.data.end(),
                  b.begin() + static_cast<std::ptrdiff_t>(table + 12 * i + 8));
      } else {
        align();
        const auto off = static_cast<std::uint32_t>(b.size());
        b.insert(b.end(), e.data.begin(), e.data.end());
        patch32(table + 12 * i + 8, off);
      }
    }
    return w;
  }

  std::uint32_t blob(const std::vector<std::uint8_t>& data) {
    align();
    const auto off = static_cast<std::uint32_t>(b.size());
    b.insert(b.end(), data.begin(), data.end());
    return off;
  }

  void set_ifd0(std::uint32_t off) { patch32(4, off); }
};

// A scanner / Photoshop style RGB TIFF. Optional Make and extra IFD0 entries.
std::vector<std::uint8_t> plain_tiff(const char* make = nullptr,
                                     std::vector<ifd_entry> extra = {}, bool little = true,
                                     bool cr2_marker = false) {
  tiff_builder t(little, cr2_marker);
  std::vector<std::uint8_t> pixels(4 * 4 * 3, 128);
  const std::uint32_t strip = t.blob(pixels);
  std::vector<ifd_entry> e = {
      t.longs(256, {4}),          t.longs(257, {4}),      t.shorts(258, {8, 8, 8}),
      t.shorts(259, {1}),         t.shorts(262, {2}),     t.longs(273, {strip}),
      t.shorts(277, {3}),         t.longs(278, {4}),      t.longs(279, {48}),
      t.ascii(305, "Scanner 3000"),
  };
  if (make) e.push_back(t.ascii(271, make));
  for (auto& x : extra) e.push_back(std::move(x));
  const auto w = t.ifd(std::move(e));
  t.set_ifd0(w.offset);
  return t.b;
}

// ---------------------------------------------------------------------------
// Synthetic DNG: RGGB CFA, 16-bit uncompressed, JPEG preview in IFD0.
// ---------------------------------------------------------------------------

constexpr std::uint32_t kW = 480;
constexpr std::uint32_t kH = 320;

// Linear scene value. The bright block marks the top-left corner so a wrong
// rotation cannot correlate.
double scene(std::uint32_t x, std::uint32_t y, int c) {
  if (x < kW / 4 && y < kH / 3) return 0.8;
  const double fx = static_cast<double>(x) / (kW - 1);
  const double fy = static_cast<double>(y) / (kH - 1);
  switch (c) {
    case 0: return 0.03 + 0.5 * fx;
    case 1: return 0.03 + 0.5 * fy;
    default: return 0.03 + 0.5 * (1.0 - fx) * fy;
  }
}

std::uint8_t srgb8(double lin) {
  lin = std::clamp(lin, 0.0, 1.0);
  const double v = lin <= 0.0031308 ? lin * 12.92 : 1.055 * std::pow(lin, 1.0 / 2.4) - 0.055;
  return static_cast<std::uint8_t>(std::lround(v * 255.0));
}

struct dng_options {
  std::uint16_t orientation = 1;
  bool preview_prerotated = false;  // preview pixels already in display orientation (90° only)
  bool with_preview = true;
};

std::vector<std::uint8_t> make_dng(const dng_options& opt = {}) {
  tiff_builder t;

  // Preview pixels.
  const bool swap = opt.preview_prerotated && (opt.orientation == 6 || opt.orientation == 8);
  const std::uint32_t pw = swap ? kH : kW;
  const std::uint32_t ph = swap ? kW : kH;
  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(pw) * ph * 3);
  for (std::uint32_t y = 0; y < ph; ++y) {
    for (std::uint32_t x = 0; x < pw; ++x) {
      std::uint32_t sx = x;
      std::uint32_t sy = y;
      if (swap && opt.orientation == 6) {  // 90° CW: dst(x,y) = src(y, H-1-x)
        sx = y;
        sy = kH - 1 - x;
      } else if (swap) {  // 90° CCW: dst(x,y) = src(W-1-y, x)
        sx = kW - 1 - y;
        sy = x;
      }
      for (int c = 0; c < 3; ++c) {
        rgb[(static_cast<std::size_t>(y) * pw + x) * 3 + static_cast<std::size_t>(c)] =
            srgb8(scene(sx, sy, c));
      }
    }
  }
  const std::vector<std::uint8_t> jpeg = fixtures::jpeg_rgb(pw, ph, rgb.data());

  // CFA pixels.
  std::vector<std::uint8_t> cfa(static_cast<std::size_t>(kW) * kH * 2);
  for (std::uint32_t y = 0; y < kH; ++y) {
    for (std::uint32_t x = 0; x < kW; ++x) {
      const int c = (y % 2 == 0) ? ((x % 2 == 0) ? 0 : 1) : ((x % 2 == 0) ? 1 : 2);
      const auto v = static_cast<std::uint16_t>(std::lround(scene(x, y, c) * 60000.0));
      const std::size_t i = (static_cast<std::size_t>(y) * kW + x) * 2;
      cfa[i] = static_cast<std::uint8_t>(v);
      cfa[i + 1] = static_cast<std::uint8_t>(v >> 8);
    }
  }

  // IFD0 (preview) and the raw SubIFD first, pixel data after, like a camera.
  std::vector<ifd_entry> ifd0 = {
      t.longs(254, {opt.with_preview ? 1u : 1u}),
      t.ascii(271, "MediaViewer"),
      t.ascii(272, "Synthetic DNG"),
      t.shorts(274, {opt.orientation}),
      t.longs(330, {0}),  // SubIFDs, patched
      t.bytes(50706, {1, 4, 0, 0}),
      t.bytes(50707, {1, 1, 0, 0}),
      t.ascii(50708, "MediaViewer Synthetic DNG"),
      // XYZ(D65) -> camera: the camera is linear sRGB.
      t.rationals(50721, true,
                  {3.2406, -1.5372, -0.4986, -0.9689, 1.8758, 0.0415, 0.0557, -0.2040, 1.0570}),
      t.rationals(50728, false, {1.0, 1.0, 1.0}),
      t.shorts(50778, {21}),
  };
  if (opt.with_preview) {
    ifd0.push_back(t.longs(256, {pw}));
    ifd0.push_back(t.longs(257, {ph}));
    ifd0.push_back(t.shorts(258, {8, 8, 8}));
    ifd0.push_back(t.shorts(259, {7}));
    ifd0.push_back(t.shorts(262, {6}));
    ifd0.push_back(t.longs(273, {0}));  // patched
    ifd0.push_back(t.shorts(277, {3}));
    ifd0.push_back(t.longs(278, {ph}));
    ifd0.push_back(t.longs(279, {static_cast<std::uint32_t>(jpeg.size())}));
    ifd0.push_back(t.shorts(284, {1}));
  }
  const auto w0 = t.ifd(std::move(ifd0));
  t.set_ifd0(w0.offset);

  const auto wraw = t.ifd({
      t.longs(254, {0}),
      t.longs(256, {kW}),
      t.longs(257, {kH}),
      t.shorts(258, {16}),
      t.shorts(259, {1}),
      t.shorts(262, {32803}),
      t.longs(273, {0}),  // patched
      t.shorts(277, {1}),
      t.longs(278, {kH}),
      t.longs(279, {static_cast<std::uint32_t>(cfa.size())}),
      t.shorts(284, {1}),
      t.shorts(33421, {2, 2}),
      t.bytes(33422, {0, 1, 1, 2}),
      t.longs(50714, {0}),
      t.longs(50717, {65535}),
  });
  t.patch32(w0.value_pos(330), wraw.offset);

  if (opt.with_preview) t.patch32(w0.value_pos(273), t.blob(jpeg));
  t.patch32(wraw.value_pos(273), t.blob(cfa));
  return t.b;
}

// ---------------------------------------------------------------------------
// Image comparison
// ---------------------------------------------------------------------------

std::vector<double> luma_grid(const std::vector<std::uint8_t>& rgba, std::uint32_t w,
                              std::uint32_t h, int n = 24) {
  std::vector<double> sum(static_cast<std::size_t>(n) * n, 0.0);
  std::vector<double> cnt(sum.size(), 0.0);
  const std::uint32_t step = std::max<std::uint32_t>(1, std::min(w, h) / 256);
  for (std::uint32_t y = 0; y < h; y += step) {
    const auto gy = static_cast<std::size_t>(static_cast<std::uint64_t>(y) * n / h);
    for (std::uint32_t x = 0; x < w; x += step) {
      const auto gx = static_cast<std::size_t>(static_cast<std::uint64_t>(x) * n / w);
      const const std::uint8_t* p = rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4;
      sum[gy * n + gx] += 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
      cnt[gy * n + gx] += 1.0;
    }
  }
  for (std::size_t i = 0; i < sum.size(); ++i) sum[i] = cnt[i] > 0 ? sum[i] / cnt[i] : 0.0;
  return sum;
}

double pearson(const std::vector<double>& a, const std::vector<double>& b) {
  const double n = static_cast<double>(a.size());
  double ma = 0;
  double mb = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    ma += a[i];
    mb += b[i];
  }
  ma /= n;
  mb /= n;
  double sab = 0;
  double saa = 0;
  double sbb = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    sab += (a[i] - ma) * (b[i] - mb);
    saa += (a[i] - ma) * (a[i] - ma);
    sbb += (b[i] - mb) * (b[i] - mb);
  }
  return (saa > 0 && sbb > 0) ? sab / std::sqrt(saa * sbb) : 0.0;
}

double mean(const std::vector<double>& v) {
  double s = 0;
  for (double x : v) s += x;
  return v.empty() ? 0.0 : s / static_cast<double>(v.size());
}

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// FNV-1a over the file's bytes, independent of mv::io.
std::uint64_t file_hash(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::uint64_t h = 1469598103934665603ull;
  char buf[1 << 16];
  while (in) {
    in.read(buf, sizeof(buf));
    const std::streamsize n = in.gcount();
    for (std::streamsize i = 0; i < n; ++i) {
      h ^= static_cast<std::uint8_t>(buf[i]);
      h *= 1099511628211ull;
    }
  }
  return h;
}

std::set<std::string> dir_listing(const std::filesystem::path& dir) {
  std::set<std::string> out;
  for (const auto& e : std::filesystem::directory_iterator(dir)) out.insert(e.path().filename().string());
  return out;
}

constexpr const char* kSamples[] = {
    "canon_eos7dmk2.cr2", "nikon_d7500.nef", "sony_ilce7rm3.arw",
    "canon_eosr6.cr3",    "pentax_k50.dng",
};

// Declares `var` = path of tools/testmedia/raw/<name>, or SKIPs (FAILs under
// MV_REQUIRE_CORPUS=1). A macro: SKIP/FAIL return from the test case.
#define MV_REQUIRE_RAW_SAMPLE(var, name)                                                  \
  const std::string var = ::corpus::path_of((std::string("raw/") + (name)).c_str());      \
  if (var.empty()) {                                                                      \
    if (::corpus::corpus_required()) {                                                    \
      FAIL("MV_REQUIRE_CORPUS is set and tools/testmedia/raw/" << (name)                  \
           << " is missing. Run tools/testmedia/fetch-raw.ps1.");                         \
    }                                                                                     \
    SKIP("tools/testmedia/raw/" << (name) << " not fetched - run "                        \
         "tools/testmedia/fetch-raw.ps1. This test proved NOTHING.");                     \
  }

}  // namespace

// ===========================================================================
// Detection
// ===========================================================================

TEST_CASE("looks_like_raw: camera TIFF-container RAWs yes, plain TIFFs no", "[codec][raw]") {
  tiff_builder t;  // only for entry helpers

  SECTION("plain scanner / Photoshop TIFF is not RAW") {
    const auto tif = plain_tiff();
    REQUIRE(mv::codec::probe(tif) == format_family::tiff);
    REQUIRE_FALSE(looks_like_raw(tif));
    REQUIRE_FALSE(looks_like_raw(plain_tiff(nullptr, {}, false)));
  }
  SECTION("a camera-made RGB TIFF (Make only, no raw structure) is not RAW") {
    REQUIRE_FALSE(looks_like_raw(plain_tiff("NIKON CORPORATION")));
    REQUIRE_FALSE(looks_like_raw(plain_tiff("Canon")));
  }
  SECTION("raw structure from a non-camera maker is not RAW") {
    REQUIRE_FALSE(looks_like_raw(plain_tiff("EPSON", {t.longs(330, {8})})));
  }
  SECTION("DNG by DNGVersion") {
    REQUIRE(looks_like_raw(plain_tiff(nullptr, {t.bytes(50706, {1, 4, 0, 0})})));
    REQUIRE(looks_like_raw(make_dng()));
  }
  SECTION("CR2 by the CR marker") {
    REQUIRE(looks_like_raw(plain_tiff("Canon", {}, true, true)));
  }
  SECTION("NEF / ARW / PEF shapes by maker + structure") {
    REQUIRE(looks_like_raw(plain_tiff("NIKON CORPORATION", {t.longs(330, {8})})));
    REQUIRE(looks_like_raw(plain_tiff("NIKON", {t.longs(254, {1})})));
    tiff_builder be(false);
    REQUIRE(looks_like_raw(plain_tiff("NIKON", {be.longs(330, {8})}, false)));
    REQUIRE(looks_like_raw(plain_tiff("SONY", {t.longs(50740, {8})})));
    REQUIRE(looks_like_raw(plain_tiff("PENTAX Corporation", {t.shorts(259, {65535})})));
  }
  SECTION("CFA photometric is RAW regardless of maker") {
    tiff_builder cfa;
    std::vector<std::uint8_t> px(16, 0);
    const std::uint32_t strip = cfa.blob(px);
    const auto w = cfa.ifd({cfa.longs(256, {4}), cfa.longs(257, {4}), cfa.shorts(258, {8}),
                            cfa.shorts(262, {32803}), cfa.longs(273, {strip}),
                            cfa.longs(279, {16})});
    cfa.set_ifd0(w.offset);
    REQUIRE(looks_like_raw(cfa.b));
  }
  SECTION("non-TIFF RAW magics and non-RAW formats") {
    const std::uint8_t orf[16] = {'I', 'I', 'R', 'O', 8, 0, 0, 0};
    REQUIRE(looks_like_raw(orf));
    std::uint8_t cr3[16] = {0, 0, 0, 24, 'f', 't', 'y', 'p', 'c', 'r', 'x', ' '};
    REQUIRE(looks_like_raw(cr3));
    const std::uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0, 0, 0, 0};
    REQUIRE_FALSE(looks_like_raw(jpeg));
    REQUIRE_FALSE(looks_like_raw({}));
  }
  SECTION("hostile IFD offsets and counts are bounds-checked") {
    auto tif = plain_tiff("NIKON");
    auto bad_offset = tif;
    bad_offset[4] = 0xFF;
    bad_offset[5] = 0xFF;
    bad_offset[6] = 0xFF;
    bad_offset[7] = 0x7F;
    REQUIRE_FALSE(looks_like_raw(bad_offset));
    const std::uint8_t tiny[] = {'I', 'I', 42, 0, 8, 0, 0, 0, 0xFF, 0xFF};
    REQUIRE_FALSE(looks_like_raw(tiny));

    std::mt19937 rng(1234);
    for (int i = 0; i < 20000; ++i) {
      std::vector<std::uint8_t> junk(16 + rng() % 300);
      for (auto& b : junk) b = static_cast<std::uint8_t>(rng());
      junk[0] = 'I';
      junk[1] = 'I';
      junk[2] = 42;
      junk[3] = 0;
      junk[4] = static_cast<std::uint8_t>(rng() % 32);
      junk[5] = junk[6] = junk[7] = 0;
      (void)looks_like_raw(junk);
      // Truncations of a real-shaped header too.
      (void)looks_like_raw(std::span<const std::uint8_t>(tif.data(), rng() % tif.size()));
    }
    SUCCEED("20000 malformed headers probed without a crash");
  }
}

// ===========================================================================
// Synthetic DNG: preview and full decode
// ===========================================================================

TEST_CASE("synthetic DNG: embedded preview and full decode agree", "[codec][raw]") {
  const auto dng = make_dng();
  auto preview = mv::codec::decode_raw_preview(dng);
  REQUIRE(preview);
  auto full = mv::codec::decode_raw(dng);
  REQUIRE(full);

  CHECK(preview->format == format_family::raw);
  CHECK(full->format == format_family::raw);
  CHECK(preview->intent == mv::codec::transfer_intent::display_referred);
  CHECK(full->intent == mv::codec::transfer_intent::display_referred);
  CHECK(preview->tagged_srgb);
  CHECK(full->tagged_srgb);
  CHECK(full->icc.empty());

  REQUIRE(preview->width == kW);
  REQUIRE(preview->height == kH);
  REQUIRE(full->width == kW);
  REQUIRE(full->height == kH);
  const auto gp = luma_grid(preview->rgba, preview->width, preview->height);
  const auto gf = luma_grid(full->rgba, full->width, full->height);
  CHECK(pearson(gp, gf) > 0.9);
}

TEST_CASE("RAW preview comes out in the full decode's orientation", "[codec][raw]") {
  const std::uint16_t orientation = GENERATE(as<std::uint16_t>{}, 3, 6, 8);
  const bool prerotated = GENERATE(false, true);
  if (prerotated && orientation == 3) SKIP("a 180° preview cannot be told apart; not claimed");
  CAPTURE(orientation, prerotated);

  dng_options opt;
  opt.orientation = orientation;
  opt.preview_prerotated = prerotated;
  const auto dng = make_dng(opt);
  auto preview = mv::codec::decode_raw_preview(dng);
  REQUIRE(preview);
  auto full = mv::codec::decode_raw(dng);
  REQUIRE(full);

  const bool swapped = orientation == 6 || orientation == 8;
  REQUIRE(full->width == (swapped ? kH : kW));
  REQUIRE(full->height == (swapped ? kW : kH));
  REQUIRE(preview->width == full->width);
  REQUIRE(preview->height == full->height);

  const auto gp = luma_grid(preview->rgba, preview->width, preview->height);
  const auto gf = luma_grid(full->rgba, full->width, full->height);
  auto gf180 = gf;
  std::reverse(gf180.begin(), gf180.end());
  const double same = pearson(gp, gf);
  const double rotated = pearson(gp, gf180);
  CAPTURE(same, rotated);
  CHECK(same > 0.9);
  CHECK(same > rotated + 0.2);
}

TEST_CASE("pipeline routes a DNG through the RAW preview, then the full decode", "[codec][raw]") {
  dng_options opt;
  opt.orientation = 6;
  const auto dng = make_dng(opt);
  auto preview = mv::image::decode_preview(dng);
  REQUIRE(preview);
  auto full = mv::image::decode_bytes(dng);
  REQUIRE(full);
  CHECK(preview->width == full->width);
  CHECK(preview->height == full->height);
  CHECK(full->format == format_family::raw);
}

TEST_CASE("a RAW without an embedded preview is unsupported for preview only", "[codec][raw]") {
  dng_options opt;
  opt.with_preview = false;
  const auto dng = make_dng(opt);
  REQUIRE(looks_like_raw(dng));
  auto preview = mv::codec::decode_raw_preview(dng);
  REQUIRE_FALSE(preview);
  CHECK(preview.error() == mv::status::unsupported_format);
  auto full = mv::codec::decode_raw(dng);
  REQUIRE(full);
  CHECK(full->width == kW);
}

TEST_CASE("RAW decoders reject what is not RAW", "[codec][raw]") {
  const auto tif = plain_tiff();
  auto p = mv::codec::decode_raw_preview(tif);
  REQUIRE_FALSE(p);
  CHECK(p.error() == mv::status::unsupported_format);
  auto f = mv::codec::decode_raw(tif);
  REQUIRE_FALSE(f);
  CHECK(f.error() == mv::status::unsupported_format);
}

TEST_CASE("a cancelled RAW decode returns cancelled", "[codec][raw]") {
  const auto dng = make_dng();
  std::atomic<mv::generation> current{2};
  mv::job_context ctx(1, 1, &current, 0);
  auto p = mv::codec::decode_raw_preview(dng, &ctx);
  REQUIRE_FALSE(p);
  CHECK(p.error() == mv::status::cancelled);
  auto f = mv::codec::decode_raw(dng, &ctx);
  REQUIRE_FALSE(f);
  CHECK(f.error() == mv::status::cancelled);
}

TEST_CASE("a truncated synthetic RAW is an error, never a crash or hang", "[codec][raw]") {
  const auto dng = make_dng();
  // Cut inside the header, the IFDs, the preview, and the CFA strip.
  for (double frac : {0.001, 0.005, 0.05, 0.3, 0.6, 0.9, 0.999}) {
    const auto n = static_cast<std::size_t>(static_cast<double>(dng.size()) * frac);
    CAPTURE(frac, n);
    const std::span<const std::uint8_t> cut(dng.data(), n);
    const auto t0 = std::chrono::steady_clock::now();
    auto p = mv::codec::decode_raw_preview(cut);
    auto f = mv::codec::decode_raw(cut);
    CHECK(ms_since(t0) < 5000.0);
    // Pixel data is the last thing in the file, so every cut loses some.
    CHECK_FALSE(f);
    if (!f) CHECK(f.error() != mv::status::ok);
    (void)p;  // the preview may legitimately survive a cut after it
  }
}

TEST_CASE("decoding a RAW never modifies the original or writes beside it", "[codec][raw]") {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("mv_test_raw_" + std::to_string(std::chrono::steady_clock::now()
                                                         .time_since_epoch()
                                                         .count()));
  std::filesystem::create_directories(dir);
  const auto path = dir / "IMG_0001.DNG";
  {
    const auto dng = make_dng({6, false, true});
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(dng.data()), static_cast<std::streamsize>(dng.size()));
  }
  const auto before_hash = file_hash(path);
  const auto before_time = std::filesystem::last_write_time(path);
  const auto before_list = dir_listing(dir);

  auto bytes = mv::io::read_all(path.string());
  REQUIRE(bytes);
  auto preview = mv::image::decode_preview(bytes.value());
  REQUIRE(preview);
  auto full = mv::image::decode_bytes(bytes.value());
  REQUIRE(full);

  CHECK(file_hash(path) == before_hash);
  CHECK(std::filesystem::last_write_time(path) == before_time);
  CHECK(dir_listing(dir) == before_list);
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

// ===========================================================================
// Real camera files (tools/testmedia/fetch-raw.ps1)
// ===========================================================================

TEST_CASE("camera RAW: preview first, full decode replaces it in place, bytes unchanged",
          "[codec][raw][corpus]") {
  const char* name = GENERATE(from_range(std::begin(kSamples), std::end(kSamples)));
  MV_REQUIRE_RAW_SAMPLE(path, name);
  CAPTURE(name);

  const std::filesystem::path fs_path(path);
  const auto before_hash = file_hash(fs_path);
  const auto before_time = std::filesystem::last_write_time(fs_path);
  const auto before_list = dir_listing(fs_path.parent_path());

  auto bytes = mv::io::read_all(path);
  REQUIRE(bytes);
  REQUIRE(looks_like_raw(bytes.value()));

  // Warm the DLL and page cache so the numbers are decode, not first-touch.
  (void)mv::image::decode_preview(bytes.value());

  auto t0 = std::chrono::steady_clock::now();
  auto preview = mv::image::decode_preview(bytes.value());
  const double preview_ms = ms_since(t0);
  REQUIRE(preview);

  t0 = std::chrono::steady_clock::now();
  auto full = mv::image::decode_bytes(bytes.value());
  const double full_ms = ms_since(t0);
  REQUIRE(full);

  // Same-size JPEG through the JPEG first-pixel path, for the verify line's
  // "preview time comparable to a JPEG".
  auto jpeg = mv::codec::encode_jpeg_rgba(full->rgba, full->width, full->height, 92);
  REQUIRE(jpeg);
  (void)mv::image::decode_preview(jpeg.value());
  t0 = std::chrono::steady_clock::now();
  auto jpeg_preview = mv::image::decode_preview(jpeg.value());
  const double jpeg_preview_ms = ms_since(t0);
  REQUIRE(jpeg_preview);

  const auto gp = luma_grid(preview->rgba, preview->width, preview->height);
  const auto gf = luma_grid(full->rgba, full->width, full->height);
  const double corr = pearson(gp, gf);
  std::printf("[raw] %-20s full %5ux%-5u  preview %5ux%-5u  preview %6.1f ms  full %7.1f ms  "
              "jpeg-preview %5.1f ms  corr %.3f  luma preview %.1f full %.1f\n",
              name, full->width, full->height, preview->width, preview->height, preview_ms,
              full_ms, jpeg_preview_ms, corr, mean(gp), mean(gf));

  // Replacement without a pop: same aspect (the pipeline scales the preview
  // to the full frame), same orientation, same picture.
  const double ar_preview = static_cast<double>(preview->width) / preview->height;
  const double ar_full = static_cast<double>(full->width) / full->height;
  CHECK(std::abs(std::log(ar_preview / ar_full)) < 0.03);
  CHECK(corr > 0.85);

  CHECK(file_hash(fs_path) == before_hash);
  CHECK(std::filesystem::last_write_time(fs_path) == before_time);
  CHECK(dir_listing(fs_path.parent_path()) == before_list);
}

TEST_CASE("camera RAW: cancelling a full decode returns promptly", "[codec][raw][corpus]") {
  const char* name = GENERATE(from_range(std::begin(kSamples), std::end(kSamples)));
  MV_REQUIRE_RAW_SAMPLE(path, name);
  CAPTURE(name);
  auto bytes = mv::io::read_all(path);
  REQUIRE(bytes);

  for (int delay_ms : {0, 40, 150}) {
    CAPTURE(delay_ms);
    std::atomic<mv::generation> current{1};
    mv::job_context ctx(1, 1, &current, 0);
    std::chrono::steady_clock::time_point cancelled_at{};
    std::thread canceller([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
      cancelled_at = std::chrono::steady_clock::now();
      current.store(2);
    });
    auto r = mv::codec::decode_raw(bytes.value(), &ctx);
    const auto returned_at = std::chrono::steady_clock::now();
    canceller.join();
    if (r) continue;  // finished before the cancel landed
    CHECK(r.error() == mv::status::cancelled);
    const double latency =
        std::chrono::duration<double, std::milli>(returned_at - cancelled_at).count();
    std::printf("[raw] %-20s cancel after %3d ms -> returned %.1f ms later\n", name, delay_ms,
                latency);
    CHECK(latency < 400.0);
  }
}

TEST_CASE("camera RAW: truncated files are errors, not crashes or hangs", "[codec][raw][corpus]") {
  const char* name = GENERATE(from_range(std::begin(kSamples), std::end(kSamples)));
  MV_REQUIRE_RAW_SAMPLE(path, name);
  CAPTURE(name);
  auto bytes = mv::io::read_all(path);
  REQUIRE(bytes);
  for (double frac : {0.02, 0.5, 0.9}) {
    CAPTURE(frac);
    const std::span<const std::uint8_t> cut(
        bytes->data(), static_cast<std::size_t>(static_cast<double>(bytes->size()) * frac));
    const auto t0 = std::chrono::steady_clock::now();
    auto p = mv::codec::decode_raw_preview(cut);
    auto f = mv::codec::decode_raw(cut);
    const double elapsed = ms_since(t0);
    std::printf("[raw] %-20s cut %.2f: preview %s, full %s (%.0f ms)\n", name, frac,
                p ? "ok" : mv::status_name(p.error()), f ? "ok" : mv::status_name(f.error()),
                elapsed);
    CHECK(elapsed < 10000.0);
    CHECK_FALSE(f);
  }
}

// Not part of the normal run: `mv_tests "[.raw-bench]"`. The measurements
// behind raw.cpp's defaults (demosaic, auto-bright, preview scale).
TEST_CASE("RAW decode choices benchmark", "[.raw-bench]") {
  using mv::codec::raw_detail::demosaic;
  using mv::codec::raw_detail::raw_options;
  for (const char* name : kSamples) {
    const std::string path = ::corpus::path_of((std::string("raw/") + name).c_str());
    if (path.empty()) {
      std::printf("[bench] %s missing\n", name);
      continue;
    }
    auto bytes = mv::io::read_all(path);
    REQUIRE(bytes);

    for (std::uint32_t side : {0u, 2560u, 1280u}) {
      raw_options o = mv::codec::raw_detail::default_options();
      o.preview_min_long_side = side;
      (void)mv::codec::raw_detail::decode_raw_preview_with(bytes.value(), nullptr, o);
      const auto t0 = std::chrono::steady_clock::now();
      auto p = mv::codec::raw_detail::decode_raw_preview_with(bytes.value(), nullptr, o);
      const double ms = ms_since(t0);
      std::printf("[bench] %-20s preview min_long_side=%4u -> %s %ux%u %.1f ms\n", name, side,
                  p ? "ok" : mv::status_name(p.error()), p ? p->width : 0u, p ? p->height : 0u,
                  ms);
    }

    auto preview = mv::codec::decode_raw_preview(bytes.value());
    const auto gp = preview ? luma_grid(preview->rgba, preview->width, preview->height)
                            : std::vector<double>{};
    for (demosaic q : {demosaic::linear, demosaic::vng, demosaic::ppg, demosaic::ahd}) {
      for (bool bright : {true, false}) {
        if (!bright && q != demosaic::ahd) continue;
        raw_options o = mv::codec::raw_detail::default_options();
        o.quality = q;
        o.auto_bright = bright;
        const auto t0 = std::chrono::steady_clock::now();
        auto f = mv::codec::raw_detail::decode_raw_with(bytes.value(), nullptr, o);
        const double ms = ms_since(t0);
        if (!f) {
          std::printf("[bench] %-20s q=%d bright=%d -> %s\n", name, static_cast<int>(q),
                      bright ? 1 : 0, mv::status_name(f.error()));
          continue;
        }
        const auto gf = luma_grid(f->rgba, f->width, f->height);
        std::printf("[bench] %-20s q=%d auto_bright=%d %ux%u %7.1f ms  luma full %.1f preview %.1f "
                    "corr %.3f\n",
                    name, static_cast<int>(q), bright ? 1 : 0, f->width, f->height, ms, mean(gf),
                    gp.empty() ? 0.0 : mean(gp), gp.empty() ? 0.0 : pearson(gp, gf));
      }
    }
  }
}
