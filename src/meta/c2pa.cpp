// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #290: C2PA / Content Credentials, read only (docs/design/06 "Beyond Exiv2").
//
// A manifest store is a JUMBF superbox (ISO/IEC 19566-5) labelled "c2pa"; each
// manifest in it is a superbox holding an assertion store, a claim and a
// signature, whose content boxes are CBOR (RFC 8949) or JSON. The signature is a
// COSE_Sign1 whose x5chain carries the signer's X.509 certificate (DER) and whose
// sigTst carries an RFC 3161 timestamp token.
//
// Nothing here validates anything: a hash, a signature, a certificate chain and
// a timestamp are shown for what they say, never checked, and never looked up
// online (rule 6). Every structure is bounded: nesting, item counts, box counts,
// string lengths and rows.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "core/json.h"
#include "meta/generators.h"
#include "meta/parse.h"
#include "meta/parse_util.h"

namespace mv::meta::parse {
namespace {

using util::bytes;

constexpr int kMaxCborDepth = 24;
constexpr std::size_t kMaxCborItems = 20000;
constexpr std::size_t kMaxManifests = 64;
constexpr std::size_t kMaxBoxes = 512;
constexpr std::size_t kMaxRawRows = 1500;
constexpr const char* kGroup = "Content Credentials (C2PA)";
constexpr const char* kRawGroup = "C2PA assertions";

// ---- CBOR ---------------------------------------------------------------------

struct cbor {
  enum class type : std::uint8_t { uint, nint, bytes, text, array, map, tag, simple, real, invalid };
  type t = type::invalid;
  std::uint64_t u = 0;    // uint / nint argument, tag number, simple value
  double f = 0;
  std::string_view s;     // bytes / text, a view into the decoded buffer
  std::vector<cbor> items;  // array elements; map keys and values interleaved; a tag's one value

  [[nodiscard]] const cbor* get(std::string_view key) const noexcept {
    if (t != type::map) return nullptr;
    for (std::size_t i = 0; i + 1 < items.size(); i += 2) {
      if (items[i].t == type::text && items[i].s == key) return &items[i + 1];
    }
    return nullptr;
  }
  [[nodiscard]] const cbor* get(std::int64_t key) const noexcept {
    if (t != type::map) return nullptr;
    for (std::size_t i = 0; i + 1 < items.size(); i += 2) {
      const cbor& k = items[i];
      if ((k.t == type::uint && key >= 0 && k.u == static_cast<std::uint64_t>(key)) ||
          (k.t == type::nint && key < 0 && k.u == static_cast<std::uint64_t>(-1 - key))) {
        return &items[i + 1];
      }
    }
    return nullptr;
  }
  // Through any tags to the value they wrap.
  [[nodiscard]] const cbor& untagged() const noexcept {
    const cbor* c = this;
    for (int i = 0; i < 8 && c->t == type::tag && !c->items.empty(); ++i) c = &c->items[0];
    return *c;
  }
  [[nodiscard]] std::string_view text() const noexcept {
    const cbor& c = untagged();
    return c.t == type::text ? c.s : std::string_view{};
  }
};

class cbor_decoder {
 public:
  explicit cbor_decoder(bytes b) noexcept : b_(b) {}

  bool decode(cbor& out) { return item(out, 0) && at_ <= b_.size(); }
  [[nodiscard]] std::size_t position() const noexcept { return at_; }

 private:
  bool head(std::uint8_t& major, std::uint8_t& info, std::uint64_t& arg) noexcept {
    if (at_ >= b_.size()) return false;
    const std::uint8_t ib = b_[at_++];
    major = static_cast<std::uint8_t>(ib >> 5);
    info = static_cast<std::uint8_t>(ib & 0x1F);
    if (info < 24) {
      arg = info;
      return true;
    }
    std::size_t n = 0;
    switch (info) {
      case 24: n = 1; break;
      case 25: n = 2; break;
      case 26: n = 4; break;
      case 27: n = 8; break;
      case 31: arg = 0; return true;  // indefinite
      default: return false;
    }
    if (!util::has(b_, at_, n)) return false;
    arg = 0;
    for (std::size_t i = 0; i < n; ++i) arg = (arg << 8) | b_[at_ + i];
    at_ += n;
    return true;
  }

  bool item(cbor& out, int depth) {
    if (depth > kMaxCborDepth || ++count_ > kMaxCborItems) return false;
    std::uint8_t major = 0, info = 0;
    std::uint64_t arg = 0;
    if (!head(major, info, arg)) return false;
    const bool indefinite = info == 31;
    switch (major) {
      case 0: out.t = cbor::type::uint; out.u = arg; return !indefinite;
      case 1: out.t = cbor::type::nint; out.u = arg; return !indefinite;
      case 2:
      case 3: {
        if (indefinite) return false;  // chunked strings: not used by C2PA, refused
        if (arg > b_.size() - std::min(at_, b_.size()) || !util::has(b_, at_, static_cast<std::size_t>(arg))) {
          return false;
        }
        out.t = major == 2 ? cbor::type::bytes : cbor::type::text;
        out.s = std::string_view(reinterpret_cast<const char*>(b_.data() + at_), static_cast<std::size_t>(arg));
        at_ += static_cast<std::size_t>(arg);
        return true;
      }
      case 4:
      case 5: {
        out.t = major == 4 ? cbor::type::array : cbor::type::map;
        const std::uint64_t per = major == 4 ? 1 : 2;
        if (indefinite) {
          for (;;) {
            if (at_ >= b_.size()) return false;
            if (b_[at_] == 0xFF) {
              ++at_;
              return out.t == cbor::type::array || out.items.size() % 2 == 0;
            }
            if (out.items.size() >= kMaxCborItems) return false;
            out.items.emplace_back();
            if (!item(out.items.back(), depth + 1)) return false;
          }
        }
        // Every item takes at least one byte: a count past the bytes left is a lie.
        if (arg > (b_.size() - std::min(at_, b_.size())) || arg * per > kMaxCborItems) return false;
        out.items.reserve(static_cast<std::size_t>(arg * per));
        for (std::uint64_t i = 0; i < arg * per; ++i) {
          out.items.emplace_back();
          if (!item(out.items.back(), depth + 1)) return false;
        }
        return true;
      }
      case 6:
        if (indefinite) return false;
        out.t = cbor::type::tag;
        out.u = arg;
        out.items.emplace_back();
        return item(out.items.back(), depth + 1);
      default: {  // 7: simple values and floats
        if (indefinite) return false;  // a stray break
        if (info == 25) {
          const auto h = static_cast<std::uint16_t>(arg);
          const int e = (h >> 10) & 0x1F, m = h & 0x3FF;
          double v = e == 0 ? std::ldexp(m, -24) : e == 31 ? (m ? 0.0 : 1e300) : std::ldexp(m + 1024, e - 25);
          out.t = cbor::type::real;
          out.f = (h & 0x8000) ? -v : v;
          return true;
        }
        if (info == 26) {
          float v = 0;
          const auto w = static_cast<std::uint32_t>(arg);
          std::memcpy(&v, &w, sizeof(v));
          out.t = cbor::type::real;
          out.f = static_cast<double>(v);
          return true;
        }
        if (info == 27) {
          double v = 0;
          std::memcpy(&v, &arg, sizeof(v));
          out.t = cbor::type::real;
          out.f = v;
          return true;
        }
        out.t = cbor::type::simple;
        out.u = arg;
        return true;
      }
    }
  }

  bytes b_;
  std::size_t at_ = 0;
  std::size_t count_ = 0;
};

bool decode_cbor(bytes b, cbor& out) {
  cbor_decoder d(b);
  return d.decode(out);
}

std::string scalar_text(const cbor& c) {
  switch (c.t) {
    case cbor::type::uint: return std::to_string(c.u);
    case cbor::type::nint:
      return c.u >= 0x7FFFFFFFFFFFFFFFull ? std::string("-huge") : "-" + std::to_string(c.u + 1);
    case cbor::type::text: return std::string(c.s);
    case cbor::type::bytes: return std::to_string(c.s.size()) + " bytes";
    case cbor::type::real: return util::number_text(c.f);
    case cbor::type::simple:
      return c.u == 20 ? "false" : c.u == 21 ? "true" : c.u == 22 ? "null" : c.u == 23 ? "undefined"
                                                                           : "simple(" + std::to_string(c.u) + ")";
    default: return {};
  }
}

// Leaves of a CBOR value as "path = value" rows.
void flatten(const cbor& c, const std::string& path, const std::string& tag_prefix, sink& out,
             std::size_t& budget, int depth) {
  if (budget == 0 || out.full() || depth > kMaxCborDepth) return;
  const cbor& v = c.untagged();
  if (v.t == cbor::type::map) {
    for (std::size_t i = 0; i + 1 < v.items.size() && budget > 0; i += 2) {
      const cbor& k = v.items[i].untagged();
      const std::string key = k.t == cbor::type::text ? std::string(k.s) : scalar_text(k);
      flatten(v.items[i + 1], path.empty() ? key : path + "." + key, tag_prefix, out, budget, depth + 1);
    }
    return;
  }
  if (v.t == cbor::type::array) {
    for (std::size_t i = 0; i < v.items.size() && budget > 0; ++i) {
      flatten(v.items[i], path + "[" + std::to_string(i + 1) + "]", tag_prefix, out, budget, depth + 1);
    }
    return;
  }
  --budget;
  (void)out.add(origin::c2pa, kRawGroup, path, path, scalar_text(v), tag_prefix + path);
}

void flatten_json(const json::value& v, const std::string& path, const std::string& tag_prefix, sink& out,
                  std::size_t& budget, int depth) {
  if (budget == 0 || out.full() || depth > kMaxCborDepth) return;
  switch (v.k) {
    case json::kind::object:
      for (const auto& [k, child] : v.o) {
        if (budget == 0) return;
        flatten_json(child, path.empty() ? k : path + "." + k, tag_prefix, out, budget, depth + 1);
      }
      return;
    case json::kind::array:
      for (std::size_t i = 0; i < v.a.size() && budget > 0; ++i) {
        flatten_json(v.a[i], path + "[" + std::to_string(i + 1) + "]", tag_prefix, out, budget, depth + 1);
      }
      return;
    default: break;
  }
  std::string text;
  switch (v.k) {
    case json::kind::null: text = "null"; break;
    case json::kind::boolean: text = v.b ? "true" : "false"; break;
    case json::kind::number: text = v.is_integer ? std::to_string(v.i) : util::number_text(v.d); break;
    case json::kind::string: text = v.s; break;
    default: break;
  }
  --budget;
  (void)out.add(origin::c2pa, kRawGroup, path, path, text, tag_prefix + path);
}

// ---- JUMBF ------------------------------------------------------------------------

struct box {
  std::uint32_t type = 0;
  bytes payload;
};

constexpr std::uint32_t code(const char (&c)[5]) noexcept {
  return (static_cast<std::uint32_t>(static_cast<unsigned char>(c[0])) << 24) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(c[1])) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(c[2])) << 8) |
         static_cast<std::uint32_t>(static_cast<unsigned char>(c[3]));
}

// One ISO box at `at` (32- or 64-bit size, 0 = to the end).
bool next_box(bytes b, std::size_t& at, box& out) noexcept {
  if (!util::has(b, at, 8)) return false;
  std::uint64_t size = util::be32(b, at);
  out.type = util::be32(b, at + 4);
  std::size_t header = 8;
  if (size == 1) {
    if (!util::has(b, at, 16)) return false;
    size = util::be64(b, at + 8);
    header = 16;
  } else if (size == 0) {
    size = b.size() - at;
  }
  if (size < header || size > b.size() - at) return false;
  out.payload = b.subspan(at + header, static_cast<std::size_t>(size) - header);
  at += static_cast<std::size_t>(size);
  return true;
}

struct superbox {
  std::string label;
  std::vector<box> content;  // every box after the description box
};

bool read_superbox(bytes payload, superbox& out) {
  std::size_t at = 0;
  box d;
  if (!next_box(payload, at, d) || d.type != code("jumd") || d.payload.size() < 17) return false;
  const std::uint8_t toggles = d.payload[16];
  if (toggles & 0x02) {
    std::size_t next = 0;
    out.label = std::string(util::cstr(d.payload, 17, next));
  }
  box b;
  while (out.content.size() < kMaxBoxes && next_box(payload, at, b)) out.content.push_back(b);
  return true;
}

// ---- DER (X.509 names, RFC 3161 genTime) -------------------------------------------

struct der {
  std::uint8_t tag = 0;
  bytes content;
};

bool der_next(bytes b, std::size_t& at, der& out) noexcept {
  if (!util::has(b, at, 2)) return false;
  out.tag = b[at];
  if ((out.tag & 0x1F) == 0x1F) return false;  // high tag numbers: not in these structures
  std::size_t len = b[at + 1];
  std::size_t p = at + 2;
  if (len & 0x80) {
    const std::size_t n = len & 0x7F;
    if (n == 0 || n > 4 || !util::has(b, p, n)) return false;  // indefinite or absurd
    len = 0;
    for (std::size_t i = 0; i < n; ++i) len = (len << 8) | b[p + i];
    p += n;
  }
  if (!util::has(b, p, len)) return false;
  out.content = b.subspan(p, len);
  at = p + len;
  return true;
}

std::string der_string(const der& d) {
  switch (d.tag) {
    case 0x0C: case 0x13: case 0x16: case 0x14: case 0x1A:
      return latin1_or_utf8(util::chars(d.content));
    case 0x1E: {  // BMPString: UTF-16BE, the BMP only
      std::string out;
      for (std::size_t i = 0; i + 1 < d.content.size(); i += 2) {
        const std::uint32_t cp = util::be16(d.content, i);
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
          out += static_cast<char>(0xC0 | (cp >> 6));
          out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0xD800 || cp > 0xDFFF) {
          out += static_cast<char>(0xE0 | (cp >> 12));
          out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
          out += static_cast<char>(0x80 | (cp & 0x3F));
        }
      }
      return out;
    }
    default: return {};
  }
}

struct x509_name {
  std::string cn, o;
  [[nodiscard]] std::string display() const {
    if (!o.empty() && !cn.empty() && cn != o) return o + " (" + cn + ")";
    return o.empty() ? cn : o;
  }
};

x509_name read_name(bytes name) {
  x509_name n;
  std::size_t at = 0;
  der set;
  for (int guard = 0; guard < 64 && der_next(name, at, set); ++guard) {
    if (set.tag != 0x31) continue;
    std::size_t at2 = 0;
    der atv;
    while (der_next(set.content, at2, atv)) {
      if (atv.tag != 0x30) continue;
      std::size_t at3 = 0;
      der oid, val;
      if (!der_next(atv.content, at3, oid) || oid.tag != 0x06 || !der_next(atv.content, at3, val)) continue;
      if (oid.content.size() == 3 && oid.content[0] == 0x55 && oid.content[1] == 0x04) {
        if (oid.content[2] == 0x03 && n.cn.empty()) n.cn = der_string(val);
        if (oid.content[2] == 0x0A && n.o.empty()) n.o = der_string(val);
      }
    }
  }
  return n;
}

struct cert_names {
  x509_name subject, issuer;
};

std::optional<cert_names> read_certificate(bytes cert) {
  std::size_t at = 0;
  der c, tbs;
  if (!der_next(cert, at, c) || c.tag != 0x30) return std::nullopt;
  at = 0;
  if (!der_next(c.content, at, tbs) || tbs.tag != 0x30) return std::nullopt;
  std::vector<der> fields;
  at = 0;
  der f;
  while (fields.size() < 8 && der_next(tbs.content, at, f)) fields.push_back(f);
  std::size_t i = !fields.empty() && fields[0].tag == 0xA0 ? 1 : 0;  // [0] version
  // serial, signature algorithm, issuer, validity, subject
  if (fields.size() < i + 5 || fields[i + 2].tag != 0x30 || fields[i + 4].tag != 0x30) return std::nullopt;
  cert_names out;
  out.issuer = read_name(fields[i + 2].content);
  out.subject = read_name(fields[i + 4].content);
  return out;
}

// The first GeneralizedTime in a timestamp token, depth first. A TSTInfo's
// genTime comes before the certificates and signer attributes in SignedData.
bool find_gentime(bytes b, int depth, std::size_t& nodes, std::string& out) {
  if (depth > 12) return false;
  std::size_t at = 0;
  der d;
  while (++nodes < 4000 && der_next(b, at, d)) {
    if (d.tag == 0x18) {
      out = std::string(util::chars(d.content));
      return true;
    }
    const bool constructed = (d.tag & 0x20) != 0;
    if ((constructed || d.tag == 0x04) && find_gentime(d.content, depth + 1, nodes, out)) return true;
  }
  return false;
}

// "20240501140322Z" / "20240501140322.123Z" → "2024-05-01 14:03:22 UTC".
std::string pretty_gentime(const std::string& g) {
  if (g.size() < 14) return g;
  for (std::size_t i = 0; i < 14; ++i) {
    if (g[i] < '0' || g[i] > '9') return g;
  }
  std::string out = g.substr(0, 4) + "-" + g.substr(4, 2) + "-" + g.substr(6, 2) + " " + g.substr(8, 2) + ":" +
                    g.substr(10, 2) + ":" + g.substr(12, 2);
  return g.back() == 'Z' ? out + " UTC" : out;
}

// ---- The manifest ------------------------------------------------------------------

struct manifest {
  std::string label;
  std::vector<std::pair<std::string, box>> assertions;  // label, content box
  std::optional<box> claim;
  std::optional<box> signature;
};

std::optional<box> first_content(const superbox& s) {
  for (const box& b : s.content) {
    if (b.type == code("cbor") || b.type == code("json")) return b;
  }
  return std::nullopt;
}

manifest read_manifest(const superbox& m) {
  manifest out;
  out.label = m.label;
  for (const box& b : m.content) {
    if (b.type != code("jumb")) continue;
    superbox child;
    if (!read_superbox(b.payload, child)) continue;
    if (child.label == "c2pa.assertions") {
      for (const box& a : child.content) {
        if (a.type != code("jumb") || out.assertions.size() >= kMaxBoxes) continue;
        superbox as;
        if (!read_superbox(a.payload, as)) continue;
        if (auto c = first_content(as)) out.assertions.emplace_back(as.label, *c);
        else out.assertions.emplace_back(as.label, box{});  // embedded data (a thumbnail): named only
      }
    } else if (child.label.rfind("c2pa.claim", 0) == 0) {
      out.claim = first_content(child);
    } else if (child.label == "c2pa.signature") {
      out.signature = first_content(child);
    }
  }
  return out;
}

std::string agent_name(const cbor& agent) {
  const cbor& a = agent.untagged();
  if (a.t == cbor::type::text) return std::string(a.s);
  if (a.t == cbor::type::map) {
    std::string name(a.get("name") ? a.get("name")->text() : std::string_view{});
    if (const cbor* v = a.get("version"); v && !v->text().empty() && !name.empty()) {
      name += " " + std::string(v->text());
    }
    return name;
  }
  return {};
}

std::string claim_generator(const cbor& claim) {
  // v2: one claim_generator_info map; v1: an array of them, and/or claim_generator text.
  if (const cbor* info = claim.get("claim_generator_info")) {
    const cbor& i = info->untagged();
    if (i.t == cbor::type::map) {
      if (std::string n = agent_name(i); !n.empty()) return n;
    } else if (i.t == cbor::type::array && !i.items.empty()) {
      if (std::string n = agent_name(i.items[0]); !n.empty()) return n;
    }
  }
  if (const cbor* g = claim.get("claim_generator")) {
    // "ChatGPT c2pa-rs/0.31.0": the product is the first token(s) before a library tag.
    return std::string(g->text());
  }
  return {};
}

void read_signature(const box& sig, sink& out, bool curated) {
  cbor c;
  if (sig.type != code("cbor") || !decode_cbor(sig.payload, c)) return;
  const cbor& s = c.untagged();
  if (s.t != cbor::type::array || s.items.size() < 2) return;
  cbor protected_map;
  const cbor& p = s.items[0].untagged();
  if (p.t == cbor::type::bytes) {
    (void)decode_cbor(bytes(reinterpret_cast<const std::uint8_t*>(p.s.data()), p.s.size()), protected_map);
  }
  const cbor& unprotected = s.items[1].untagged();
  const cbor* chain = protected_map.get(33);
  if (!chain) chain = unprotected.get(33);
  if (!chain) chain = protected_map.get("x5chain");
  std::string_view leaf;
  if (chain) {
    const cbor& ch = chain->untagged();
    if (ch.t == cbor::type::bytes) leaf = ch.s;
    else if (ch.t == cbor::type::array && !ch.items.empty() && ch.items[0].untagged().t == cbor::type::bytes) {
      leaf = ch.items[0].untagged().s;
    }
  }
  if (!leaf.empty()) {
    if (auto names = read_certificate(bytes(reinterpret_cast<const std::uint8_t*>(leaf.data()), leaf.size()))) {
      const std::string signer = names->subject.display();
      if (curated) {
        if (out.ev.c2pa_signer.empty()) out.ev.c2pa_signer = names->subject.o.empty() ? names->subject.cn : names->subject.o;
        (void)out.add(origin::c2pa, kGroup, "Signer", "Signed by", signer, "C2PA.signature.subject");
        (void)out.add(origin::c2pa, kGroup, "Issuer", "Certificate issuer", names->issuer.display(),
                      "C2PA.signature.issuer");
      }
    }
  }
  for (const char* key : {"sigTst", "sigTst2"}) {
    const cbor* tst = unprotected.get(key);
    if (!tst || !curated) continue;
    const cbor* tokens = tst->untagged().get("tstTokens");
    if (!tokens || tokens->untagged().t != cbor::type::array || tokens->untagged().items.empty()) continue;
    const cbor* val = tokens->untagged().items[0].untagged().get("val");
    if (!val || val->untagged().t != cbor::type::bytes) continue;
    const std::string_view v = val->untagged().s;
    std::string when;
    std::size_t nodes = 0;
    if (find_gentime(bytes(reinterpret_cast<const std::uint8_t*>(v.data()), v.size()), 0, nodes, when)) {
      (void)out.add(origin::c2pa, kGroup, "SigningTime", "Signed at", pretty_gentime(when), "C2PA.signature.time");
      return;
    }
  }
}

void read_actions(const cbor& assertion, std::vector<std::string>& names, sink& out, bool active) {
  const cbor* list = assertion.untagged().get("actions");
  if (!list || list->untagged().t != cbor::type::array) return;
  for (const cbor& a0 : list->untagged().items) {
    const cbor& a = a0.untagged();
    if (const cbor* n = a.get("action"); n && !n->text().empty() && names.size() < 32) {
      names.emplace_back(n->text());
    }
    if (const cbor* d = a.get("digitalSourceType"); d && !d->text().empty()) {
      if (out.ev.source_types.size() < 32) out.ev.source_types.emplace_back(d->text());
    }
    if (const cbor* sa = a.get("softwareAgent"); sa && active) {
      if (std::string n = agent_name(*sa); !n.empty() && out.ev.c2pa_agents.size() < 32) {
        out.ev.c2pa_agents.push_back(std::move(n));
      }
    }
  }
}

}  // namespace

void read_c2pa(std::span<const std::uint8_t> jumbf, sink& out) {
  try {
    std::size_t at = 0;
    box store;
    if (!next_box(jumbf, at, store) || store.type != code("jumb")) return;
    superbox s;
    if (!read_superbox(store.payload, s) || s.label != "c2pa") return;
    std::vector<manifest> manifests;
    for (const box& b : s.content) {
      if (b.type != code("jumb") || manifests.size() >= kMaxManifests) continue;
      superbox m;
      if (read_superbox(b.payload, m)) manifests.push_back(read_manifest(m));
    }
    if (manifests.empty()) return;
    const manifest& active = manifests.back();  // the store's last manifest is the active one

    // The claim: who made this file.
    cbor claim;
    std::string generator;
    if (active.claim && active.claim->type == code("cbor") && decode_cbor(active.claim->payload, claim)) {
      generator = claim_generator(claim.untagged());
    }
    out.ev.c2pa_generator = generator;
    if (!generator.empty()) {
      (void)out.add(origin::c2pa, kGroup, "ClaimGenerator", "Claim generator", generator, "C2PA.claim.generator");
    }
    if (const cbor* t = claim.untagged().get("dc:title"); t && !t->text().empty()) {
      (void)out.add(origin::c2pa, kGroup, "Title", "Title", t->text(), "C2PA.claim.dc:title");
    }

    // Actions and source types from every manifest (an ingredient's AI origin is
    // still evidence); the curated list is the active manifest's.
    std::vector<std::string> action_names;
    std::size_t ingredients = 0;
    for (std::size_t mi = 0; mi < manifests.size(); ++mi) {
      const bool is_active = mi + 1 == manifests.size();
      std::vector<std::string> names;
      for (const auto& [label, content] : manifests[mi].assertions) {
        if (label.rfind("c2pa.ingredient", 0) == 0 && is_active) ++ingredients;
        if (label.rfind("c2pa.actions", 0) != 0 || content.type != code("cbor")) continue;
        cbor a;
        if (decode_cbor(content.payload, a)) read_actions(a, names, out, is_active);
      }
      if (is_active) action_names = std::move(names);
    }
    std::string actions;
    for (const std::string& n : action_names) actions += (actions.empty() ? "" : ", ") + n;
    if (!actions.empty()) (void)out.add(origin::c2pa, kGroup, "Actions", "Actions", actions, "C2PA.actions");
    std::string sources;
    for (const std::string& t : out.ev.source_types) {
      const std::string d = describe_source_type(t);
      if (sources.find(d) == std::string::npos) sources += (sources.empty() ? "" : "; ") + d;
    }
    if (!sources.empty()) {
      (void)out.add(origin::c2pa, kGroup, "DigitalSourceType", "Digital source type", sources,
                    "C2PA.actions.digitalSourceType");
    }
    std::string agents;
    for (const std::string& a : out.ev.c2pa_agents) {
      if (agents.find(a) == std::string::npos) agents += (agents.empty() ? "" : ", ") + a;
    }
    if (!agents.empty()) (void)out.add(origin::c2pa, kGroup, "SoftwareAgent", "Software agent", agents, "C2PA.actions.softwareAgent");
    if (active.signature) read_signature(*active.signature, out, true);
    if (manifests.size() > 1) {
      (void)out.add(origin::c2pa, kGroup, "Manifests", "Manifests", std::to_string(manifests.size()), "C2PA.manifests");
    }
    if (ingredients > 0) {
      (void)out.add(origin::c2pa, kGroup, "Ingredients", "Ingredients", std::to_string(ingredients), "C2PA.ingredients");
    }
    (void)out.add(origin::c2pa, kGroup, "Validation", "Validation",
                  "Not checked: shown as the file states it", "C2PA.validation");

    // Every assertion and the claim, flattened, active manifest first.
    std::size_t budget = kMaxRawRows;
    for (std::size_t n = manifests.size(); n-- > 0 && budget > 0;) {
      const manifest& m = manifests[n];
      const std::string prefix = n + 1 == manifests.size() ? "C2PA." : "C2PA.manifest" + std::to_string(n + 1) + ".";
      for (const auto& [label, content] : m.assertions) {
        if (budget == 0) break;
        if (content.type == code("cbor")) {
          cbor a;
          if (decode_cbor(content.payload, a)) flatten(a, label, prefix, out, budget, 0);
        } else if (content.type == code("json")) {
          if (auto j = json::parse(util::chars(content.payload), kMaxCborDepth)) {
            flatten_json(*j, label, prefix, out, budget, 0);
          }
        } else {
          --budget;
          (void)out.add(origin::c2pa, kRawGroup, label, label, "embedded data", prefix + label);
        }
      }
      if (n + 1 == manifests.size() && claim.t != cbor::type::invalid) {
        flatten(claim, "claim", prefix, out, budget, 0);
      }
    }
  } catch (...) {
    // Allocation failure: keep the rows already added.
  }
}

std::vector<std::uint8_t> jpeg_jumbf(std::span<const std::uint8_t> jpeg) {
  struct piece {
    std::uint32_t seq;
    bytes data;
  };
  std::vector<std::uint8_t> out;
  try {
    if (jpeg.size() < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) return out;
    struct tagged {
      std::uint16_t instance;
      piece p;
    };
    std::vector<tagged> all;
    std::size_t at = 2;
    for (int guard = 0; guard < 4096 && util::has(jpeg, at, 4); ++guard) {
      if (jpeg[at] != 0xFF) break;
      const std::uint8_t marker = jpeg[at + 1];
      if (marker == 0xFF) {  // fill byte
        ++at;
        continue;
      }
      if (marker == 0xD9 || marker == 0xDA) break;  // EOI / SOS: the metadata is behind us
      if ((marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
        at += 2;
        continue;
      }
      const std::size_t len = util::be16(jpeg, at + 2);
      if (len < 2 || !util::has(jpeg, at + 2, len)) break;
      const bytes seg = jpeg.subspan(at + 4, len - 2);
      // APP11: "JP", box instance (2), packet sequence (4), then the box.
      if (marker == 0xEB && seg.size() >= 16 && seg[0] == 'J' && seg[1] == 'P' && util::fourcc_is(seg, 12, "jumb") &&
          all.size() < 4096) {
        all.push_back({util::be16(seg, 2), {util::be32(seg, 4), seg.subspan(8)}});
      }
      at += 2 + len;
    }
    // The store is the JUMBF instance whose first packet's description box is
    // labelled "c2pa": LBox, 'jumb', LBox, 'jumd', a 16-byte type, toggles, label.
    const auto is_store = [](bytes d) {
      return d.size() >= 37 && util::fourcc_is(d, 12, "jumd") && (d[32] & 0x02) != 0 &&
             std::memcmp(d.data() + 33, "c2pa", 4) == 0 && (d.size() == 37 || d[37] == 0);
    };
    std::optional<std::uint16_t> instance;
    std::uint32_t lowest = 0xFFFFFFFFu;
    for (const tagged& t : all) {
      if (t.p.seq <= lowest && is_store(t.p.data)) {
        instance = t.instance;
        lowest = t.p.seq;
      }
    }
    if (!instance) return out;
    std::vector<piece> pieces;
    for (const tagged& t : all) {
      if (t.instance == *instance) pieces.push_back(t.p);
    }
    std::stable_sort(pieces.begin(), pieces.end(), [](const piece& a, const piece& b) { return a.seq < b.seq; });
    for (std::size_t i = 0; i < pieces.size(); ++i) {
      bytes d = pieces[i].data;
      if (i > 0) {
        // Each continuation repeats the box header (LBox, TBox[, XLBox]); skip it.
        const std::size_t header = util::has(d, 0, 4) && util::be32(d, 0) == 1 ? 16 : 8;
        if (d.size() < header) continue;
        d = d.subspan(header);
      }
      if (out.size() + d.size() > (64u << 20)) break;  // 64 MB: no real manifest store is near it
      out.insert(out.end(), d.begin(), d.end());
    }
  } catch (...) {
    out.clear();
  }
  return out;
}

}  // namespace mv::meta::parse
