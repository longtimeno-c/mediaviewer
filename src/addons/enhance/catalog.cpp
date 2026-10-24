// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/catalog.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace mv::enhance {
namespace {

namespace fs = std::filesystem;

fs::path fs_path(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

// ai-models.py's ALLOWED_LICENCES: one allow-list for both add-ons.
constexpr std::array<std::string_view, 8> kLicences{"MIT", "Apache-2.0", "BSD-2-Clause", "BSD-3-Clause",
                                                    "Zlib", "ISC", "CC0-1.0", "GPL-3.0-or-later"};

// enhance-models.py's is_int: a whole number a double holds exactly.
constexpr std::int64_t kExact = std::int64_t{1} << 53;

bool is_int(const json::value& v) noexcept {
  return v.k == json::kind::number && v.is_integer && v.i > -kExact && v.i < kExact;
}

bool is_number(const json::value& v) noexcept {
  return is_int(v) || (v.k == json::kind::number && !v.is_integer && std::isfinite(v.d));
}

double number_of(const json::value& v) noexcept { return v.is_integer ? static_cast<double>(v.i) : v.d; }

// Python's len(): code points, not bytes (the parser has checked the UTF-8).
std::size_t code_points(std::string_view s) noexcept {
  return static_cast<std::size_t>(
      std::count_if(s.begin(), s.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

const std::string* text_of(const json::value& o, std::string_view key, std::size_t most = 200) noexcept {
  const std::string* s = o.str(key);
  if (!s || s->empty() || code_points(*s) > most) return nullptr;
  return s;
}

bool allowed_licence(const std::string* s) noexcept {
  return s && std::find(kLicences.begin(), kLicences.end(), *s) != kLicences.end();
}

bool only_keys(const json::value& o, std::initializer_list<std::string_view> a,
               std::initializer_list<std::string_view> b = {}) noexcept {
  return std::all_of(o.o.begin(), o.o.end(), [&](const auto& member) {
    return std::find(a.begin(), a.end(), member.first) != a.end() ||
           std::find(b.begin(), b.end(), member.first) != b.end();
  });
}

// One or more of still, video, audio, each once.
std::optional<std::vector<kind>> kind_list(const json::value* v) {
  if (!v || v->k != json::kind::array || v->a.empty()) return std::nullopt;
  std::vector<kind> out;
  for (const json::value& e : v->a) {
    const auto k = e.k == json::kind::string ? kind_from(e.s) : std::nullopt;
    if (!k || std::find(out.begin(), out.end(), *k) != out.end()) return std::nullopt;
    out.push_back(*k);
  }
  return out;
}

// A model folder's id: [a-z0-9-]{1,32} (it names a folder and node ids).
bool model_id_ok(std::string_view id) noexcept {
  return !id.empty() && id.size() <= 32 && std::all_of(id.begin(), id.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
  });
}

bool knob_id_ok(std::string_view id) noexcept {
  return !id.empty() && id.size() <= 32 && std::all_of(id.begin(), id.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
  });
}

bool hex64(std::string_view s) noexcept {
  return s.size() == 64 &&
         std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// A relative, '/'-separated path with no "..", root, drive or control character.
bool safe_relative(std::string_view p) noexcept {
  if (p.empty() || p.size() > 512 || p.front() == '/' || p.find('\\') != std::string_view::npos ||
      p.find(':') != std::string_view::npos) {
    return false;
  }
  if (std::any_of(p.begin(), p.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; })) return false;
  std::size_t at = 0;
  for (;;) {
    const std::size_t slash = p.find('/', at);
    const std::string_view part = p.substr(at, slash == std::string_view::npos ? std::string_view::npos : slash - at);
    if (part.empty() || part == "." || part == "..") return false;
    if (slash == std::string_view::npos) return true;
    at = slash + 1;
  }
}

// src/addon/manifest.cpp remote_url_ok, rule for rule: the host checked the
// signed manifest at load, but this reads it again at the page's first open.
// A pinned Hugging Face file, never a query, a fragment or a user.
bool remote_url_ok(std::string_view url) noexcept {
  constexpr std::string_view kHost = "https://huggingface.co/";
  if (url.size() <= kHost.size() || url.size() > 1024 || url.substr(0, kHost.size()) != kHost) return false;
  for (const char c : url) {
    if (c == '?' || c == '#' || c == '@' || c == '\\' || static_cast<unsigned char>(c) <= 0x20 ||
        static_cast<unsigned char>(c) >= 0x7F) {
      return false;
    }
  }
  return url.find("/../") == std::string_view::npos && url.find("/resolve/") != std::string_view::npos;
}

// enhance-models.py knob_problems: a knob that breaks a rule leaves the model out.
std::optional<knob> read_knob(const json::value& v, const std::vector<kind>& model_kinds,
                              std::set<std::string>& seen) {
  if (v.k != json::kind::object) return std::nullopt;
  knob out;
  const std::string* id = v.str("id");
  if (!id || !knob_id_ok(*id) || !seen.insert(*id).second) return std::nullopt;
  out.id = *id;
  const std::string* label = text_of(v, "label");
  const std::string* type = v.str("type");
  if (!label || !type) return std::nullopt;
  out.label = *label;
  out.type = *type;
  if (out.type == "slider") {
    if (!only_keys(v, {"id", "label", "type", "kinds", "default"}, {"min", "max", "step"})) return std::nullopt;
  } else if (out.type == "choice") {
    if (!only_keys(v, {"id", "label", "type", "kinds", "default"}, {"options"})) return std::nullopt;
  } else if (out.type == "toggle") {
    if (!only_keys(v, {"id", "label", "type", "kinds", "default"})) return std::nullopt;
  } else {
    return std::nullopt;
  }
  auto kinds = kind_list(v.find("kinds"));
  if (!kinds || !std::all_of(kinds->begin(), kinds->end(), [&](kind k) {
        return std::find(model_kinds.begin(), model_kinds.end(), k) != model_kinds.end();
      })) {
    return std::nullopt;
  }
  out.kinds = std::move(*kinds);
  const json::value* def = v.find("default");
  if (!def) return std::nullopt;
  if (out.type == "slider") {
    const json::value* lo = v.find("min");
    const json::value* hi = v.find("max");
    const json::value* step = v.find("step");
    if (!lo || !hi || !step || !is_number(*lo) || !is_number(*hi) || !is_number(*step) || !is_number(*def)) {
      return std::nullopt;
    }
    out.min = number_of(*lo);
    out.max = number_of(*hi);
    out.step = number_of(*step);
    const double d = number_of(*def);
    if (!(out.min < out.max) || out.step <= 0 || out.step > out.max - out.min || d < out.min || d > out.max) {
      return std::nullopt;
    }
  } else if (out.type == "choice") {
    const json::value* options = v.find("options");
    if (!options || options->k != json::kind::array || options->a.empty() || options->a.size() > 256) {
      return std::nullopt;
    }
    for (const json::value& o : options->a) {
      if (o.k != json::kind::object || o.o.size() != 2) return std::nullopt;
      const std::string* oid = text_of(o, "id", 64);
      const std::string* text = text_of(o, "text");
      if (!oid || !text) return std::nullopt;
      if (std::any_of(out.options.begin(), out.options.end(), [&](const option& e) { return e.id == *oid; })) {
        return std::nullopt;
      }
      out.options.push_back(option{*oid, *text});
    }
    if (def->k != json::kind::string ||
        std::none_of(out.options.begin(), out.options.end(), [&](const option& e) { return e.id == def->s; })) {
      return std::nullopt;
    }
  } else if (def->k != json::kind::boolean) {
    return std::nullopt;
  }
  out.default_value = *def;
  return out;
}

// enhance-models.py model_problems, for models/<id>/model.json.
std::optional<model> read_model(std::string_view id, std::string_view text) {
  const auto doc = json::parse(text);
  if (!doc || doc->k != json::kind::object) return std::nullopt;
  const json::value& v = *doc;
  if (!only_keys(v, {"id", "name", "model", "kinds", "run_bytes", "cache_bytes", "accelerators", "cpu_unit_ms",
                     "licence", "knobs"})) {
    return std::nullopt;
  }
  model m;
  const std::string* own = v.str("id");
  if (!own || *own != id) return std::nullopt;
  m.id = *own;
  const std::string* name = text_of(v, "name");
  const std::string* what = text_of(v, "model");
  if (!name || !what) return std::nullopt;
  m.name = *name;
  m.model_name = *what;
  auto kinds = kind_list(v.find("kinds"));
  if (!kinds) return std::nullopt;
  m.kinds = std::move(*kinds);
  const json::value* run = v.find("run_bytes");
  const json::value* cache = v.find("cache_bytes");
  if (!run || !is_int(*run) || run->i <= 0 || !cache || !is_int(*cache) || cache->i < 0) return std::nullopt;
  m.run_bytes = static_cast<std::uint64_t>(run->i);
  m.cache_bytes = static_cast<std::uint64_t>(cache->i);
  const json::value* acc = v.find("accelerators");
  if (!acc || acc->k != json::kind::array) return std::nullopt;
  for (const json::value& a : acc->a) {
    if (a.k != json::kind::string) return std::nullopt;
    bool& slot = a.s == "cuda" ? m.cuda : m.coreml;
    if ((a.s != "cuda" && a.s != "coreml") || slot) return std::nullopt;
    slot = true;
  }
  if (const json::value* cpu = v.find("cpu_unit_ms")) {
    if (!is_int(*cpu) || cpu->i <= 0) return std::nullopt;
    m.cpu_unit_ms = static_cast<std::uint64_t>(cpu->i);
  }
  if (!m.cuda && !m.coreml && !m.cpu_unit_ms) return std::nullopt;  // it runs nowhere
  const std::string* licence = v.str("licence");
  if (!allowed_licence(licence)) return std::nullopt;
  m.licence = *licence;
  if (const json::value* knobs = v.find("knobs")) {
    if (knobs->k != json::kind::array) return std::nullopt;
    std::set<std::string> seen;
    for (const json::value& k : knobs->a) {
      auto one = read_knob(k, m.kinds, seen);
      if (!one) return std::nullopt;
      m.knobs.push_back(std::move(*one));
    }
  }
  return m;
}

std::optional<std::string> read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream text;
  text << in.rdbuf();
  if (in.bad()) return std::nullopt;
  return text.str();
}

std::string join(const std::string& dir, std::string_view rel) {
  if (dir.empty()) return std::string(rel);
  const char last = dir.back();
  return (last == '/' || last == '\\') ? dir + std::string(rel) : dir + "/" + std::string(rel);
}

}  // namespace

const char* kind_id(kind k) noexcept {
  switch (k) {
    case kind::still: return "still";
    case kind::video: return "video";
    case kind::audio: return "audio";
  }
  return "still";
}

std::optional<kind> kind_from(std::string_view id) noexcept {
  for (kind k : kKinds) {
    if (id == kind_id(k)) return k;
  }
  return std::nullopt;
}

std::uint64_t model::download_bytes() const noexcept {
  std::uint64_t n = 0;
  for (const pinned& f : files) n += f.size;
  return n;
}

bool model::serves(kind k) const noexcept { return std::find(kinds.begin(), kinds.end(), k) != kinds.end(); }

const model* catalog::find(std::string_view id) const noexcept {
  for (const model& m : models) {
    if (m.id == id) return &m;
  }
  return nullptr;
}

std::vector<pinned> read_remote(std::string_view manifest_json) {
  std::vector<pinned> out;
  const auto doc = json::parse(manifest_json);
  const json::value* remote = doc ? doc->find("remote") : nullptr;
  if (!remote || remote->k != json::kind::array) return out;
  for (const json::value& r : remote->a) {
    const std::string* url = r.str("url");
    const std::string* path = r.str("path");
    const std::string* sha = r.str("sha256");
    const std::string* licence = r.str("licence");
    const auto size = r.integer("size");
    if (!url || !remote_url_ok(*url) || !path || !safe_relative(*path) || !sha || !hex64(*sha) || !licence ||
        licence->empty() || !size || *size <= 0) {
      continue;
    }
    out.push_back(pinned{*url, *path, *sha, static_cast<std::uint64_t>(*size), *licence});
  }
  return out;
}

result<catalog> parse_catalog(std::string_view index_json, const model_source& model_json,
                              std::span<const pinned> remote) {
  const auto doc = json::parse(index_json);
  if (!doc || doc->k != json::kind::object) return err(status::corrupt);
  const auto schema = doc->integer("schema");
  const json::value* kinds = doc->find("kinds");
  if (!schema || *schema != 1 || !kinds || kinds->k != json::kind::object) return err(status::corrupt);

  catalog c;
  std::vector<std::string> named;  // every id a kind lists, in first-mention order
  for (kind_entry& e : c.kinds) {
    const json::value* spec = kinds->find(kind_id(e.k));
    if (!spec || spec->k != json::kind::object) continue;
    if (const std::string* name = text_of(*spec, "name")) e.name = *name;
    if (const std::string* unit = text_of(*spec, "unit")) e.unit = *unit;
    if (const json::value* budget = spec->find("cpu_budget_ms"); budget && is_int(*budget) && budget->i > 0) {
      e.cpu_budget_ms = static_cast<std::uint64_t>(budget->i);
    }
    const json::value* list = spec->find("models");
    if (!list || list->k != json::kind::array) continue;
    for (const json::value& id : list->a) {
      if (id.k != json::kind::string || !model_id_ok(id.s)) continue;
      e.models.push_back(id.s);
      if (std::find(named.begin(), named.end(), id.s) == named.end()) named.push_back(id.s);
    }
  }

  for (const std::string& id : named) {
    const auto text = model_json ? model_json(id) : std::nullopt;
    if (!text) continue;
    auto m = read_model(id, *text);
    if (!m) continue;
    const std::string folder = "models/" + id + "/";
    for (const pinned& f : remote) {
      if (f.path.size() > folder.size() && f.path.compare(0, folder.size(), folder) == 0) m->files.push_back(f);
    }
    if (m->files.empty()) continue;  // nothing to fetch: not offered
    c.models.push_back(std::move(*m));
  }

  // A kind keeps the ids that name a model serving it, each once.
  for (kind_entry& e : c.kinds) {
    std::vector<std::string> kept;
    for (const std::string& id : e.models) {
      const model* m = c.find(id);
      if (m && m->serves(e.k) && std::find(kept.begin(), kept.end(), id) == kept.end()) kept.push_back(id);
    }
    e.models = std::move(kept);
  }
  return c;
}

result<catalog> read_catalog(const std::string& version_dir) {
  const fs::path index = fs_path(join(version_dir, "models/enhance.json"));
  std::error_code ec;
  if (!fs::exists(index, ec)) return catalog{};
  const auto text = read_text(index);
  if (!text) return err(status::io);
  const auto manifest = read_text(fs_path(join(version_dir, "manifest.json")));
  const std::vector<pinned> remote = manifest ? read_remote(*manifest) : std::vector<pinned>{};
  return parse_catalog(
      *text,
      [&version_dir](std::string_view id) {
        return read_text(fs_path(join(version_dir, "models/" + std::string(id) + "/model.json")));
      },
      remote);
}

}  // namespace mv::enhance
