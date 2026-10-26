// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/spec.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace mv::enhance {
namespace {

bool id_text(std::string_view s) noexcept {
  if (s.empty() || s.size() > 32) return false;
  return std::all_of(s.begin(), s.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

// Shown text: non-empty, no control characters, bounded (the view tree's own
// label cap is 200 bytes).
bool label_text(const std::string* s, std::size_t max = 120) noexcept {
  if (!s || s->empty() || s->size() > max) return false;
  return std::none_of(s->begin(), s->end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; });
}

std::optional<double> num(const json::value* v) noexcept {
  if (!v || v->k != json::kind::number) return std::nullopt;
  const double d = v->is_integer ? static_cast<double>(v->i) : v->d;
  if (!std::isfinite(d) || std::fabs(d) > 1e9) return std::nullopt;
  return d;
}

// A weights path under its kind's folder: one plain file name, no folders.
bool weights_name(std::string_view s) noexcept {
  if (s.empty() || s.size() > 128 || s == "." || s == ".." || s == "model.json") return false;
  return std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
           c == '.';
  });
}

std::optional<knob> read_knob(const json::value& v, model_kind own) {
  if (v.k != json::kind::object) return std::nullopt;
  knob k;
  const std::string* id = v.str("id");
  const std::string* label = v.str("label");
  const std::string* type = v.str("type");
  if (!id || !id_text(*id) || !label_text(label) || !type) return std::nullopt;
  k.id = *id;
  k.label = *label;
  if (const json::value* help = v.find("help")) {
    if (!label_text(help->k == json::kind::string ? &help->s : nullptr, 200)) return std::nullopt;
    k.help = help->s;
  }
  if (*type == "slider") {
    k.t = knob::type::slider;
    const auto lo = num(v.find("min"));
    const auto hi = num(v.find("max"));
    const auto def = num(v.find("default"));
    if (!lo || !hi || !def || *lo >= *hi || *def < *lo || *def > *hi) return std::nullopt;
    k.min = *lo;
    k.max = *hi;
    k.def = *def;
    if (const json::value* step = v.find("step")) {
      const auto s = num(step);
      if (!s || *s <= 0 || *s > *hi - *lo) return std::nullopt;
      k.step = *s;
    }
  } else if (*type == "choice") {
    k.t = knob::type::choice;
    const json::value* options = v.find("options");
    const std::string* def = v.str("default");
    if (!options || options->k != json::kind::array || options->a.empty() || options->a.size() > 16 || !def) {
      return std::nullopt;
    }
    std::set<std::string> seen;
    for (const json::value& o : options->a) {
      const std::string* oid = o.str("id");
      const std::string* text = o.str("text");
      if (!oid || oid->empty() || oid->size() > 32 || !label_text(text, 64) || !seen.insert(*oid).second) {
        return std::nullopt;
      }
      k.options.push_back({*oid, *text});
    }
    if (!seen.count(*def)) return std::nullopt;
    k.def_choice = *def;
  } else if (*type == "toggle") {
    k.t = knob::type::toggle;
    const auto def = v.boolean("default");
    if (!def) return std::nullopt;
    k.def_on = *def;
  } else {
    return std::nullopt;
  }
  if (const json::value* kinds = v.find("kinds")) {
    if (kinds->k != json::kind::array || kinds->a.empty()) return std::nullopt;
    for (const json::value& n : kinds->a) {
      const auto mk = n.k == json::kind::string ? kind_from(n.s) : std::nullopt;
      if (!mk) return std::nullopt;
      if (std::find(k.kinds.begin(), k.kinds.end(), *mk) == k.kinds.end()) k.kinds.push_back(*mk);
    }
  } else {
    k.kinds.push_back(own);
  }
  return k;
}

}  // namespace

const char* kind_name(model_kind k) noexcept {
  switch (k) {
    case model_kind::still: return "still";
    case model_kind::video: return "video";
    case model_kind::audio: return "audio";
  }
  return "still";
}

std::optional<model_kind> kind_from(std::string_view name) noexcept {
  for (const model_kind k : kModelKinds) {
    if (name == kind_name(k)) return k;
  }
  return std::nullopt;
}

const knob* model_spec::find(std::string_view id) const noexcept {
  for (const knob& k : knobs) {
    if (k.id == id) return &k;
  }
  return nullptr;
}

std::optional<model_spec> parse_spec(std::string_view json_text, model_kind expected) {
  const auto doc = json::parse(json_text, 16);
  if (!doc || doc->k != json::kind::object) return std::nullopt;
  model_spec s;
  const std::string* kind = doc->str("kind");
  if (!kind || kind_from(*kind) != expected) return std::nullopt;
  s.kind = expected;
  const std::string* name = doc->str("name");
  if (!label_text(name, 64)) return std::nullopt;
  s.name = *name;
  if (const json::value* credit = doc->find("credit")) {
    if (!label_text(credit->k == json::kind::string ? &credit->s : nullptr, 200)) return std::nullopt;
    s.credit = credit->s;
  }
  const json::value* weights = doc->find("weights");
  if (!weights || weights->k != json::kind::array || weights->a.empty() || weights->a.size() > 8) {
    return std::nullopt;
  }
  for (const json::value& w : weights->a) {
    if (w.k != json::kind::string || !weights_name(w.s)) return std::nullopt;
    if (std::find(s.weights.begin(), s.weights.end(), w.s) != s.weights.end()) return std::nullopt;
    s.weights.push_back(w.s);
  }
  const auto run_bytes = doc->integer("run_bytes");
  if (!run_bytes || *run_bytes <= 0) return std::nullopt;
  s.run_bytes = static_cast<std::uint64_t>(*run_bytes);
  s.cpu = doc->boolean("cpu").value_or(false);
  if (const json::value* knobs = doc->find("knobs")) {
    if (knobs->k != json::kind::array || knobs->a.size() > 16) return std::nullopt;
    std::set<std::string> seen;
    for (const json::value& kv : knobs->a) {
      auto k = read_knob(kv, expected);
      if (!k || !seen.insert(k->id).second) return std::nullopt;
      s.knobs.push_back(std::move(*k));
    }
  }
  if (const json::value* io = doc->find("io")) {
    if (io->k != json::kind::object) return std::nullopt;
    s.io = *io;
  }
  return s;
}

json::value knob_value(const knob& k, const json::value* stored) {
  if (stored) {
    if (auto ok = check_knob_value(k, *stored)) return *ok;
  }
  json::value v;
  switch (k.t) {
    case knob::type::slider:
      v.k = json::kind::number;
      v.d = k.def;
      break;
    case knob::type::choice:
      v.k = json::kind::string;
      v.s = k.def_choice;
      break;
    case knob::type::toggle:
      v.k = json::kind::boolean;
      v.b = k.def_on;
      break;
  }
  return v;
}

void write_scalar(json::writer& w, const json::value& v) {
  switch (v.k) {
    case json::kind::number:
      if (v.is_integer) {
        w.integer(v.i);
      } else {
        w.number(v.d);
      }
      break;
    case json::kind::string: w.string(v.s); break;
    case json::kind::boolean: w.boolean(v.b); break;
    default: w.null(); break;
  }
}

std::optional<json::value> check_knob_value(const knob& k, const json::value& v) {
  switch (k.t) {
    case knob::type::slider: {
      const auto d = num(&v);
      if (!d || *d < k.min || *d > k.max) return std::nullopt;
      json::value out;
      out.k = json::kind::number;
      out.d = *d;
      return out;
    }
    case knob::type::choice:
      if (v.k != json::kind::string ||
          std::none_of(k.options.begin(), k.options.end(), [&](const knob_option& o) { return o.id == v.s; })) {
        return std::nullopt;
      }
      return v;
    case knob::type::toggle:
      if (v.k != json::kind::boolean) return std::nullopt;
      return v;
  }
  return std::nullopt;
}

}  // namespace mv::enhance
