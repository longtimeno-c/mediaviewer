// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #290 (docs/plans/metadata-search-and-place.md, slice 1): every tag in the file,
// AI provenance and GPS as numbers. Fixtures are built in the test: a C2PA store
// (JUMBF + CBOR + a DER certificate) in JPEG APP11 segments, a PNG caBX chunk and
// an ISO-BMFF uuid box; PNG tEXt / zTXt / iTXt from Stable Diffusion and ComfyUI;
// Midjourney- and DJI-style XMP; an ICC profile. Every parser is also fed
// truncated and mutated input (no crash, no out-of-bounds read under ASan).
#include <catch2/catch_test_macros.hpp>

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "fixtures.h"
#include "meta/generators.h"
#include "meta/meta.h"
#include "meta/parse.h"
#include "meta/probe.h"
#include "meta/tables.h"
#include "meta/write.h"

namespace {

namespace fs = std::filesystem;
using bytes = std::vector<std::uint8_t>;

struct temp_file {
  fs::path path;
  explicit temp_file(const std::string& name) { path = fs::temp_directory_path() / ("mv_prov_test_" + name); }
  ~temp_file() {
    std::error_code ec;
    fs::remove(path, ec);
  }
  [[nodiscard]] std::string utf8() const { return path.string(); }
  void write(const bytes& b) const {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  }
};

void append(bytes& out, const bytes& b) { out.insert(out.end(), b.begin(), b.end()); }
void append(bytes& out, std::string_view s) { out.insert(out.end(), s.begin(), s.end()); }
void be16(bytes& out, std::uint32_t v) {
  out.push_back(static_cast<std::uint8_t>(v >> 8));
  out.push_back(static_cast<std::uint8_t>(v));
}
void be32(bytes& out, std::uint32_t v) {
  be16(out, v >> 16);
  be16(out, v & 0xFFFF);
}

// ---- CBOR ---------------------------------------------------------------------
void cbor_head(bytes& out, int major, std::uint64_t n) {
  const auto m = static_cast<std::uint8_t>(major << 5);
  if (n < 24) {
    out.push_back(static_cast<std::uint8_t>(m | n));
  } else if (n < 256) {
    out.push_back(m | 24);
    out.push_back(static_cast<std::uint8_t>(n));
  } else if (n < 65536) {
    out.push_back(m | 25);
    be16(out, static_cast<std::uint32_t>(n));
  } else {
    out.push_back(m | 26);
    be32(out, static_cast<std::uint32_t>(n));
  }
}
bytes ctext(std::string_view s) {
  bytes b;
  cbor_head(b, 3, s.size());
  append(b, s);
  return b;
}
bytes cbytes(const bytes& v) {
  bytes b;
  cbor_head(b, 2, v.size());
  append(b, v);
  return b;
}
bytes cmap(std::initializer_list<std::pair<bytes, bytes>> kv) {
  bytes b;
  cbor_head(b, 5, kv.size());
  for (const auto& [k, v] : kv) {
    append(b, k);
    append(b, v);
  }
  return b;
}
bytes carr(std::initializer_list<bytes> items) {
  bytes b;
  cbor_head(b, 4, items.size());
  for (const auto& i : items) append(b, i);
  return b;
}
bytes cuint(std::uint64_t v) {
  bytes b;
  cbor_head(b, 0, v);
  return b;
}

// ---- DER ----------------------------------------------------------------------
bytes der(std::uint8_t tag, const bytes& content) {
  bytes b{tag};
  if (content.size() < 128) {
    b.push_back(static_cast<std::uint8_t>(content.size()));
  } else {
    b.push_back(0x82);
    be16(b, static_cast<std::uint32_t>(content.size()));
  }
  append(b, content);
  return b;
}
bytes der_str(std::string_view s) { return der(0x0C, bytes(s.begin(), s.end())); }
bytes rdn(std::uint8_t oid_last, std::string_view value) {
  bytes seq = der(0x06, {0x55, 0x04, oid_last});
  append(seq, der_str(value));
  return der(0x31, der(0x30, seq));
}
bytes name(std::string_view cn, std::string_view o) {
  bytes n = rdn(0x03, cn);
  append(n, rdn(0x0A, o));
  return der(0x30, n);
}
bytes certificate() {
  bytes tbs = der(0xA0, der(0x02, {0x02}));
  append(tbs, der(0x02, {0x01}));
  append(tbs, der(0x30, der(0x06, {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02})));
  append(tbs, name("Test Root CA", "Truepic"));
  append(tbs, der(0x30, {}));
  append(tbs, name("ChatGPT signer", "OpenAI"));
  return der(0x30, der(0x30, tbs));
}
bytes timestamp_token() {
  // ContentInfo → SignedData → encapContentInfo → [0] OCTET STRING (TSTInfo) → genTime.
  bytes tst = der(0x02, {0x01});
  append(tst, der(0x18, bytes{'2', '0', '2', '5', '0', '3', '0', '1', '1', '2', '3', '4', '5', '6', 'Z'}));
  bytes encap = der(0x06, {0x2A});
  append(encap, der(0xA0, der(0x04, der(0x30, tst))));
  return der(0x30, der(0x30, encap));
}

// ---- JUMBF --------------------------------------------------------------------
bytes box(std::string_view type, const bytes& payload) {
  bytes b;
  be32(b, static_cast<std::uint32_t>(payload.size() + 8));
  append(b, type);
  append(b, payload);
  return b;
}
bytes superbox(std::string_view label, std::initializer_list<bytes> children) {
  bytes jumd(16, 0x11);
  jumd.push_back(0x03);  // requestable, label
  append(jumd, label);
  jumd.push_back(0);
  bytes payload = box("jumd", jumd);
  for (const auto& c : children) append(payload, c);
  return box("jumb", payload);
}

bytes c2pa_store(std::string_view generator, std::string_view source_type, std::string_view agent) {
  const bytes actions = cmap({{ctext("actions"),
                               carr({cmap({{ctext("action"), ctext("c2pa.created")},
                                           {ctext("digitalSourceType"), ctext(source_type)},
                                           {ctext("softwareAgent"), ctext(agent)}}),
                                     cmap({{ctext("action"), ctext("c2pa.converted")}})})}});
  const bytes claim = cmap({{ctext("claim_generator_info"), carr({cmap({{ctext("name"), ctext(generator)},
                                                                        {ctext("version"), ctext("1.0")}})})},
                            {ctext("dc:title"), ctext("image.png")},
                            {ctext("instanceID"), ctext("xmp:iid:1234")}});
  bytes protected_hdr = cmap({{cuint(1), bytes{0x26}}, {cuint(33), cbytes(certificate())}});
  bytes sig;
  cbor_head(sig, 6, 18);
  append(sig, carr({cbytes(protected_hdr),
                    cmap({{ctext("sigTst"), cmap({{ctext("tstTokens"),
                                                   carr({cmap({{ctext("val"), cbytes(timestamp_token())}})})}})}}),
                    bytes{0xF6}, cbytes(bytes(64, 0xAB))}));
  return superbox("c2pa",
                  {superbox("urn:uuid:abc",
                            {superbox("c2pa.assertions",
                                      {superbox("c2pa.actions", {box("cbor", actions)}),
                                       superbox("stds.schema-org.CreativeWork",
                                                {box("json", bytes{'{', '"', 'a', 'u', 't', 'h', 'o', 'r', '"', ':',
                                                                   '"', 'X', '"', '}'})}),
                                       superbox("c2pa.hash.data", {box("cbor", cmap({{ctext("hash"),
                                                                                     cbytes(bytes(32, 1))}}))})}),
                             superbox("c2pa.claim", {box("cbor", claim)}), superbox("c2pa.signature", {box("cbor", sig)})})});
}

constexpr std::string_view kTrained = "http://cv.iptc.org/newscodes/digitalsourcetype/trainedAlgorithmicMedia";

bytes grey_jpeg() {
  std::vector<std::uint8_t> rgb(32 * 24 * 3, 90);
  return fixtures::jpeg_rgb(32, 24, rgb.data());
}

// Inserts segments right after SOI.
bytes jpeg_with_segments(const bytes& jpeg, const std::vector<bytes>& segments) {
  bytes out(jpeg.begin(), jpeg.begin() + 2);
  for (const auto& s : segments) append(out, s);
  out.insert(out.end(), jpeg.begin() + 2, jpeg.end());
  return out;
}

bytes segment(std::uint8_t marker, const bytes& payload) {
  bytes s{0xFF, marker};
  be16(s, static_cast<std::uint32_t>(payload.size() + 2));
  append(s, payload);
  return s;
}

// The store in APP11 segments of at most `piece` bytes, continuations repeating
// the superbox header, as JPEG XT and C2PA write it.
std::vector<bytes> app11(const bytes& store, std::size_t piece, std::uint16_t instance = 1) {
  std::vector<bytes> segs;
  std::uint32_t seq = 1;
  for (std::size_t at = 0; at < store.size(); at += piece, ++seq) {
    bytes p{'J', 'P'};
    be16(p, instance);
    be32(p, seq);
    if (seq > 1) p.insert(p.end(), store.begin(), store.begin() + 8);
    p.insert(p.end(), store.begin() + static_cast<std::ptrdiff_t>(at),
             store.begin() + static_cast<std::ptrdiff_t>(std::min(store.size(), at + piece)));
    segs.push_back(segment(0xEB, p));
  }
  return segs;
}

// ---- PNG ----------------------------------------------------------------------
bytes png_chunk(std::string_view type, const bytes& data) {
  bytes c;
  be32(c, static_cast<std::uint32_t>(data.size()));
  append(c, type);
  append(c, data);
  uLong crc = crc32(0, reinterpret_cast<const Bytef*>(type.data()), 4);
  crc = crc32(crc, data.data(), static_cast<uInt>(data.size()));
  be32(c, static_cast<std::uint32_t>(crc));
  return c;
}
bytes zlib(std::string_view s) {
  uLongf n = compressBound(static_cast<uLong>(s.size()));
  bytes out(n);
  compress(out.data(), &n, reinterpret_cast<const Bytef*>(s.data()), static_cast<uLong>(s.size()));
  out.resize(n);
  return out;
}
bytes text_chunk(std::string_view key, std::string_view text) {
  bytes d(key.begin(), key.end());
  d.push_back(0);
  append(d, text);
  return png_chunk("tEXt", d);
}
bytes ztxt_chunk(std::string_view key, std::string_view text) {
  bytes d(key.begin(), key.end());
  d.push_back(0);
  d.push_back(0);
  append(d, zlib(text));
  return png_chunk("zTXt", d);
}
bytes itxt_chunk(std::string_view key, std::string_view text, bool compressed, std::string_view lang = "") {
  bytes d(key.begin(), key.end());
  d.push_back(0);
  d.push_back(compressed ? 1 : 0);
  d.push_back(0);
  append(d, lang);
  d.push_back(0);
  d.push_back(0);  // translated keyword
  if (compressed) append(d, zlib(text));
  else append(d, text);
  return png_chunk("iTXt", d);
}
bytes small_png() {
  std::vector<std::uint8_t> rgba(8 * 8 * 4, 200);
  return fixtures::png_rgba(8, 8, rgba.data());
}
// Inserts chunks after IHDR (8-byte signature + 25-byte IHDR chunk).
bytes png_with(const bytes& png, const std::vector<bytes>& chunks) {
  bytes out(png.begin(), png.begin() + 33);
  for (const auto& c : chunks) append(out, c);
  out.insert(out.end(), png.begin() + 33, png.end());
  return out;
}

const mv::meta::property* find_tag(const mv::meta::metadata& m, std::string_view raw_tag) {
  for (const auto& p : m.properties) {
    if (p.raw_tag == raw_tag) return &p;
  }
  return nullptr;
}
std::size_t count_space(const mv::meta::metadata& m, mv::meta::origin o) {
  return static_cast<std::size_t>(
      std::count_if(m.properties.begin(), m.properties.end(), [&](const auto& p) { return p.space == o; }));
}
std::string summary_value(const mv::meta::metadata& m, std::string_view label) {
  for (const auto& r : mv::meta::summary_rows(m)) {
    if (r.label == label) return r.value;
  }
  return "<absent>";
}

mv::meta::metadata read_bytes(const std::string& name, const bytes& b) {
  temp_file f(name);
  f.write(b);
  auto m = mv::meta::read(f.utf8());
  REQUIRE(m);
  return *m;
}

}  // namespace

TEST_CASE("generator table: whole words, specific spellings first, positive only", "[meta][provenance]") {
  using mv::meta::match_generator;
  CHECK(match_generator("ChatGPT")->name == "ChatGPT");
  CHECK(match_generator("GPT-4o")->name == "ChatGPT");
  CHECK(match_generator("OpenAI-API c2pa-rs/0.31")->name == "OpenAI");
  CHECK(match_generator("DALL-E 3")->name == "DALL\xC2\xB7" "E");
  CHECK(match_generator("Adobe Firefly 2.1")->name == "Adobe Firefly");
  CHECK(match_generator("Midjourney")->name == "Midjourney");
  CHECK(match_generator("Version: v1.6.0-2-g4afaaf8a, stable-diffusion-webui")->name ==
        "Stable Diffusion (AUTOMATIC1111)");
  CHECK(match_generator("ComfyUI")->name == "ComfyUI");
  CHECK(match_generator("InvokeAI 3.4")->name == "InvokeAI");
  CHECK(match_generator("Image Creator from Microsoft Designer")->name == "Microsoft Designer");
  CHECK(match_generator("Bing Image Creator")->name == "Bing Image Creator");
  CHECK(match_generator("Google Imagen 3")->name == "Google Imagen");
  CHECK(match_generator("Gemini")->name == "Google Gemini");
  CHECK(match_generator("Imagine with Meta AI")->name == "Meta AI");
  CHECK(match_generator("Leonardo.Ai")->name == "Leonardo.Ai");
  CHECK(match_generator("Ideogram 2.0")->name == "Ideogram");
  CHECK(match_generator("FLUX.1 [dev]")->name == "FLUX");
  for (const auto& [pattern, g] : mv::meta::generator_table()) {
    CHECK(match_generator(pattern).has_value());
    CHECK(g.ai);
  }
  // Not generators, and not substrings of a longer word.
  CHECK_FALSE(match_generator("Adobe Photoshop 25.0 (Macintosh)"));
  CHECK_FALSE(match_generator("Ver.1.00"));
  CHECK_FALSE(match_generator("Canon EOS R5 Firmware 1.8.1"));
  CHECK_FALSE(match_generator("Imagenet"));
  CHECK_FALSE(match_generator("Editor de imagen 2.1"));  // Spanish "image"
  CHECK_FALSE(match_generator("Meta AI"));               // the glasses' camera app
  CHECK_FALSE(match_generator("Fireflyer"));
  CHECK_FALSE(match_generator(""));
}

TEST_CASE("provenance rule: source type, table, PNG chunks, claim generator", "[meta][provenance]") {
  using mv::meta::derive_provenance;
  mv::meta::parse::evidence ev;
  CHECK(derive_provenance(ev).made_with.empty());
  CHECK_FALSE(derive_provenance(ev).ai_generated);

  ev.exif_software = "Adobe Photoshop 25.0";
  CHECK(derive_provenance(ev).made_with.empty());  // an editor is not "made with"

  ev.c2pa_generator = "Adobe Photoshop 25.0";
  CHECK(derive_provenance(ev).made_with == "Adobe Photoshop 25.0");  // a signed claim is
  CHECK_FALSE(derive_provenance(ev).ai_generated);
  ev.source_types.push_back("http://cv.iptc.org/newscodes/digitalsourcetype/compositeWithTrainedAlgorithmicMedia");
  CHECK(derive_provenance(ev).ai_generated);

  mv::meta::parse::evidence sd;
  sd.png_parameters = true;
  CHECK(derive_provenance(sd).made_with == "Stable Diffusion");
  CHECK(derive_provenance(sd).ai_generated);
  sd.png_generator_text.push_back("Version: v1.10.1, stable-diffusion-webui");
  CHECK(derive_provenance(sd).made_with == "Stable Diffusion (AUTOMATIC1111)");

  mv::meta::parse::evidence comfy;
  comfy.png_comfyui = true;
  CHECK(derive_provenance(comfy).made_with == "ComfyUI");

  mv::meta::parse::evidence xmp;
  xmp.xmp_creator_tool = "Midjourney";
  CHECK(derive_provenance(xmp).made_with == "Midjourney");
  CHECK(derive_provenance(xmp).ai_generated);

  mv::meta::parse::evidence st;
  st.source_types.push_back("digitalCapture");
  CHECK_FALSE(derive_provenance(st).ai_generated);
  CHECK(mv::meta::describe_source_type(kTrained) == "AI-generated (trainedAlgorithmicMedia)");
  CHECK(mv::meta::describe_source_type("http://cv.iptc.org/newscodes/digitalsourcetype/digitalCapture") ==
        "Digital capture (digitalCapture)");
}

TEST_CASE("ChatGPT-style JPEG: C2PA across APP11 segments", "[meta][provenance][c2pa]") {
  const bytes store = c2pa_store("ChatGPT", kTrained, "GPT-4o");
  REQUIRE(store.size() > 300);
  const bytes jpeg = jpeg_with_segments(grey_jpeg(), app11(store, 120));
  REQUIRE(mv::meta::parse::jpeg_jumbf(jpeg) == store);

  const auto m = read_bytes("chatgpt.jpg", jpeg);
  CHECK(m.s.made_with == "ChatGPT");
  CHECK(m.s.ai_generated);
  CHECK(summary_value(m, "Made with") == "ChatGPT \xC2\xB7 AI-generated");
  const auto* gen = find_tag(m, "C2PA.claim.generator");
  REQUIRE(gen);
  CHECK(gen->value == "ChatGPT 1.0");
  CHECK(gen->space == mv::meta::origin::c2pa);
  CHECK(find_tag(m, "C2PA.actions")->value == "c2pa.created, c2pa.converted");
  CHECK(find_tag(m, "C2PA.actions.digitalSourceType")->value.find("trainedAlgorithmicMedia") != std::string::npos);
  CHECK(find_tag(m, "C2PA.actions.softwareAgent")->value == "GPT-4o");
  CHECK(find_tag(m, "C2PA.signature.subject")->value == "OpenAI (ChatGPT signer)");
  CHECK(find_tag(m, "C2PA.signature.issuer")->value == "Truepic (Test Root CA)");
  CHECK(find_tag(m, "C2PA.signature.time")->value == "2025-03-01 12:34:56 UTC");
  // Every assertion, raw: CBOR, JSON and the hash (as a size, not bytes).
  CHECK(find_tag(m, "C2PA.c2pa.actions.actions[1].action")->value == "c2pa.created");
  CHECK(find_tag(m, "C2PA.stds.schema-org.CreativeWork.author")->value == "X");
  CHECK(find_tag(m, "C2PA.c2pa.hash.data.hash")->value == "32 bytes");
  CHECK(find_tag(m, "C2PA.claim.dc:title")->value == "image.png");
  // JFIF from the same walk.
  CHECK(find_tag(m, "JFIF.Density"));

  // Read-only in the pane: every non-Exiv2 row is "r".
  const std::string table = mv::meta::editable_properties_table(m);
  std::size_t lines = 0;
  for (std::size_t at = 0; (at = table.find("\nc2pa\t", at)) != std::string::npos; ++at) {
    const auto eol = table.find('\n', at + 1);
    CHECK(table.substr(eol - 2, 2) == "\tr");
    ++lines;
  }
  CHECK(lines > 5);
}

TEST_CASE("APP11 segments out of order and a second JUMBF instance", "[meta][provenance][c2pa]") {
  const bytes store = c2pa_store("Adobe Firefly", kTrained, "Adobe Firefly");
  auto segs = app11(store, 64, 7);
  std::reverse(segs.begin(), segs.end());
  // An unrelated JUMBF instance (not labelled c2pa) first.
  const bytes other = superbox("jpegxt", {box("json", bytes{'{', '}'})});
  auto noise = app11(other, 1000, 3);
  segs.insert(segs.begin(), noise.begin(), noise.end());
  const bytes jpeg = jpeg_with_segments(grey_jpeg(), segs);
  CHECK(mv::meta::parse::jpeg_jumbf(jpeg) == store);
  const auto m = read_bytes("firefly.jpg", jpeg);
  CHECK(m.s.made_with == "Adobe Firefly");
  CHECK(m.s.ai_generated);
}

TEST_CASE("PNG caBX, Stable Diffusion parameters, ComfyUI workflow, zTXt and iTXt", "[meta][provenance][png]") {
  const bytes store = c2pa_store("Microsoft Designer", kTrained, "DALL-E 3");
  const std::string params =
      "a lighthouse at dusk, oil painting\nNegative prompt: blurry\nSteps: 30, Sampler: DPM++ 2M, CFG scale: 7, "
      "Seed: 1234, Model: sd_xl_base_1.0, Version: v1.10.1";
  const bytes png = png_with(small_png(), {png_chunk("caBX", store), text_chunk("parameters", params),
                                           itxt_chunk("workflow", "{\"nodes\": [1, 2, 3]}", true),
                                           itxt_chunk("Title", "Leuchtturm", false, "de"),
                                           ztxt_chunk("Comment", "made at the coast \xE9t\xE9"),
                                           text_chunk("Software", "ComfyUI")});
  const auto m = read_bytes("sd.png", png);
  CHECK(m.s.made_with == "Microsoft Designer");  // the signed claim wins
  CHECK(m.s.ai_generated);
  CHECK(find_tag(m, "PNG.tEXt.parameters")->value.rfind("a lighthouse at dusk", 0) == 0);
  CHECK(find_tag(m, "PNG.tEXt.parameters")->space == mv::meta::origin::png_text);
  CHECK(find_tag(m, "PNG.iTXt.workflow")->value == "{\"nodes\": [1, 2, 3]}");
  CHECK(find_tag(m, "PNG.iTXt.Title")->label == "Title (de)");
  CHECK(find_tag(m, "PNG.zTXt.Comment")->value == "made at the coast \xC3\xA9t\xC3\xA9");  // Latin-1 → UTF-8
  CHECK(find_tag(m, "C2PA.actions.softwareAgent")->value == "DALL-E 3");

  // Without the store: the chunks alone decide.
  const auto sd = read_bytes("sd2.png", png_with(small_png(), {text_chunk("parameters", params)}));
  CHECK(sd.s.made_with == "Stable Diffusion");
  CHECK(sd.s.ai_generated);
  const auto comfy = read_bytes("comfy.png", png_with(small_png(), {itxt_chunk("prompt", "{\"3\": {}}", false),
                                                                    ztxt_chunk("workflow", "{\"nodes\": []}")}));
  CHECK(comfy.s.made_with == "ComfyUI");
  CHECK(comfy.s.ai_generated);
  // A plain PNG says nothing.
  const auto plain = read_bytes("plain.png", png_with(small_png(), {text_chunk("Author", "Me")}));
  CHECK(plain.s.made_with.empty());
  CHECK_FALSE(plain.s.ai_generated);
  CHECK(summary_value(plain, "Made with") == "<absent>");
}

TEST_CASE("Midjourney-style XMP: unregistered namespace and history listed raw", "[meta][provenance][xmp]") {
  const std::string packet =
      "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>"
      "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
      "<rdf:Description rdf:about=\"\" xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\""
      " xmlns:mjx=\"https://example.invalid/mj/ns/1.0/\""
      " xmlns:Iptc4xmpExt=\"http://iptc.org/std/Iptc4xmpExt/2008-02-29/\""
      " xmlns:xmpMM=\"http://ns.adobe.com/xap/1.0/mm/\""
      " xmlns:stEvt=\"http://ns.adobe.com/xap/1.0/sType/ResourceEvent#\""
      " xmlns:tiff=\"urn:example:not-tiff\" tiff:Seed=\"42\""
      " xmp:CreatorTool=\"Midjourney\" mjx:JobId=\"0f3e-77\""
      " Iptc4xmpExt:DigitalSourceType=\"http://cv.iptc.org/newscodes/digitalsourcetype/trainedAlgorithmicMedia\">"
      "<mjx:Prompt>a red fox in snow --v 6</mjx:Prompt>"
      "<mjx:Settings rdf:parseType=\"Resource\"><mjx:Stylize>250</mjx:Stylize><mjx:Aspect>3:2</mjx:Aspect></mjx:Settings>"
      "<xmpMM:History><rdf:Seq>"
      "<rdf:li rdf:parseType=\"Resource\"><stEvt:action>created</stEvt:action><stEvt:softwareAgent>Midjourney"
      "</stEvt:softwareAgent></rdf:li>"
      "<rdf:li stEvt:action=\"saved\" stEvt:when=\"2025-01-02T03:04:05Z\"/>"
      "</rdf:Seq></xmpMM:History>"
      "</rdf:Description></rdf:RDF></x:xmpmeta><?xpacket end=\"w\"?>";
  bytes app1;
  append(app1, std::string_view("http://ns.adobe.com/xap/1.0/\0", 29));
  append(app1, packet);
  const auto m = read_bytes("mj.jpg", jpeg_with_segments(grey_jpeg(), {segment(0xE1, app1)}));
  CHECK(m.s.made_with == "Midjourney");
  CHECK(m.s.ai_generated);

  // Every leaf of the packet is somewhere in the tree, from Exiv2 or raw.
  const auto has_leaf = [&](std::string_view needle, std::string_view value) {
    for (const auto& p : m.properties) {
      if ((p.space == mv::meta::origin::xmp || p.space == mv::meta::origin::xmp_raw) &&
          p.raw_tag.find(needle) != std::string::npos && p.value.find(value) != std::string::npos) {
        return true;
      }
    }
    return false;
  };
  CHECK(has_leaf("CreatorTool", "Midjourney"));
  CHECK(has_leaf("Seed", "42"));  // a prefix that collides with a registered one
  CHECK(has_leaf("JobId", "0f3e-77"));
  CHECK(has_leaf("Prompt", "a red fox in snow"));
  CHECK(has_leaf("Stylize", "250"));
  CHECK(has_leaf("Aspect", "3:2"));
  CHECK(has_leaf("History[1]/stEvt:action", "created"));
  CHECK(has_leaf("History[2]/stEvt:when", "2025-01-02"));
  // Raw rows are never editable.
  for (const auto& p : m.properties) {
    if (p.space == mv::meta::origin::xmp_raw) {
      CHECK(mv::meta::row_access(p, mv::meta::write_target::in_file) == mv::meta::tag_access::read_only);
    }
  }

  // The reader alone, nothing listed: every leaf as a raw row with its path.
  std::vector<mv::meta::property> rows;
  mv::meta::parse::evidence ev;
  mv::meta::parse::sink sink{rows, ev, 4000};
  mv::meta::parse::read_xmp(packet, nullptr, nullptr, sink);
  const auto raw = [&](std::string_view key) -> std::string {
    for (const auto& p : rows) {
      if (p.raw_tag == key) return p.value;
    }
    return "<none>";
  };
  CHECK(raw("Xmp.mjx.JobId") == "0f3e-77");
  CHECK(raw("Xmp.mjx.Settings/mjx:Stylize") == "250");
  CHECK(raw("Xmp.xmpMM.History[1]/stEvt:softwareAgent") == "Midjourney");
  CHECK(raw("Xmp.xmpMM.History[2]/stEvt:action") == "saved");
  CHECK(ev.xmp_creator_tool == "Midjourney");
  REQUIRE(ev.source_types.size() == 1);
}


TEST_CASE("DJI-style XMP GPS without EXIF GPS fills where, with altitude", "[meta][provenance][gps]") {
  const std::string packet =
      "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
      "<rdf:Description rdf:about=\"\" xmlns:exif=\"http://ns.adobe.com/exif/1.0/\""
      " xmlns:drone-dji=\"http://www.dji.com/drone-dji/1.0/\""
      " exif:GPSLatitude=\"38,47.6160N\" exif:GPSLongitude=\"9,23.2560W\" exif:GPSAltitude=\"1234/10\""
      " exif:GPSAltitudeRef=\"0\" drone-dji:FlightYawDegree=\"+12.30\"/>"
      "</rdf:RDF></x:xmpmeta>";
  bytes app1;
  append(app1, std::string_view("http://ns.adobe.com/xap/1.0/\0", 29));
  append(app1, packet);
  const auto m = read_bytes("dji.jpg", jpeg_with_segments(grey_jpeg(), {segment(0xE1, app1)}));
  REQUIRE(m.s.where);
  CHECK(m.s.where->lat > 38.7935);
  CHECK(m.s.where->lat < 38.7937);
  CHECK(m.s.where->lon < -9.3875);
  CHECK(m.s.where->lon > -9.3877);
  REQUIRE(m.s.where->altitude);
  CHECK(*m.s.where->altitude > 123.39);
  CHECK(*m.s.where->altitude < 123.41);
  CHECK(m.s.gps == "38.79360\xC2\xB0 N, 9.38760\xC2\xB0 W");
  CHECK_FALSE(m.s.made_with.size());  // a drone is not a generator
}

TEST_CASE("coordinate parsers: XMP and ISO 6709", "[meta][gps]") {
  using mv::meta::parse_iso6709;
  using mv::meta::parse_xmp_coordinate;
  CHECK(std::fabs(*parse_xmp_coordinate("48,51.5N") - (48.0 + 51.5 / 60.0)) < 1e-9);
  CHECK(std::fabs(*parse_xmp_coordinate("2,17,40.2W") + (2.0 + 17.0 / 60.0 + 40.2 / 3600.0)) < 1e-9);
  CHECK(std::fabs(*parse_xmp_coordinate("-33.865") + 33.865) < 1e-9);
  CHECK_FALSE(parse_xmp_coordinate(""));
  CHECK_FALSE(parse_xmp_coordinate("48,61N"));
  CHECK_FALSE(parse_xmp_coordinate("abc"));
  CHECK_FALSE(parse_xmp_coordinate("190,0E"));
  CHECK_FALSE(parse_xmp_coordinate("48,,1N"));

  auto p = parse_iso6709("+48.8584+002.2945+035.000/");
  REQUIRE(p);
  CHECK(std::fabs(p->lat - 48.8584) < 1e-9);
  CHECK(std::fabs(p->lon - 2.2945) < 1e-9);
  REQUIRE(p->altitude);
  CHECK(std::fabs(*p->altitude - 35.0) < 1e-9);
  p = parse_iso6709("-33.8688+151.2093/");
  REQUIRE(p);
  CHECK(p->lat < 0);
  CHECK_FALSE(p->altitude);
  p = parse_iso6709("+4851.50-00217.67/");  // degrees and minutes
  REQUIRE(p);
  CHECK(std::fabs(p->lat - (48.0 + 51.5 / 60.0)) < 1e-9);
  CHECK(std::fabs(p->lon + (2.0 + 17.67 / 60.0)) < 1e-9);
  CHECK_FALSE(parse_iso6709("+95.0+000.0/"));
  CHECK_FALSE(parse_iso6709("48.85+2.29"));
  CHECK_FALSE(parse_iso6709(""));
}

TEST_CASE("ISO-BMFF c2pa uuid box (HEIF / MP4 layout)", "[meta][provenance][c2pa]") {
  const bytes store = c2pa_store("Sora", kTrained, "Sora");
  bytes ftyp_payload;
  append(ftyp_payload, std::string_view("isom"));
  be32(ftyp_payload, 0);
  append(ftyp_payload, std::string_view("isommp41"));
  bytes uuid{0xD8, 0xFE, 0xC3, 0xD6, 0x1B, 0x0E, 0x48, 0x3C, 0x92, 0x97, 0x58, 0x28, 0x87, 0x7E, 0xC4, 0x81};
  be32(uuid, 0);  // version, flags
  append(uuid, std::string_view("manifest\0", 9));
  be32(uuid, 0);
  be32(uuid, 0);  // merkle offset
  append(uuid, store);
  bytes file = box("ftyp", ftyp_payload);
  append(file, box("mdat", bytes(4096, 0)));
  append(file, box("uuid", uuid));

  std::vector<mv::meta::property> rows;
  mv::meta::parse::evidence ev;
  mv::meta::parse::sink sink{rows, ev, 4000};
  mv::meta::parse::memory_source src(file);
  mv::meta::parse::read_bmff(src, false, sink);
  CHECK(ev.c2pa_generator == "Sora 1.0");
  CHECK(mv::meta::derive_provenance(ev).made_with == "Sora");

  // Through meta::read: not a clip FFmpeg can play, still read past the mdat.
  const auto m = read_bytes("sora.mp4", file);
  CHECK(m.s.made_with == "Sora");
  CHECK(m.s.ai_generated);
  CHECK(find_tag(m, "BMFF.ftyp"));
}

namespace {
bytes icc_profile() {
  bytes p(128, 0);
  const auto put32 = [&](std::size_t at, std::uint32_t v) {
    p[at] = static_cast<std::uint8_t>(v >> 24);
    p[at + 1] = static_cast<std::uint8_t>(v >> 16);
    p[at + 2] = static_cast<std::uint8_t>(v >> 8);
    p[at + 3] = static_cast<std::uint8_t>(v);
  };
  std::memcpy(&p[12], "mntr", 4);
  std::memcpy(&p[16], "RGB ", 4);
  std::memcpy(&p[20], "XYZ ", 4);
  std::memcpy(&p[36], "acsp", 4);
  p[8] = 4;
  p[9] = 0x30;
  // Two tags: desc (mluc, "Display P3") and cprt (text).
  bytes desc;
  append(desc, std::string_view("mluc"));
  be32(desc, 0);
  be32(desc, 1);
  be32(desc, 12);
  append(desc, std::string_view("enUS"));
  const std::u16string name = u"Display P3";
  be32(desc, static_cast<std::uint32_t>(name.size() * 2));
  be32(desc, 28);
  for (char16_t c : name) be16(desc, c);
  bytes cprt;
  append(cprt, std::string_view("text"));
  be32(cprt, 0);
  append(cprt, std::string_view("Copyright Apple Inc., 2022\0", 27));
  bytes table;
  be32(table, 2);
  const std::uint32_t desc_at = 128 + 4 + 24;
  const std::uint32_t cprt_at = desc_at + static_cast<std::uint32_t>(desc.size());
  append(table, std::string_view("desc"));
  be32(table, desc_at);
  be32(table, static_cast<std::uint32_t>(desc.size()));
  append(table, std::string_view("cprt"));
  be32(table, cprt_at);
  be32(table, static_cast<std::uint32_t>(cprt.size()));
  append(p, table);
  append(p, desc);
  append(p, cprt);
  put32(0, static_cast<std::uint32_t>(p.size()));
  return p;
}
}  // namespace

TEST_CASE("ICC profile description and copyright", "[meta][icc]") {
  const bytes icc = icc_profile();
  std::vector<mv::meta::property> rows;
  mv::meta::parse::evidence ev;
  mv::meta::parse::sink sink{rows, ev, 4000};
  CHECK(mv::meta::parse::read_icc(icc, sink) == "Display P3");
  bool cprt = false, cls = false;
  for (const auto& r : rows) {
    cprt |= r.raw_tag == "ICC.cprt" && r.value == "Copyright Apple Inc., 2022";
    cls |= r.raw_tag == "ICC.Header.DeviceClass" && r.value == "Display";
    CHECK(r.space == mv::meta::origin::icc);
  }
  CHECK(cprt);
  CHECK(cls);

  // In a real file, through Exiv2's reassembled profile.
  std::vector<std::uint8_t> rgb(16 * 16 * 3, 100);
  const auto m = read_bytes("icc.jpg", fixtures::jpeg_rgb(16, 16, rgb.data(), icc));
  CHECK(find_tag(m, "ICC.desc"));
}

TEST_CASE("a 2 000-tag file lists every tag", "[meta][provenance]") {
  std::vector<bytes> chunks;
  for (int i = 0; i < 2000; ++i) chunks.push_back(text_chunk("Key" + std::to_string(i), "value " + std::to_string(i)));
  const auto m = read_bytes("many.png", png_with(small_png(), chunks));
  CHECK(count_space(m, mv::meta::origin::png_text) == 2000);
  CHECK(find_tag(m, "PNG.tEXt.Key1999")->value == "value 1999");
  const std::string table = mv::meta::editable_properties_table(m);
  CHECK(static_cast<std::size_t>(std::count(table.begin(), table.end(), '\n')) == m.properties.size());
}

namespace {
// A small deterministic PRNG for the mutation loops.
struct rng {
  std::uint64_t s = 0x9E3779B97F4A7C15ull;
  std::uint32_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return static_cast<std::uint32_t>(s);
  }
};

template <typename F>
void abuse(const bytes& good, F&& parse) {
  for (std::size_t n = 0; n < good.size(); n += std::max<std::size_t>(1, good.size() / 300)) {
    parse(bytes(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n)));
  }
  rng r;
  for (int i = 0; i < 1500; ++i) {
    bytes b = good;
    const int flips = 1 + static_cast<int>(r.next() % 8);
    for (int k = 0; k < flips; ++k) b[r.next() % b.size()] = static_cast<std::uint8_t>(r.next());
    parse(b);
  }
}
}  // namespace

TEST_CASE("every reader survives truncated and mutated input", "[meta][provenance][fuzz]") {
  const bytes store = c2pa_store("ChatGPT", kTrained, "GPT-4o");
  const auto run = [](auto&& fn) {
    return [fn](const bytes& b) {
      std::vector<mv::meta::property> rows;
      mv::meta::parse::evidence ev;
      mv::meta::parse::sink sink{rows, ev, 4000};
      fn(b, sink);
      CHECK(rows.size() <= 4000);
    };
  };
  abuse(store, run([](const bytes& b, mv::meta::parse::sink& s) { mv::meta::parse::read_c2pa(b, s); }));
  abuse(jpeg_with_segments(grey_jpeg(), app11(store, 90)), run([](const bytes& b, mv::meta::parse::sink& s) {
          mv::meta::parse::read_jpeg(b, s);
        }));
  abuse(png_with(small_png(), {png_chunk("caBX", store), ztxt_chunk("Comment", "abc"),
                               itxt_chunk("workflow", "{\"a\":1}", true), text_chunk("parameters", "x")}),
        run([](const bytes& b, mv::meta::parse::sink& s) { mv::meta::parse::read_png(b, s); }));
  abuse(icc_profile(), run([](const bytes& b, mv::meta::parse::sink& s) { (void)mv::meta::parse::read_icc(b, s); }));
  const std::string xmp =
      "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
      "<rdf:Description xmlns:a=\"urn:a\" a:x=\"1\"><a:y><rdf:Bag><rdf:li>p</rdf:li><rdf:li rdf:parseType=\"Resource\">"
      "<a:z>q</a:z></rdf:li></rdf:Bag></a:y></rdf:Description></rdf:RDF></x:xmpmeta>";
  abuse(bytes(xmp.begin(), xmp.end()), run([](const bytes& b, mv::meta::parse::sink& s) {
          mv::meta::parse::read_xmp(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()), nullptr,
                                    nullptr, s);
        }));
  bytes webp;
  append(webp, std::string_view("RIFF"));
  webp.insert(webp.end(), {40, 0, 0, 0});
  append(webp, std::string_view("WEBPVP8X"));
  webp.insert(webp.end(), {10, 0, 0, 0, 0x3E, 0, 0, 0, 99, 0, 0, 49, 0, 0});
  append(webp, std::string_view("ANIM"));
  webp.insert(webp.end(), {6, 0, 0, 0, 1, 2, 3, 4, 0, 0});
  abuse(webp, run([](const bytes& b, mv::meta::parse::sink& s) { mv::meta::parse::read_webp(b, s); }));
  bytes bmff = box("ftyp", bytes{'h', 'e', 'i', 'c', 0, 0, 0, 0, 'm', 'i', 'f', '1'});
  append(bmff, box("meta", bytes(4, 0)));
  bytes uuid{0xD8, 0xFE, 0xC3, 0xD6, 0x1B, 0x0E, 0x48, 0x3C, 0x92, 0x97, 0x58, 0x28, 0x87, 0x7E, 0xC4, 0x81, 0, 0, 0, 0};
  append(uuid, std::string_view("manifest\0\0\0\0\0\0\0\0\0", 17));
  append(uuid, store);
  append(bmff, box("uuid", uuid));
  abuse(bmff, run([](const bytes& b, mv::meta::parse::sink& s) {
          mv::meta::parse::memory_source src(b);
          mv::meta::parse::read_bmff(src, true, s);
        }));
  std::string out;
  CHECK_FALSE(mv::meta::parse::inflate_bounded(zlib(std::string(100000, 'a')), 1000, out));
  CHECK(out.size() == 1000);  // the cap holds against a zip bomb
}

// Writes the meta_* fuzz seeds (tests/data/seeds/{c2pa,xmp,icc}, README) from the
// builders above. Hidden: `MV_SEED_DIR=tests/data/seeds mv_tests "[.write-seeds]"`.
TEST_CASE("write the metadata fuzz seeds", "[.write-seeds]") {
  const char* dir = std::getenv("MV_SEED_DIR");
  if (!dir) SKIP("set MV_SEED_DIR");
  const auto put = [&](const std::string& family, const std::string& file, const bytes& b) {
    const fs::path d = fs::path(dir) / family;
    fs::create_directories(d);
    std::ofstream f(d / file, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  };
  const bytes store = c2pa_store("ChatGPT", kTrained, "GPT-4o");
  put("c2pa", "store.jumbf", store);
  put("c2pa", "chatgpt_app11.jpg", jpeg_with_segments(grey_jpeg(), app11(store, 200)));
  put("c2pa", "cabx_text.png",
      png_with(small_png(), {png_chunk("caBX", store), text_chunk("parameters", "a fox\nSteps: 20, Version: v1.10"),
                             ztxt_chunk("Comment", "zipped"), itxt_chunk("workflow", "{\"nodes\":[]}", true, "en")}));
  bytes uuid{0xD8, 0xFE, 0xC3, 0xD6, 0x1B, 0x0E, 0x48, 0x3C, 0x92, 0x97, 0x58, 0x28, 0x87, 0x7E, 0xC4, 0x81, 0, 0, 0, 0};
  append(uuid, std::string_view("manifest\0\0\0\0\0\0\0\0\0", 17));
  append(uuid, store);
  bytes bmff = box("ftyp", bytes{'i', 's', 'o', 'm', 0, 0, 0, 0, 'm', 'p', '4', '1'});
  append(bmff, box("uuid", uuid));
  put("c2pa", "uuid.mp4", bmff);
  put("icc", "display_p3_like.icc", icc_profile());
  const std::string xmp =
      "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
      "<rdf:Description rdf:about=\"\" xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\" xmlns:mjx=\"urn:mjx\""
      " xmlns:xmpMM=\"http://ns.adobe.com/xap/1.0/mm/\" xmlns:stEvt=\"http://ns.adobe.com/xap/1.0/sType/ResourceEvent#\""
      " xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmp:CreatorTool=\"Midjourney\" mjx:JobId=\"1\">"
      "<dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">Fox</rdf:li></rdf:Alt></dc:title>"
      "<mjx:S rdf:parseType=\"Resource\"><mjx:A>1</mjx:A></mjx:S>"
      "<xmpMM:History><rdf:Seq><rdf:li stEvt:action=\"saved\"/><rdf:li><rdf:Description stEvt:action=\"x\"/>"
      "</rdf:li></rdf:Seq></xmpMM:History></rdf:Description></rdf:RDF></x:xmpmeta>";
  put("xmp", "midjourney_like.xmp", bytes(xmp.begin(), xmp.end()));
}

// ---- #345: the badge's probe gives the full read's answer ------------------------
namespace {

void le16(bytes& out, std::uint32_t v) {
  out.push_back(static_cast<std::uint8_t>(v));
  out.push_back(static_cast<std::uint8_t>(v >> 8));
}
void le32(bytes& out, std::uint32_t v) {
  le16(out, v & 0xFFFF);
  le16(out, v >> 16);
}

// A little-endian TIFF block whose IFD0 holds only Software.
bytes tiff_software(std::string_view software) {
  bytes t{'I', 'I', 42, 0};
  le32(t, 8);
  le16(t, 1);
  le16(t, 0x0131);
  le16(t, 2);
  le32(t, static_cast<std::uint32_t>(software.size() + 1));
  le32(t, 8 + 2 + 12 + 4);
  le32(t, 0);  // no next IFD
  append(t, software);
  t.push_back(0);
  return t;
}
bytes exif_app1(std::string_view software) {
  bytes p;
  append(p, std::string_view("Exif\0\0", 6));
  append(p, tiff_software(software));
  return segment(0xE1, p);
}
bytes xmp_app1(const std::string& packet) {
  bytes p;
  append(p, std::string_view("http://ns.adobe.com/xap/1.0/\0", 29));
  append(p, packet);
  return segment(0xE1, p);
}
std::string xmp_packet(std::string_view attrs) {
  return "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
         "<rdf:Description rdf:about=\"\" xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\""
         " xmlns:Iptc4xmpExt=\"http://iptc.org/std/Iptc4xmpExt/2008-02-29/\" " +
         std::string(attrs) + "/></rdf:RDF></x:xmpmeta>";
}

// The probe and meta::read agree on all three facts; returns the probe's answer.
mv::meta::provenance same_answer(const std::string& name, const bytes& b) {
  temp_file f(name);
  f.write(b);
  const auto m = mv::meta::read(f.utf8());
  REQUIRE(m);
  const mv::meta::provenance p = mv::meta::probe_provenance(f.utf8());
  INFO(name);
  CHECK(p.made_with == m->s.made_with);
  CHECK(p.ai_generated == m->s.ai_generated);
  CHECK(p.evidence == m->s.ai_evidence);
  return p;
}

}  // namespace

TEST_CASE("the provenance probe agrees with the full read, fixture by fixture", "[meta][provenance][probe]") {
  const std::string params = "a fox\nSteps: 20, Seed: 1, Model: sdxl, Version: v1.10.1";
  // ChatGPT: C2PA in APP11 segments, behind a camera-style EXIF and before the scan.
  std::vector<bytes> segs{exif_app1("Firmware 1.0")};
  for (auto& s : app11(c2pa_store("ChatGPT", kTrained, "GPT-4o"), 120)) segs.push_back(s);
  auto p = same_answer("p_chatgpt.jpg", jpeg_with_segments(grey_jpeg(), segs));
  CHECK(p.ai_generated);
  CHECK(p.made_with == "ChatGPT");
  CHECK(p.evidence == "Content Credentials (C2PA): AI-generated (trainedAlgorithmicMedia)");

  // Firefly: segments out of order.
  segs = app11(c2pa_store("Adobe Firefly", kTrained, "Adobe Firefly"), 64, 7);
  std::reverse(segs.begin(), segs.end());
  p = same_answer("p_firefly.jpg", jpeg_with_segments(grey_jpeg(), segs));
  CHECK(p.made_with == "Adobe Firefly");

  // Midjourney-style XMP CreatorTool; an IPTC source type in XMP.
  p = same_answer("p_mj.jpg", jpeg_with_segments(grey_jpeg(), {xmp_app1(xmp_packet("xmp:CreatorTool=\"Midjourney 6\""))}));
  CHECK(p.evidence == "XMP CreatorTool: Midjourney 6");
  p = same_answer("p_iptc.jpg", jpeg_with_segments(grey_jpeg(), {xmp_app1(xmp_packet(
                                    "Iptc4xmpExt:DigitalSourceType=\"" + std::string(kTrained) + "\""))}));
  CHECK(p.ai_generated);
  CHECK(p.evidence.rfind("IPTC digital source type (XMP): ", 0) == 0);
  // A generator in EXIF Software.
  p = same_answer("p_exif.jpg", jpeg_with_segments(grey_jpeg(), {exif_app1("Adobe Firefly 2.1")}));
  CHECK(p.evidence == "EXIF Software: Adobe Firefly 2.1");
  // A camera JPEG with an editor's XMP, and a bare JPEG: nothing.
  p = same_answer("p_camera.jpg",
                  jpeg_with_segments(grey_jpeg(), {exif_app1("Firmware Version 1.8.1"),
                                                   xmp_app1(xmp_packet("xmp:CreatorTool=\"Adobe Lightroom\""))}));
  CHECK_FALSE(p.ai_generated);
  CHECK(p.made_with.empty());
  CHECK_FALSE(same_answer("p_plain.jpg", grey_jpeg()).ai_generated);

  // PNG: caBX plus generator chunks; the chunks alone; ComfyUI; the XMP packet; plain.
  p = same_answer("p_sd.png", png_with(small_png(), {png_chunk("caBX", c2pa_store("Microsoft Designer", kTrained, "DALL-E 3")),
                                                    text_chunk("parameters", params)}));
  CHECK(p.made_with == "Microsoft Designer");
  p = same_answer("p_sd2.png", png_with(small_png(), {text_chunk("parameters", params)}));
  CHECK(p.ai_generated);
  p = same_answer("p_comfy.png", png_with(small_png(), {itxt_chunk("prompt", "{\"3\": {}}", false),
                                                       ztxt_chunk("workflow", "{\"nodes\": []}")}));
  CHECK(p.made_with == "ComfyUI");
  p = same_answer("p_xmp.png", png_with(small_png(), {itxt_chunk("XML:com.adobe.xmp",
                                                                 xmp_packet("xmp:CreatorTool=\"Midjourney\""), false)}));
  CHECK(p.made_with == "Midjourney");
  CHECK_FALSE(same_answer("p_plain.png", png_with(small_png(), {text_chunk("Author", "Me")})).ai_generated);
  // Text after the image data counts too (the probe seeks past IDAT).
  bytes late = small_png();
  const bytes tail = text_chunk("parameters", params);
  late.insert(late.end() - 12, tail.begin(), tail.end());  // just before IEND
  p = same_answer("p_late.png", late);
  CHECK(p.ai_generated);

  // A clip with its C2PA box after a large mdat (Sora / Veo layout).
  bytes uuid{0xD8, 0xFE, 0xC3, 0xD6, 0x1B, 0x0E, 0x48, 0x3C, 0x92, 0x97, 0x58, 0x28, 0x87, 0x7E, 0xC4, 0x81, 0, 0, 0, 0};
  append(uuid, std::string_view("manifest\0\0\0\0\0\0\0\0\0", 17));
  append(uuid, c2pa_store("Sora", kTrained, "Sora"));
  bytes file = box("ftyp", bytes{'i', 's', 'o', 'm', 0, 0, 0, 0, 'i', 's', 'o', 'm'});
  append(file, box("mdat", bytes(1 << 20, 0)));
  append(file, box("uuid", uuid));
  p = same_answer("p_sora.mp4", file);
  CHECK(p.made_with == "Sora");
  CHECK(p.ai_generated);
}

TEST_CASE("the provenance probe: WebP chunks, damage, and files it cannot read", "[meta][provenance][probe]") {
  // WebP: an XMP chunk after the bitstream (an odd-sized one, padded).
  bytes riff;
  append(riff, std::string_view("WEBP"));
  bytes chunk;
  append(chunk, std::string_view("VP8X"));
  le32(chunk, 10);
  bytes vp8x(10, 0);
  vp8x[0] = 0x0C;  // EXIF + XMP
  append(chunk, vp8x);
  append(chunk, std::string_view("VP8 "));
  le32(chunk, 3);
  append(chunk, bytes{1, 2, 3, 0});
  const std::string packet = xmp_packet("xmp:CreatorTool=\"Ideogram 2.0\"");
  append(chunk, std::string_view("XMP "));
  le32(chunk, static_cast<std::uint32_t>(packet.size()));
  append(chunk, packet);
  if (packet.size() & 1) chunk.push_back(0);
  append(riff, chunk);
  bytes webp;
  append(webp, std::string_view("RIFF"));
  le32(webp, static_cast<std::uint32_t>(riff.size()));
  append(webp, riff);
  {
    temp_file f("p.webp");
    f.write(webp);
    const auto p = mv::meta::probe_provenance(f.utf8());
    CHECK(p.made_with == "Ideogram");
    CHECK(p.evidence == "XMP CreatorTool: Ideogram 2.0");
  }

  // Truncated and mutated: never a crash.
  const bytes jpeg = jpeg_with_segments(grey_jpeg(), app11(c2pa_store("ChatGPT", kTrained, "GPT-4o"), 120));
  for (std::size_t cut : {std::size_t{3}, std::size_t{40}, std::size_t{200}, jpeg.size() / 2}) {
    temp_file f("p_cut.jpg");
    f.write(bytes(jpeg.begin(), jpeg.begin() + static_cast<std::ptrdiff_t>(cut)));
    (void)mv::meta::probe_provenance(f.utf8());
  }
  bytes bad = jpeg;
  for (std::size_t i = 4; i < bad.size(); i += 37) bad[i] ^= 0x5A;
  {
    temp_file f("p_bad.jpg");
    f.write(bad);
    (void)mv::meta::probe_provenance(f.utf8());
  }
  // Missing and empty files have no provenance.
  CHECK_FALSE(mv::meta::probe_provenance((fs::temp_directory_path() / "mv_prov_absent.jpg").string()).ai_generated);
  temp_file empty("p_empty.jpg");
  empty.write({});
  CHECK(mv::meta::probe_provenance(empty.utf8()) == mv::meta::provenance{});
}
