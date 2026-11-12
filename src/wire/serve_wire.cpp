// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "wire/serve_wire.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <set>

#include "core/json.h"

namespace mv::driftwire::serve {

namespace {

constexpr std::array<std::pair<op, const char*>, 5> kOps = {{{op::roots, "roots"},
                                                             {op::list, "list"},
                                                             {op::stat, "stat"},
                                                             {op::read_range, "read_range"},
                                                             {op::admin, "admin"}}};

void put_u32(std::uint8_t* p, std::uint32_t v) noexcept {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void put_u64(std::uint8_t* p, std::uint64_t v) noexcept {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint32_t get_u32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
         static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
std::uint64_t get_u64(const std::uint8_t* p) noexcept {
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

void write_ops(json::writer& w, std::uint32_t ops) {
  w.key("ops").begin_array();
  for (const auto& [o, name] : kOps) {
    if (ops & op_bit(o)) w.string(name);
  }
  w.end_array();
}

std::uint32_t read_ops(const json::value& doc) {
  const json::value* a = doc.find("ops");
  if (!a || a->k != json::kind::array) return 0;
  std::uint32_t ops = 0;
  for (const auto& v : a->a) {
    if (v.k != json::kind::string) continue;
    if (const auto o = parse_op(v.s)) ops |= op_bit(*o);
  }
  return ops;
}

bool in_range(const std::optional<std::int64_t>& v, std::int64_t lo, std::int64_t hi) noexcept {
  return v && *v >= lo && *v <= hi;
}

}  // namespace

const char* op_name(op o) noexcept {
  for (const auto& [k, name] : kOps) {
    if (k == o) return name;
  }
  return "";
}

std::optional<op> parse_op(std::string_view name) noexcept {
  for (const auto& [k, n] : kOps) {
    if (name == n) return k;
  }
  return std::nullopt;
}

std::string hello_json(std::uint32_t ops) {
  json::writer w;
  w.begin_object().key("v").integer(kVersion).key("min").integer(kMinVersion);
  write_ops(w, ops);
  w.end_object();
  return w.take();
}

std::optional<hello_msg> parse_hello(std::string_view text) {
  if (text.size() > kMaxRequestBytes) return std::nullopt;
  const auto doc = json::parse(text, 4);
  if (!doc) return std::nullopt;
  const auto v = doc->integer("v");
  const auto min = doc->integer("min");
  if (!in_range(v, 1, 1 << 16) || !in_range(min, 1, *v)) return std::nullopt;
  hello_msg h;
  h.version = static_cast<std::uint32_t>(*v);
  h.min_version = static_cast<std::uint32_t>(*min);
  h.ops = read_ops(*doc);
  return h;
}

std::string caps_json(std::uint32_t version, std::uint32_t ops, std::string_view owner_fields) {
  json::writer w;
  w.begin_object().key("v").integer(version);
  write_ops(w, ops);
  w.key("chunk").integer(static_cast<std::int64_t>(kInteractiveChunk));
  w.key("bulk_chunk").integer(static_cast<std::int64_t>(kBulkChunk));
  w.end_object();
  std::string out = w.take();
  if (!owner_fields.empty()) {
    out.pop_back();
    out.append(",").append(owner_fields).append("}");
  }
  return out;
}

std::optional<caps_msg> parse_caps(std::string_view text) {
  if (text.size() > kMaxRequestBytes) return std::nullopt;
  const auto doc = json::parse(text, 4);
  if (!doc) return std::nullopt;
  const auto v = doc->integer("v");
  const auto chunk = doc->integer("chunk");
  const auto bulk = doc->integer("bulk_chunk");
  if (!in_range(v, 1, 1 << 16) || !in_range(chunk, 1, static_cast<std::int64_t>(kBulkChunk)) ||
      !in_range(bulk, 1, static_cast<std::int64_t>(kBulkChunk))) {
    return std::nullopt;
  }
  caps_msg c;
  c.version = static_cast<std::uint32_t>(*v);
  c.ops = read_ops(*doc);
  c.chunk = static_cast<std::uint32_t>(*chunk);
  c.bulk_chunk = static_cast<std::uint32_t>(*bulk);
  if (const json::value* f = doc->find("features"); f && f->k == json::kind::array) {
    for (const auto& e : f->a) {
      if (e.k == json::kind::string && e.s.size() <= 32 && c.features.size() < 16) c.features.push_back(e.s);
    }
  }
  if (const std::string* p = doc->str("passphrase"); p && p->size() <= 16) c.passphrase = *p;
  if (const json::value* g = doc->find("grant"); g && g->k == json::kind::object) {
    const auto expires = g->integer("expires");
    if (expires && *expires >= 0) {
      c.has_grant = true;
      c.grant_expires = *expires;
      c.grant_scoped = g->boolean("scoped") == true;
    }
  }
  return c;
}

std::vector<std::uint8_t> encode_open(const open_request& r) {
  json::writer w;
  w.begin_object().key("op").string(op_name(r.kind));
  if (!r.root.empty()) w.key("root").string(r.root);
  if (!r.rel.empty()) w.key("rel").string(r.rel);
  if (r.kind == op::read_range) {
    w.key("at").integer(static_cast<std::int64_t>(r.at));
    if (r.length != UINT64_MAX) w.key("len").integer(static_cast<std::int64_t>(r.length));
    if (r.bulk) w.key("bulk").boolean(true);
  }
  if (r.credit != 0) w.key("credit").integer(static_cast<std::int64_t>(r.credit));
  w.end_object();
  const std::string text = w.take();
  std::vector<std::uint8_t> out(4 + text.size());
  put_u32(out.data(), r.id);
  std::memcpy(out.data() + 4, text.data(), text.size());
  return out;
}

std::optional<open_request> parse_open(std::span<const std::uint8_t> body, bool& unknown_op) {
  unknown_op = false;
  if (body.size() < 4 || body.size() - 4 > kMaxRequestBytes) return std::nullopt;
  open_request r;
  r.id = get_u32(body.data());
  if (r.id == 0) return std::nullopt;
  const std::string_view text(reinterpret_cast<const char*>(body.data() + 4), body.size() - 4);
  const auto doc = json::parse(text, 4);
  if (!doc) return std::nullopt;
  const std::string* name = doc->str("op");
  if (!name) return std::nullopt;
  const auto o = parse_op(*name);
  if (!o) {
    unknown_op = true;
    return r;  // answered on its own stream
  }
  r.kind = *o;
  if (const auto* root = doc->str("root")) r.root = *root;
  if (const auto* rel = doc->str("rel")) r.rel = *rel;
  if (const json::value* c = doc->find("credit")) {
    if (c->k != json::kind::number || !c->is_integer || c->i < 0 || c->i > kMaxStreamCredit) return std::nullopt;
    r.credit = static_cast<std::uint32_t>(c->i);
  }
  r.length = UINT64_MAX;
  if (r.kind == op::read_range) {
    const auto at = doc->integer("at");
    if (!in_range(at, 0, static_cast<std::int64_t>(kMaxFileBytes))) return std::nullopt;
    r.at = static_cast<std::uint64_t>(*at);
    if (doc->find("len")) {
      const auto len = doc->integer("len");
      if (!in_range(len, 0, static_cast<std::int64_t>(kMaxFileBytes))) return std::nullopt;
      r.length = static_cast<std::uint64_t>(*len);
    }
    if (doc->find("bulk") && !doc->boolean("bulk")) return std::nullopt;
    r.bulk = doc->boolean("bulk").value_or(false);
  }
  return r;
}

std::vector<std::uint8_t> encode_credit(std::uint32_t id, std::uint32_t bytes) {
  std::vector<std::uint8_t> out(8);
  put_u32(out.data(), id);
  put_u32(out.data() + 4, bytes);
  return out;
}

std::optional<credit_msg> parse_credit(std::span<const std::uint8_t> body) noexcept {
  if (body.size() != 8) return std::nullopt;
  return credit_msg{get_u32(body.data()), get_u32(body.data() + 4)};
}

std::vector<std::uint8_t> encode_cancel(std::uint32_t id) {
  std::vector<std::uint8_t> out(4);
  put_u32(out.data(), id);
  return out;
}

std::optional<std::uint32_t> parse_cancel(std::span<const std::uint8_t> body) noexcept {
  if (body.size() != 4) return std::nullopt;
  return get_u32(body.data());
}

void put_data_header(std::span<std::uint8_t, kDataHeaderBytes> out, std::uint32_t id, std::uint64_t offset) noexcept {
  put_u32(out.data(), id);
  put_u64(out.data() + 4, offset);
}

std::optional<data_msg> parse_data(std::span<const std::uint8_t> body) noexcept {
  if (body.size() < kDataHeaderBytes) return std::nullopt;
  data_msg d;
  d.id = get_u32(body.data());
  d.offset = get_u64(body.data() + 4);
  d.bytes = body.subspan(kDataHeaderBytes);
  return d;
}

std::vector<std::uint8_t> encode_end(std::uint32_t id, end_code code, std::string_view json) {
  std::vector<std::uint8_t> out(5 + json.size());
  put_u32(out.data(), id);
  out[4] = static_cast<std::uint8_t>(code);
  if (!json.empty()) std::memcpy(out.data() + 5, json.data(), json.size());
  return out;
}

std::optional<end_msg> parse_end(std::span<const std::uint8_t> body) noexcept {
  if (body.size() < 5 || body[4] > static_cast<std::uint8_t>(end_code::too_many)) return std::nullopt;
  end_msg e;
  e.id = get_u32(body.data());
  e.code = static_cast<end_code>(body[4]);
  e.json = std::string_view(reinterpret_cast<const char*>(body.data() + 5), body.size() - 5);
  return e;
}

std::string roots_json(std::span<const root_info> roots) {
  json::writer w;
  w.begin_object().key("roots").begin_array();
  for (const auto& r : roots) w.begin_object().key("id").string(r.id).key("label").string(r.label).end_object();
  w.end_array().end_object();
  return w.take();
}

std::optional<std::vector<root_info>> parse_roots(std::string_view text) {
  const auto doc = json::parse(text, 4);
  if (!doc) return std::nullopt;
  const json::value* a = doc->find("roots");
  if (!a || a->k != json::kind::array || a->a.size() > kMaxRoots) return std::nullopt;
  std::vector<root_info> out;
  for (const auto& v : a->a) {
    const std::string* id = v.str("id");
    const std::string* label = v.str("label");
    if (!id || !valid_id(*id) || !label || label->size() > kMaxLabelBytes) return std::nullopt;
    out.push_back({*id, *label});
  }
  return out;
}

std::string stat_json(const stat_info& s) {
  json::writer w;
  w.begin_object()
      .key("size")
      .integer(static_cast<std::int64_t>(s.size))
      .key("mtime")
      .integer(s.mtime)
      .key("dir")
      .boolean(s.dir)
      .end_object();
  return w.take();
}

std::optional<stat_info> parse_stat(std::string_view text) {
  const auto doc = json::parse(text, 4);
  if (!doc) return std::nullopt;
  const auto size = doc->integer("size");
  const auto mtime = doc->integer("mtime");
  const auto dir = doc->boolean("dir");
  if (!in_range(size, 0, static_cast<std::int64_t>(kMaxFileBytes)) || !mtime || !dir) return std::nullopt;
  stat_info s;
  s.size = static_cast<std::uint64_t>(*size);
  s.mtime = *mtime;
  s.dir = *dir;
  return s;
}

std::string list_line(const list_entry& e) {
  json::writer w;
  w.begin_object().key("n").string(e.name).key("s").integer(static_cast<std::int64_t>(e.size)).key("m").integer(e.mtime);
  if (e.dir) w.key("d").boolean(true);
  w.end_object();
  std::string line = w.take();
  line.push_back('\n');
  return line;
}

std::optional<std::vector<list_entry>> parse_listing(std::string_view lines) {
  std::vector<list_entry> out;
  while (!lines.empty()) {
    const auto nl = lines.find('\n');
    if (nl == std::string_view::npos) return std::nullopt;  // cut off
    const auto doc = json::parse(lines.substr(0, nl), 4);
    if (!doc) return std::nullopt;
    const std::string* name = doc->str("n");
    const auto size = doc->integer("s");
    const auto mtime = doc->integer("m");
    if (!name || !safe_relative(*name) || name->find('/') != std::string::npos || !in_range(size, 0, INT64_MAX) ||
        !mtime) {
      return std::nullopt;
    }
    list_entry e;
    e.name = *name;
    e.size = static_cast<std::uint64_t>(*size);
    e.mtime = *mtime;
    e.dir = doc->boolean("d").value_or(false);
    out.push_back(std::move(e));
    lines.remove_prefix(nl + 1);
  }
  return out;
}

std::string pair_json(std::string_view code, std::string_view name) {
  json::writer w;
  w.begin_object().key("code").string(code).key("name").string(clean_name(std::string(name))).end_object();
  return w.take();
}

std::optional<pair_msg> parse_pair(std::string_view text) {
  if (text.size() > kMaxRequestBytes) return std::nullopt;
  const auto doc = json::parse(text, 4);
  const std::string* code = doc ? doc->str("code") : nullptr;
  if (!code || code->size() != kPairCodeChars) return std::nullopt;
  pair_msg out;
  out.code = *code;
  if (const std::string* name = doc->str("name")) out.name = clean_name(*name);
  return out;
}

std::vector<std::uint8_t> encode_admin(std::uint32_t id, std::string_view text) {
  std::vector<std::uint8_t> out(4 + text.size());
  put_u32(out.data(), id);
  std::memcpy(out.data() + 4, text.data(), text.size());
  return out;
}

std::optional<admin_msg> parse_admin(std::span<const std::uint8_t> body) noexcept {
  if (body.size() < 4 || body.size() > 4 + kMaxRequestBytes) return std::nullopt;
  admin_msg m;
  m.id = get_u32(body.data());
  if (m.id == 0) return std::nullopt;
  m.json = std::string_view(reinterpret_cast<const char*>(body.data()) + 4, body.size() - 4);
  return m;
}

result<config> parse_config(std::string_view text, bool any_port) {
  if (text.size() > kMaxConfigBytes) return err(status::invalid_arg);
  const auto doc = json::parse(text, 6);
  if (!doc) return err(status::corrupt);
  config c;
  const auto port = doc->integer("port");
  if (!in_range(port, any_port ? 0 : 1, 65535)) return err(status::invalid_arg);
  c.port = static_cast<std::uint16_t>(*port);
  const std::string* name = doc->str("name");
  c.name = name ? clean_name(*name) : std::string("MediaViewer");
  const json::value* roots = doc->find("roots");
  if (!roots || roots->k != json::kind::array || roots->a.empty() || roots->a.size() > kMaxRoots) {
    return err(status::invalid_arg);
  }
  std::set<std::string> seen;
  for (const auto& v : roots->a) {
    const std::string* id = v.str("id");
    const std::string* label = v.str("label");
    const std::string* path = v.str("path");
    if (!id || !valid_id(*id) || !seen.insert(*id).second || !label || label->size() > kMaxLabelBytes || !path ||
        path->empty()) {
      return err(status::invalid_arg);
    }
    // An absolute path: POSIX's, or a Windows drive's.
    const bool posix = path->front() == '/';
    const bool drive = path->size() > 2 && (*path)[1] == ':' && ((*path)[2] == '\\' || (*path)[2] == '/');
    if (!posix && !drive) return err(status::invalid_arg);
    c.roots.push_back({*id, *label, *path});
  }
  if (const std::string* relay = doc->str("relay")) {
    if (relay->size() > 300) return err(status::invalid_arg);
    c.relay = *relay;
  }
  return c;
}

bool parse_relay_address(std::string_view address, std::string& host, std::uint16_t& port) {
  if (address.empty() || address.size() > 255) return false;  // the phone's limit for an address too
  for (const char ch : address) {
    if (static_cast<unsigned char>(ch) <= 0x20 || ch == 0x7f) return false;
  }
  const auto colon = address.find_last_of(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 >= address.size() || address.size() - colon - 1 > 5) return false;
  std::string_view h = address.substr(0, colon);
  if (h.front() == '[') {
    if (h.size() < 3 || h.back() != ']') return false;
    h = h.substr(1, h.size() - 2);
  } else if (h.find(':') != std::string_view::npos) {
    return false;  // an IPv6 address needs its brackets
  }
  unsigned long p = 0;
  for (const char ch : address.substr(colon + 1)) {
    if (ch < '0' || ch > '9') return false;
    p = p * 10 + static_cast<unsigned long>(ch - '0');
  }
  if (p == 0 || p > 65535) return false;
  host.assign(h);
  port = static_cast<std::uint16_t>(p);
  return true;
}

std::array<std::uint8_t, 16> relay_token(const std::array<std::uint8_t, 32>& access_key,
                                         const std::array<std::uint8_t, 32>& device_public) {
  static const unsigned char label[] = "MediaViewer relay token v1";
  std::array<unsigned char, sizeof label - 1 + 32> m{};
  std::memcpy(m.data(), label, sizeof label - 1);
  std::memcpy(m.data() + sizeof label - 1, device_public.data(), 32);
  std::array<std::uint8_t, 16> out{};
  crypto_generichash(out.data(), out.size(), m.data(), m.size(), access_key.data(), access_key.size());
  return out;
}

}  // namespace mv::driftwire::serve
