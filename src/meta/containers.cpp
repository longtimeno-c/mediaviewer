// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #290: container-level metadata Exiv2 does not list (docs/design/06 "Beyond Exiv2"):
// JFIF and the JPEG comment, every PNG text chunk, WebP's feature chunks, HEIF's
// items and properties, the ICC profile's own tags, and where each container
// keeps its C2PA store. Bounded walks over spans; see parse.h.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "meta/parse.h"
#include "meta/parse_util.h"

namespace mv::meta::parse {
namespace {

using util::bytes;

constexpr std::size_t kMaxTextChunkBytes = 1u << 20;   // one inflated chunk
constexpr std::size_t kMaxTextTotalBytes = 8u << 20;   // all of a file's inflated text
constexpr std::size_t kTextCap = 2048;                 // characters shown per text value
constexpr std::size_t kMaxChunks = 100000;

std::string fmt(const char* f, double a) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), f, a);
  return buf;
}

// ---- PNG -------------------------------------------------------------------------

bool json_like(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\n' || s.front() == '\r' || s.front() == '\t')) {
    s.remove_prefix(1);
  }
  return !s.empty() && (s.front() == '{' || s.front() == '[');
}

bool equals_nocase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const auto lo = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    if (lo(a[i]) != lo(b[i])) return false;
  }
  return true;
}

void png_text(sink& out, std::string_view chunk, std::string_view keyword, const std::string& text,
              std::string_view lang) {
  // XMP and Exiv2's "Raw profile type …" chunks are Exiv2's to list.
  if (keyword == "XML:com.adobe.xmp" || keyword.rfind("Raw profile type", 0) == 0) return;
  const std::string key(latin1_or_utf8(keyword));
  // Generator evidence (generators.h): A1111 / Forge / SD.Next write `parameters`;
  // ComfyUI writes `prompt` and `workflow` as JSON.
  if (key == "parameters" && !text.empty()) {
    out.ev.png_parameters = true;
    // "…, Model: sd_xl_base_1.0, Version: v1.6.0" — the tail names the tool.
    const auto v = text.rfind("Version:");
    if (v != std::string::npos && out.ev.png_generator_text.size() < 8) {
      out.ev.png_generator_text.push_back(text.substr(v, 64));
    }
  }
  if ((key == "prompt" || key == "workflow") && json_like(text)) out.ev.png_comfyui = true;
  if (equals_nocase(key, "Software") && out.ev.png_software.empty()) out.ev.png_software = text.substr(0, 256);
  if ((equals_nocase(key, "Generator") || equals_nocase(key, "Source") || equals_nocase(key, "Model")) &&
      out.ev.png_generator_text.size() < 8) {
    out.ev.png_generator_text.push_back(text.substr(0, 256));
  }
  std::string label = key;
  if (!lang.empty()) label += " (" + std::string(lang) + ")";
  (void)out.add(origin::png_text, "PNG text", key, label, text, "PNG." + std::string(chunk) + "." + key, kTextCap);
}

}  // namespace

void read_png(std::span<const std::uint8_t> png, sink& out) {
  try {
    static constexpr std::uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (png.size() < 8 || std::memcmp(png.data(), kSig, 8) != 0) return;
    std::size_t at = 8;
    std::size_t inflated_total = 0;
    for (std::size_t n = 0; n < kMaxChunks && util::has(png, at, 12) && !out.full(); ++n) {
      const std::size_t len = util::be32(png, at);
      if (len > 0x7FFFFFFF || !util::has(png, at + 8, len)) break;  // truncated: keep what was read
      const std::string type = util::fourcc(png, at + 4);
      const bytes data = png.subspan(at + 8, len);
      at += 12 + len;
      if (type == "IEND") break;
      if (type == "tEXt") {
        std::size_t next = 0;
        const std::string_view key = util::cstr(data, 0, next);
        png_text(out, type, key, latin1_or_utf8(util::chars(data.subspan(next))), {});
      } else if (type == "zTXt") {
        std::size_t next = 0;
        const std::string_view key = util::cstr(data, 0, next);
        if (next >= data.size() || data[next] != 0) continue;  // method 0 = zlib, the only one
        std::string text;
        const bool ok = inflated_total < kMaxTextTotalBytes &&
                        inflate_bounded(data.subspan(next + 1), kMaxTextChunkBytes, text);
        inflated_total += text.size();
        if (!ok && text.empty()) continue;
        png_text(out, type, key, latin1_or_utf8(text), {});
      } else if (type == "iTXt") {
        std::size_t next = 0;
        const std::string_view key = util::cstr(data, 0, next);
        if (!util::has(data, next, 2)) continue;
        const bool compressed = data[next] != 0;
        const std::uint8_t method = data[next + 1];
        std::size_t p = next + 2;
        const std::string_view lang = util::cstr(data, p, p);
        (void)util::cstr(data, p, p);  // the translated keyword
        const bytes body = data.subspan(std::min(p, data.size()));
        std::string text;
        if (compressed) {
          if (method != 0) continue;
          const bool ok = inflated_total < kMaxTextTotalBytes && inflate_bounded(body, kMaxTextChunkBytes, text);
          inflated_total += text.size();
          if (!ok && text.empty()) continue;
        } else {
          text.assign(util::chars(body));
        }
        png_text(out, type, key, text, lang);
      } else if (type == "caBX") {
        read_c2pa(data, out);
      } else if (type == "pHYs" && len >= 9) {
        const std::uint32_t x = util::be32(data, 0), y = util::be32(data, 4);
        const bool metre = data[8] == 1;
        std::string v = metre ? fmt("%.0f", x * 0.0254) + " \xC3\x97 " + fmt("%.0f", y * 0.0254) + " pixels per inch"
                              : std::to_string(x) + " : " + std::to_string(y) + " (aspect only)";
        (void)out.add(origin::container, "PNG", "pHYs", "Pixel density", v, "PNG.pHYs");
      } else if (type == "sRGB" && len >= 1) {
        static constexpr const char* kIntent[] = {"Perceptual", "Relative colorimetric", "Saturation",
                                                  "Absolute colorimetric"};
        (void)out.add(origin::container, "PNG", "sRGB", "sRGB rendering intent",
                      data[0] < 4 ? kIntent[data[0]] : "Unknown", "PNG.sRGB");
      } else if (type == "gAMA" && len >= 4) {
        const std::uint32_t g = util::be32(data, 0);
        if (g > 0) (void)out.add(origin::container, "PNG", "gAMA", "Gamma", fmt("%.5g", g / 100000.0), "PNG.gAMA");
      } else if (type == "acTL" && len >= 8) {
        (void)out.add(origin::container, "PNG", "acTL", "Animation frames", std::to_string(util::be32(data, 0)),
                      "PNG.acTL.frames");
      }
    }
  } catch (...) {
  }
}

// ---- JPEG ------------------------------------------------------------------------

void read_jpeg(std::span<const std::uint8_t> jpeg, sink& out) {
  try {
    if (jpeg.size() < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) return;
    std::size_t at = 2;
    std::size_t comments = 0;
    for (int guard = 0; guard < 4096 && util::has(jpeg, at, 4) && !out.full(); ++guard) {
      if (jpeg[at] != 0xFF) break;
      const std::uint8_t marker = jpeg[at + 1];
      if (marker == 0xFF) {
        ++at;
        continue;
      }
      if (marker == 0xD9 || marker == 0xDA) break;
      if ((marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
        at += 2;
        continue;
      }
      const std::size_t len = util::be16(jpeg, at + 2);
      if (len < 2 || !util::has(jpeg, at + 2, len)) break;
      const bytes seg = jpeg.subspan(at + 4, len - 2);
      at += 2 + len;
      if (marker == 0xE0 && seg.size() >= 14 && std::memcmp(seg.data(), "JFIF\0", 5) == 0) {
        (void)out.add(origin::container, "JFIF", "JFIFVersion", "JFIF version",
                      std::to_string(seg[5]) + "." + (seg[6] < 10 ? "0" : "") + std::to_string(seg[6]),
                      "JFIF.Version");
        const std::uint16_t xd = util::be16(seg, 8), yd = util::be16(seg, 10);
        const char* unit = seg[7] == 1 ? " dpi" : seg[7] == 2 ? " dots per cm" : " (aspect only)";
        (void)out.add(origin::container, "JFIF", "Density", "Density",
                      std::to_string(xd) + " \xC3\x97 " + std::to_string(yd) + unit, "JFIF.Density");
        if (seg[12] > 0 && seg[13] > 0) {
          (void)out.add(origin::container, "JFIF", "Thumbnail", "Thumbnail",
                        std::to_string(seg[12]) + " \xC3\x97 " + std::to_string(seg[13]), "JFIF.Thumbnail");
        }
      } else if (marker == 0xE0 && seg.size() >= 6 && std::memcmp(seg.data(), "JFXX\0", 5) == 0) {
        const char* kind = seg[5] == 0x10 ? "JPEG" : seg[5] == 0x11 ? "1 byte per pixel" : seg[5] == 0x13 ? "3 bytes per pixel" : "unknown";
        (void)out.add(origin::container, "JFIF", "JFXX", "Extension thumbnail", kind, "JFIF.JFXX");
      } else if (marker == 0xFE && comments < 16) {
        ++comments;
        (void)out.add(origin::container, "JPEG", "Comment", "Comment", latin1_or_utf8(util::chars(seg)),
                      "JPEG.COM" + (comments > 1 ? std::to_string(comments) : std::string{}), kTextCap);
      } else if (marker == 0xEE && seg.size() >= 12 && std::memcmp(seg.data(), "Adobe", 5) == 0) {
        const char* t = seg[11] == 0 ? "None (RGB or CMYK)" : seg[11] == 1 ? "YCbCr" : seg[11] == 2 ? "YCCK" : "Unknown";
        (void)out.add(origin::container, "JPEG", "AdobeTransform", "Adobe colour transform", t, "JPEG.APP14.Transform");
      }
    }
    const std::vector<std::uint8_t> store = jpeg_jumbf(jpeg);
    if (!store.empty()) read_c2pa(store, out);
  } catch (...) {
  }
}

// ---- WebP ------------------------------------------------------------------------

void read_webp(std::span<const std::uint8_t> webp, sink& out) {
  try {
    if (webp.size() < 12 || !util::fourcc_is(webp, 0, "RIFF") || !util::fourcc_is(webp, 8, "WEBP")) return;
    std::size_t at = 12;
    std::size_t frames = 0;
    std::string bitstream;
    for (std::size_t n = 0; n < kMaxChunks && util::has(webp, at, 8); ++n) {
      const std::string type = util::fourcc(webp, at);
      const std::size_t len = util::le32(webp, at + 4);
      if (!util::has(webp, at + 8, len)) break;
      const bytes data = webp.subspan(at + 8, len);
      at += 8 + len + (len & 1);
      if (type == "VP8X" && len >= 10) {
        const std::uint8_t f = data[0];
        std::string features;
        const auto add = [&](bool on, const char* name) {
          if (on) features += (features.empty() ? "" : ", ") + std::string(name);
        };
        add(f & 0x02, "Animation");
        add(f & 0x10, "Alpha");
        add(f & 0x20, "ICC profile");
        add(f & 0x08, "EXIF");
        add(f & 0x04, "XMP");
        (void)out.add(origin::container, "WebP", "Features", "Features", features.empty() ? "None" : features, "WebP.VP8X.Features");
        (void)out.add(origin::container, "WebP", "Canvas", "Canvas",
                      std::to_string(util::le24(data, 4) + 1) + " \xC3\x97 " + std::to_string(util::le24(data, 7) + 1),
                      "WebP.VP8X.Canvas");
      } else if (type == "ANIM" && len >= 6) {
        const std::uint16_t loops = util::le16(data, 4);
        (void)out.add(origin::container, "WebP", "LoopCount", "Loop count", loops == 0 ? "Forever" : std::to_string(loops),
                      "WebP.ANIM.LoopCount");
        char bg[16];
        std::snprintf(bg, sizeof(bg), "#%02X%02X%02X%02X", data[2], data[1], data[0], data[3]);
        (void)out.add(origin::container, "WebP", "Background", "Background colour (RGBA)", bg, "WebP.ANIM.Background");
      } else if (type == "ANMF") {
        ++frames;
      } else if ((type == "VP8 " || type == "VP8L") && bitstream.empty()) {
        bitstream = type == "VP8L" ? "Lossless (VP8L)" : "Lossy (VP8)";
      } else if (type == "ALPH" && len >= 1) {
        (void)out.add(origin::container, "WebP", "AlphaCompression", "Alpha compression",
                      (data[0] & 0x03) == 1 ? "Lossless" : "None", "WebP.ALPH.Compression");
      }
    }
    if (frames > 0) (void)out.add(origin::container, "WebP", "Frames", "Frames", std::to_string(frames), "WebP.ANMF.Count");
    if (!bitstream.empty()) (void)out.add(origin::container, "WebP", "Compression", "Compression", bitstream, "WebP.Compression");
  } catch (...) {
  }
}

// ---- ISO-BMFF --------------------------------------------------------------------

namespace {

// C2PA's uuid box type (C2PA 2.x, "Embedding manifests into BMFF").
constexpr std::uint8_t kC2paUuid[16] = {0xD8, 0xFE, 0xC3, 0xD6, 0x1B, 0x0E, 0x48, 0x3C,
                                        0x92, 0x97, 0x58, 0x28, 0x87, 0x7E, 0xC4, 0x81};

struct bbox {
  std::string type;
  bytes payload;
};

bool next_bbox(bytes b, std::size_t& at, bbox& out) {
  if (!util::has(b, at, 8)) return false;
  std::uint64_t size = util::be32(b, at);
  std::size_t header = 8;
  if (size == 1) {
    if (!util::has(b, at, 16)) return false;
    size = util::be64(b, at + 8);
    header = 16;
  } else if (size == 0) {
    size = b.size() - at;
  }
  if (size < header || size > b.size() - at) return false;
  out.type = util::fourcc(b, at + 4);
  out.payload = b.subspan(at + header, static_cast<std::size_t>(size) - header);
  at += static_cast<std::size_t>(size);
  return true;
}

const char* aux_name(std::string_view urn) {
  if (urn.find("auxid:1") != std::string_view::npos || urn.find("alpha") != std::string_view::npos) return "alpha";
  if (urn.find("auxid:2") != std::string_view::npos || urn.find("depth") != std::string_view::npos) return "depth";
  if (urn.find("hdrgainmap") != std::string_view::npos) return "HDR gain map";
  if (urn.find("matte") != std::string_view::npos) return "portrait matte";
  return nullptr;
}

const char* primaries_name(unsigned v) {
  switch (v) {
    case 1: return "BT.709";
    case 5: case 6: return "BT.601";
    case 9: return "BT.2020";
    case 12: return "Display P3";
    default: return nullptr;
  }
}
const char* transfer_name(unsigned v) {
  switch (v) {
    case 1: case 6: case 14: case 15: return "BT.709";
    case 13: return "sRGB";
    case 16: return "PQ";
    case 18: return "HLG";
    case 8: return "Linear";
    default: return nullptr;
  }
}

// A HEIF / AVIF meta box: items (iinf), references (iref), properties (iprp).
void read_heif_meta(bytes meta, sink& out) {
  if (meta.size() < 4) return;
  const bytes body = meta.subspan(4);  // FullBox version / flags
  std::size_t at = 0;
  bbox b;
  std::uint32_t primary = 0;
  std::map<std::string, std::size_t> item_types;
  std::map<std::string, std::size_t> refs;
  std::vector<std::string> mimes;
  std::vector<std::string> aux;
  for (int guard = 0; guard < 256 && next_bbox(body, at, b); ++guard) {
    if (b.type == "pitm" && b.payload.size() >= 6) {
      primary = b.payload[0] == 0 ? util::be16(b.payload, 4) : (b.payload.size() >= 8 ? util::be32(b.payload, 4) : 0);
    } else if (b.type == "iinf" && b.payload.size() >= 6) {
      const std::size_t start = b.payload[0] == 0 ? 6 : 8;
      std::size_t p = start;
      bbox infe;
      for (int n = 0; n < 4096 && next_bbox(b.payload, p, infe); ++n) {
        if (infe.type != "infe" || infe.payload.size() < 4) continue;
        const std::uint8_t ver = infe.payload[0];
        if (ver < 2) continue;
        std::size_t q = 4 + (ver == 2 ? 2 : 4) + 2;  // item_ID, protection index
        if (!util::has(infe.payload, q, 4)) continue;
        const std::string type = util::fourcc(infe.payload, q);
        q += 4;
        (void)util::cstr(infe.payload, q, q);  // item name
        ++item_types[type];
        if (type == "mime" && mimes.size() < 16) {
          std::string ct(util::cstr(infe.payload, q, q));
          if (!ct.empty() && std::find(mimes.begin(), mimes.end(), ct) == mimes.end()) mimes.push_back(ct);
        }
      }
    } else if (b.type == "iref" && b.payload.size() >= 4) {
      std::size_t p = 4;
      bbox r;
      for (int n = 0; n < 4096 && next_bbox(b.payload, p, r); ++n) ++refs[r.type];
    } else if (b.type == "iprp") {
      std::size_t p = 0;
      bbox ipco;
      while (next_bbox(b.payload, p, ipco)) {
        if (ipco.type != "ipco") continue;
        std::size_t q = 0;
        bbox prop;
        for (int n = 0; n < 4096 && next_bbox(ipco.payload, q, prop) && !out.full(); ++n) {
          if (prop.type == "auxC" && prop.payload.size() > 4) {
            std::size_t r = 4;
            const std::string_view urn = util::cstr(prop.payload, r, r);
            const char* name = aux_name(urn);
            std::string a = name ? name : std::string(urn);
            if (aux.size() < 16 && std::find(aux.begin(), aux.end(), a) == aux.end()) aux.push_back(a);
          } else if (prop.type == "colr" && prop.payload.size() >= 4) {
            if (util::fourcc_is(prop.payload, 0, "nclx") && prop.payload.size() >= 11) {
              const unsigned pri = util::be16(prop.payload, 4), trc = util::be16(prop.payload, 6),
                             mat = util::be16(prop.payload, 8);
              const bool full = (prop.payload[10] & 0x80) != 0;
              const char* pn = primaries_name(pri);
              const char* tn = transfer_name(trc);
              std::string v = std::string(pn ? pn : ("primaries " + std::to_string(pri)).c_str()) + " / " +
                              (tn ? tn : ("transfer " + std::to_string(trc)).c_str()) + " / matrix " +
                              std::to_string(mat) + (full ? ", full range" : ", limited range");
              (void)out.add(origin::container, "HEIF", "nclx", "Colour (nclx)", v, "HEIF.colr.nclx");
            } else if (util::fourcc_is(prop.payload, 0, "prof") || util::fourcc_is(prop.payload, 0, "rICC")) {
              (void)read_icc(prop.payload.subspan(4), out);
            }
          } else if (prop.type == "pixi" && prop.payload.size() >= 6) {
            std::string bits;
            const std::size_t ch = std::min<std::size_t>(prop.payload[4], prop.payload.size() - 5);
            for (std::size_t c = 0; c < ch; ++c) bits += (c ? "/" : "") + std::to_string(prop.payload[5 + c]);
            (void)out.add(origin::container, "HEIF", "pixi", "Bits per channel", bits, "HEIF.pixi");
          }
        }
      }
    }
  }
  if (primary != 0) (void)out.add(origin::container, "HEIF", "PrimaryItem", "Primary item", std::to_string(primary), "HEIF.pitm");
  std::string items;
  for (const auto& [t, n] : item_types) items += (items.empty() ? "" : ", ") + std::to_string(n) + " \xC3\x97 " + t;
  if (!items.empty()) (void)out.add(origin::container, "HEIF", "Items", "Items", items, "HEIF.iinf");
  std::string rs;
  for (const auto& [t, n] : refs) rs += (rs.empty() ? "" : ", ") + std::to_string(n) + " \xC3\x97 " + t;
  if (!rs.empty()) (void)out.add(origin::container, "HEIF", "References", "Item references", rs, "HEIF.iref");
  std::string ms;
  for (const auto& m : mimes) ms += (ms.empty() ? "" : ", ") + m;
  if (!ms.empty()) (void)out.add(origin::container, "HEIF", "MimeItems", "Metadata items", ms, "HEIF.mime");
  std::string as;
  for (const auto& a : aux) as += (as.empty() ? "" : ", ") + a;
  if (!as.empty()) (void)out.add(origin::container, "HEIF", "Auxiliary", "Auxiliary images", as, "HEIF.auxC");
}

constexpr std::size_t kMaxMetaBox = 4u << 20;
constexpr std::size_t kMaxC2paBox = 32u << 20;

bool read_exact(byte_source& f, std::uint64_t at, std::vector<std::uint8_t>& buf, std::size_t n) {
  buf.resize(n);
  return f.read_at(at, buf) == n;
}

}  // namespace

void read_bmff(byte_source& file, bool stills, sink& out) {
  try {
    const std::uint64_t size = file.size();
    std::uint64_t at = 0;
    std::vector<std::uint8_t> head;
    std::vector<std::uint8_t> payload;
    bool c2pa_done = false;
    for (int guard = 0; guard < 256 && at + 8 <= size; ++guard) {
      if (!read_exact(file, at, head, static_cast<std::size_t>(std::min<std::uint64_t>(32, size - at)))) break;
      std::uint64_t box = util::be32(head, 0);
      std::size_t header = 8;
      if (box == 1) {
        if (head.size() < 16) break;
        box = util::be64(head, 8);
        header = 16;
      } else if (box == 0) {
        box = size - at;
      }
      if (box < header || box > size - at) break;
      const std::string type = util::fourcc(head, 4);
      const std::uint64_t body = box - header;
      if (type == "ftyp" && body >= 8 && body <= 4096) {
        if (read_exact(file, at + header, payload, static_cast<std::size_t>(body))) {
          std::string brands = util::fourcc(payload, 0);
          for (std::size_t i = 8; i + 4 <= payload.size() && i < 8 + 4 * 32; i += 4) {
            const std::string b = util::fourcc(payload, i);
            if (brands.find(b) == std::string::npos) brands += ", " + b;
          }
          (void)out.add(origin::container, stills ? "HEIF" : "Container", "Brands", "Brands", brands, "BMFF.ftyp");
        }
      } else if (type == "meta" && stills && body <= kMaxMetaBox) {
        if (read_exact(file, at + header, payload, static_cast<std::size_t>(body))) read_heif_meta(payload, out);
      } else if ((type == "uuid" || type == "c2pa") && !c2pa_done && body <= kMaxC2paBox) {
        if (read_exact(file, at + header, payload, static_cast<std::size_t>(body))) {
          bytes p(payload);
          if (type == "uuid" && p.size() >= 20 && std::memcmp(p.data(), kC2paUuid, 16) == 0) {
            // FullBox (4), box_purpose (NUL-terminated), then for "manifest" an
            // 8-byte merkle offset, then the JUMBF store.
            std::size_t q = 20;
            const std::string_view purpose = util::cstr(p, q, q);
            if (purpose == "manifest" && util::has(p, q, 8)) {
              read_c2pa(p.subspan(q + 8), out);
              c2pa_done = true;
            }
          } else if (type == "c2pa") {
            read_c2pa(p, out);
            c2pa_done = true;
          }
        }
      }
      at += box;
    }
  } catch (...) {
  }
}

// ---- ICC -------------------------------------------------------------------------

namespace {

// 'desc' (v2 textDescriptionType), 'text', or 'mluc' (v4) as UTF-8.
std::string icc_text(bytes tag) {
  if (tag.size() < 12) return {};
  if (util::fourcc_is(tag, 0, "desc")) {
    const std::size_t n = util::be32(tag, 8);
    if (n == 0 || !util::has(tag, 12, n)) return {};
    return latin1_or_utf8(util::chars(tag.subspan(12, n)));
  }
  if (util::fourcc_is(tag, 0, "text")) return latin1_or_utf8(util::chars(tag.subspan(8)));
  if (util::fourcc_is(tag, 0, "mluc") && tag.size() >= 16) {
    const std::size_t count = util::be32(tag, 8), rec = util::be32(tag, 12);
    if (count == 0 || rec < 12) return {};
    // The English record when there is one, else the first.
    std::size_t pick = 0;
    for (std::size_t i = 0; i < std::min<std::size_t>(count, 64); ++i) {
      const std::size_t r = 16 + i * rec;
      if (!util::has(tag, r, 12)) break;
      if (tag[r] == 'e' && tag[r + 1] == 'n') {
        pick = i;
        break;
      }
    }
    const std::size_t r = 16 + pick * rec;
    if (!util::has(tag, r, 12)) return {};
    const std::size_t len = util::be32(tag, r + 4), off = util::be32(tag, r + 8);
    if (!util::has(tag, off, len)) return {};
    std::string out;
    for (std::size_t i = 0; i + 1 < len; i += 2) {
      std::uint32_t cp = util::be16(tag, off + i);
      if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < len) {
        const std::uint32_t lo = util::be16(tag, off + i + 2);
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          i += 2;
        }
      }
      if (cp == 0) break;
      if (cp < 0x80) out += static_cast<char>(cp);
      else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
      } else if (cp < 0x10000) {
        if (cp >= 0xD800 && cp <= 0xDFFF) continue;
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
      } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
      }
    }
    return out;
  }
  return {};
}

std::string trim_sig(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '?')) s.pop_back();
  return s;
}

}  // namespace

std::string read_icc(std::span<const std::uint8_t> icc, sink& out) {
  std::string description;
  try {
    if (icc.size() < 132 || !util::fourcc_is(icc, 36, "acsp")) return {};
    const char* g = "ICC profile";
    (void)out.add(origin::icc, g, "Version", "Version",
                  std::to_string(icc[8]) + "." + std::to_string(icc[9] >> 4) + "." + std::to_string(icc[9] & 0x0F),
                  "ICC.Header.Version");
    static const std::pair<const char*, const char*> kClass[] = {
        {"mntr", "Display"}, {"scnr", "Input"}, {"prtr", "Output"}, {"link", "Device link"},
        {"spac", "Colour space"}, {"abst", "Abstract"}, {"nmcl", "Named colour"}};
    std::string cls = trim_sig(util::fourcc(icc, 12));
    for (const auto& [code, name] : kClass) {
      if (util::fourcc_is(icc, 12, code)) cls = name;
    }
    (void)out.add(origin::icc, g, "DeviceClass", "Device class", cls, "ICC.Header.DeviceClass");
    (void)out.add(origin::icc, g, "ColorSpace", "Colour space", trim_sig(util::fourcc(icc, 16)), "ICC.Header.ColorSpace");
    (void)out.add(origin::icc, g, "PCS", "Connection space", trim_sig(util::fourcc(icc, 20)), "ICC.Header.PCS");
    const std::uint16_t y = util::be16(icc, 24), mo = util::be16(icc, 26), d = util::be16(icc, 28);
    if (y > 0 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31) {
      char buf[48];
      std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u", y, mo, d, util::be16(icc, 30) % 100u,
                    util::be16(icc, 32) % 100u, util::be16(icc, 34) % 100u);
      (void)out.add(origin::icc, g, "Created", "Created", buf, "ICC.Header.DateTime");
    }
    if (const std::string cmm = trim_sig(util::fourcc(icc, 4)); !cmm.empty() && util::be32(icc, 4) != 0) {
      (void)out.add(origin::icc, g, "CMM", "Colour management module", cmm, "ICC.Header.CMM");
    }
    if (util::be32(icc, 80) != 0) {
      (void)out.add(origin::icc, g, "Creator", "Profile creator", trim_sig(util::fourcc(icc, 80)), "ICC.Header.Creator");
    }
    static constexpr const char* kIntent[] = {"Perceptual", "Relative colorimetric", "Saturation", "Absolute colorimetric"};
    const std::uint32_t intent = util::be32(icc, 64);
    if (intent < 4) (void)out.add(origin::icc, g, "RenderingIntent", "Rendering intent", kIntent[intent], "ICC.Header.RenderingIntent");

    const std::size_t count = util::be32(icc, 128);
    for (std::size_t i = 0; i < std::min<std::size_t>(count, 256); ++i) {
      const std::size_t e = 132 + i * 12;
      if (!util::has(icc, e, 12)) break;
      const std::size_t off = util::be32(icc, e + 4), len = util::be32(icc, e + 8);
      if (!util::has(icc, off, len)) continue;
      const bytes tag = icc.subspan(off, len);
      const char* label = nullptr;
      const char* key = nullptr;
      if (util::fourcc_is(icc, e, "desc")) { label = "Description"; key = "ICC.desc"; }
      else if (util::fourcc_is(icc, e, "cprt")) { label = "Copyright"; key = "ICC.cprt"; }
      else if (util::fourcc_is(icc, e, "dmnd")) { label = "Device manufacturer"; key = "ICC.dmnd"; }
      else if (util::fourcc_is(icc, e, "dmdd")) { label = "Device model"; key = "ICC.dmdd"; }
      if (!label) continue;
      std::string text = icc_text(tag);
      while (!text.empty() && (text.back() == '\0' || text.back() == ' ')) text.pop_back();
      if (text.empty()) continue;
      if (std::string_view(key) == "ICC.desc") description = clean_text(text, 256);
      (void)out.add(origin::icc, g, label, label, text, key);
    }
  } catch (...) {
  }
  return description;
}

}  // namespace mv::meta::parse
