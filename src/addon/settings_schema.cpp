// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/settings_schema.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

namespace mv::addon {
namespace {

bool fail(std::string* detail, std::string_view what) {
  if (detail) detail->assign(what);
  return false;
}

bool key_text(std::string_view k) noexcept {
  if (k.empty() || k.size() > 40 || k.front() < 'a' || k.front() > 'z') return false;
  return std::all_of(k.begin(), k.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
  });
}

bool option_text(std::string_view v) noexcept {
  if (v.empty() || v.size() > 40) return false;
  return std::all_of(v.begin(), v.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
  });
}

// A value a person types: any text but control characters, bounded.
bool free_text(std::string_view s, std::size_t max_bytes) noexcept {
  if (s.size() > max_bytes || !json::detail::valid_utf8(s)) return false;
  return std::none_of(s.begin(), s.end(), [](char ch) {
    const auto c = static_cast<unsigned char>(ch);
    return c < 0x20 || c == 0x7F;
  });
}

bool number_of(const json::value& v, double& out) noexcept {
  if (v.k != json::kind::number) return false;
  out = v.is_integer ? static_cast<double>(v.i) : v.d;
  return std::isfinite(out) && std::fabs(out) <= kSettingsNumberLimit;
}

template <std::size_t N>
bool only_keys(const json::value& v, const std::array<std::string_view, N>& keys) {
  for (const auto& [name, value] : v.o) {
    (void)value;
    if (std::find(keys.begin(), keys.end(), name) == keys.end()) return false;
  }
  return true;
}

bool text_field(const json::value& from, std::string_view key, std::string& out,
                std::size_t max_bytes, bool required) {
  const json::value* v = from.find(key);
  if (!v) return !required;
  if (v->k != json::kind::string || !valid_display_text(v->s, max_bytes, !required)) return false;
  out = v->s;
  return true;
}

bool parse_item(const json::value& v, setting_item& it, const std::set<std::string>& toggles_before,
                std::string* detail) {
  if (v.k != json::kind::object) return fail(detail, "item");
  const std::string* type = v.str("type");
  const std::string* key = v.str("key");
  if (!type) return fail(detail, "type");
  if (!key || !key_text(*key)) return fail(detail, "key");
  it.key = *key;
  if (!text_field(v, "label", it.label, 64, true)) return fail(detail, it.key + ": label");
  if (!text_field(v, "help", it.help, 280, false)) return fail(detail, it.key + ": help");
  if (const json::value* by = v.find("enabled_by")) {
    if (by->k != json::kind::string || toggles_before.count(by->s) == 0) {
      return fail(detail, it.key + ": enabled_by");
    }
    it.enabled_by = by->s;
  }
  const json::value* def = v.find("default");
  if (!def) return fail(detail, it.key + ": default");

  if (*type == "toggle") {
    static constexpr std::array<std::string_view, 6> kKeys{"type", "key", "label", "help",
                                                           "enabled_by", "default"};
    if (!only_keys(v, kKeys)) return fail(detail, it.key + ": unknown key");
    it.kind = setting_kind::toggle;
    if (def->k != json::kind::boolean) return fail(detail, it.key + ": default");
    it.flag = def->b;
    return true;
  }
  if (*type == "number") {
    static constexpr std::array<std::string_view, 10> kKeys{
        "type", "key", "label", "help", "enabled_by", "default", "min", "max", "step", "unit"};
    if (!only_keys(v, kKeys)) return fail(detail, it.key + ": unknown key");
    it.kind = setting_kind::number;
    const json::value* lo = v.find("min");
    const json::value* hi = v.find("max");
    if (!lo || !hi || !number_of(*lo, it.min) || !number_of(*hi, it.max) || !(it.min < it.max)) {
      return fail(detail, it.key + ": range");
    }
    if (const json::value* st = v.find("step")) {
      // Three decimals is what the hosts and the store write.
      if (!number_of(*st, it.step) || it.step < 0.001 || it.step > it.max - it.min) {
        return fail(detail, it.key + ": step");
      }
    }
    if (!text_field(v, "unit", it.unit, 16, false)) return fail(detail, it.key + ": unit");
    if (!number_of(*def, it.number) || it.number < it.min || it.number > it.max) {
      return fail(detail, it.key + ": default");
    }
    return true;
  }
  if (*type == "choice") {
    static constexpr std::array<std::string_view, 7> kKeys{"type", "key", "label", "help",
                                                           "enabled_by", "default", "options"};
    if (!only_keys(v, kKeys)) return fail(detail, it.key + ": unknown key");
    it.kind = setting_kind::choice;
    const json::value* opts = v.find("options");
    if (!opts || opts->k != json::kind::array || opts->a.size() < 2 ||
        opts->a.size() > kSettingsMaxOptions) {
      return fail(detail, it.key + ": options");
    }
    std::set<std::string> seen;
    for (const json::value& o : opts->a) {
      static constexpr std::array<std::string_view, 2> kOpt{"value", "label"};
      if (o.k != json::kind::object || !only_keys(o, kOpt)) return fail(detail, it.key + ": option");
      const std::string* value = o.str("value");
      setting_option opt;
      if (!value || !option_text(*value) || !seen.insert(*value).second ||
          !text_field(o, "label", opt.label, 64, true)) {
        return fail(detail, it.key + ": option");
      }
      opt.value = *value;
      it.options.push_back(std::move(opt));
    }
    if (def->k != json::kind::string || seen.count(def->s) == 0) {
      return fail(detail, it.key + ": default");
    }
    it.text = def->s;
    return true;
  }
  if (*type == "text") {
    static constexpr std::array<std::string_view, 8> kKeys{
        "type", "key", "label", "help", "enabled_by", "default", "max_length", "placeholder"};
    if (!only_keys(v, kKeys)) return fail(detail, it.key + ": unknown key");
    it.kind = setting_kind::text;
    if (const json::value* ml = v.find("max_length")) {
      if (ml->k != json::kind::number || !ml->is_integer || ml->i < 1 ||
          ml->i > static_cast<std::int64_t>(kSettingsMaxText)) {
        return fail(detail, it.key + ": max_length");
      }
      it.max_length = static_cast<std::uint32_t>(ml->i);
    }
    if (!text_field(v, "placeholder", it.placeholder, 64, false)) {
      return fail(detail, it.key + ": placeholder");
    }
    if (def->k != json::kind::string || !free_text(def->s, it.max_length)) {
      return fail(detail, it.key + ": default");
    }
    it.text = def->s;
    return true;
  }
  return fail(detail, it.key + ": type");
}

}  // namespace

bool valid_display_text(std::string_view s, std::size_t max_bytes, bool may_be_empty) noexcept {
  if (s.empty()) return may_be_empty;
  if (s.size() > max_bytes || s.front() == ' ' || s.back() == ' ') return false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x20 || c == 0x7F) return false;
    if (c == 0xE2 && i + 2 < s.size()) {
      const auto c1 = static_cast<unsigned char>(s[i + 1]);
      const auto c2 = static_cast<unsigned char>(s[i + 2]);
      // U+200B..U+200F, U+2028..U+202E, U+2060..U+206F.
      if (c1 == 0x80 && ((c2 >= 0x8B && c2 <= 0x8F) || (c2 >= 0xA8 && c2 <= 0xAE))) return false;
      if (c1 == 0x81 && c2 >= 0xA0 && c2 <= 0xAF) return false;
    }
    // U+FEFF, a zero-width no-break space.
    if (c == 0xEF && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xBB &&
        static_cast<unsigned char>(s[i + 2]) == 0xBF) {
      return false;
    }
  }
  return true;
}

const char* setting_kind_name(setting_kind k) noexcept {
  switch (k) {
    case setting_kind::toggle: return "toggle";
    case setting_kind::choice: return "choice";
    case setting_kind::number: return "number";
    case setting_kind::text: return "text";
  }
  return "toggle";
}

const setting_item* settings_page::find(std::string_view key) const noexcept {
  for (const setting_section& s : sections) {
    for (const setting_item& it : s.items) {
      if (it.key == key) return &it;
    }
  }
  return nullptr;
}

std::optional<settings_page> parse_settings_page(const json::value& v, std::string* detail) {
  if (v.k != json::kind::object) {
    fail(detail, "settings");
    return std::nullopt;
  }
  static constexpr std::array<std::string_view, 2> kKeys{"title", "sections"};
  if (!only_keys(v, kKeys)) {
    fail(detail, "settings: unknown key");
    return std::nullopt;
  }
  settings_page page;
  if (!text_field(v, "title", page.title, 64, false)) {
    fail(detail, "title");
    return std::nullopt;
  }
  const json::value* sections = v.find("sections");
  if (!sections || sections->k != json::kind::array || sections->a.empty() ||
      sections->a.size() > kSettingsMaxSections) {
    fail(detail, "sections");
    return std::nullopt;
  }
  std::set<std::string> keys;
  std::set<std::string> toggles;
  std::size_t items = 0;
  for (const json::value& s : sections->a) {
    static constexpr std::array<std::string_view, 3> kSection{"title", "help", "items"};
    setting_section section;
    if (s.k != json::kind::object || !only_keys(s, kSection) ||
        !text_field(s, "title", section.title, 64, true) ||
        !text_field(s, "help", section.help, 280, false)) {
      fail(detail, "section");
      return std::nullopt;
    }
    const json::value* list = s.find("items");
    if (!list || list->k != json::kind::array || list->a.empty()) {
      fail(detail, section.title + ": items");
      return std::nullopt;
    }
    for (const json::value& raw : list->a) {
      if (++items > kSettingsMaxItems) {
        fail(detail, "too many items");
        return std::nullopt;
      }
      setting_item it;
      if (!parse_item(raw, it, toggles, detail)) return std::nullopt;
      if (!keys.insert(it.key).second) {
        fail(detail, it.key + ": duplicate");
        return std::nullopt;
      }
      if (it.kind == setting_kind::toggle) toggles.insert(it.key);
      section.items.push_back(std::move(it));
    }
    page.sections.push_back(std::move(section));
  }
  return page;
}

std::optional<settings_page> parse_settings_page_text(std::string_view text, std::string* detail) {
  if (text.empty() || text.size() > (256u << 10)) return std::nullopt;
  const auto doc = json::parse(text, 8);
  if (!doc) {
    fail(detail, "json");
    return std::nullopt;
  }
  return parse_settings_page(*doc, detail);
}

bool setting_value_fits(const setting_item& item, const json::value& value) noexcept {
  switch (item.kind) {
    case setting_kind::toggle:
      return value.k == json::kind::boolean;
    case setting_kind::number: {
      double d = 0;
      return number_of(value, d) && d >= item.min && d <= item.max;
    }
    case setting_kind::choice:
      return value.k == json::kind::string &&
             std::any_of(item.options.begin(), item.options.end(),
                         [&](const setting_option& o) { return o.value == value.s; });
    case setting_kind::text:
      return value.k == json::kind::string && free_text(value.s, item.max_length);
  }
  return false;
}

std::string setting_value_json(const json::value& value) {
  json::writer w;
  switch (value.k) {
    case json::kind::boolean: w.boolean(value.b); break;
    case json::kind::string: w.string(value.s); break;
    case json::kind::number: {
      const double d = value.is_integer ? static_cast<double>(value.i) : value.d;
      if (value.is_integer) {
        w.integer(value.i);
      } else if (std::trunc(d) == d && std::fabs(d) <= kSettingsNumberLimit) {
        w.integer(static_cast<std::int64_t>(d));
      } else {
        w.number(d);
      }
      break;
    }
    default: w.null(); break;
  }
  return w.take();
}

std::string setting_default_json(const setting_item& item) {
  json::value v;
  switch (item.kind) {
    case setting_kind::toggle:
      v.k = json::kind::boolean;
      v.b = item.flag;
      break;
    case setting_kind::number:
      v.k = json::kind::number;
      v.d = item.number;
      if (std::trunc(item.number) == item.number) {
        v.is_integer = true;
        v.i = static_cast<std::int64_t>(item.number);
      }
      break;
    case setting_kind::choice:
    case setting_kind::text:
      v.k = json::kind::string;
      v.s = item.text;
      break;
  }
  return setting_value_json(v);
}

}  // namespace mv::addon
