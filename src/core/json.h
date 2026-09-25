// SPDX-License-Identifier: GPL-2.0-or-later
// A small strict JSON reader and writer (plan/18: add-on manifests, Import
// presets, reports). Header-only, so an add-on can carry it without linking
// the core (plan/18 "does not link the core statically").
//
// Strict on purpose, because it reads signed manifests: RFC 8259 grammar only,
// UTF-8 in, no comments, no trailing commas, a nesting limit, and a duplicate
// key in one object is a parse failure (two parsers must never disagree about
// which "sha256" a manifest meant). Numbers are kept as their integer value
// when they are integers; anything fractional is also available as a double.
#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mv::json {

enum class kind : std::uint8_t { null, boolean, number, string, array, object };

struct value {
  kind k = kind::null;
  bool b = false;
  bool is_integer = false;
  std::int64_t i = 0;
  double d = 0.0;
  std::string s;
  std::vector<value> a;
  std::vector<std::pair<std::string, value>> o;

  [[nodiscard]] const value* find(std::string_view key) const noexcept {
    if (k != kind::object) return nullptr;
    for (const auto& [name, v] : o) {
      if (name == key) return &v;
    }
    return nullptr;
  }
  [[nodiscard]] const std::string* str(std::string_view key) const noexcept {
    const value* v = find(key);
    return v && v->k == kind::string ? &v->s : nullptr;
  }
  [[nodiscard]] std::optional<std::int64_t> integer(std::string_view key) const noexcept {
    const value* v = find(key);
    if (!v || v->k != kind::number || !v->is_integer) return std::nullopt;
    return v->i;
  }
  [[nodiscard]] std::optional<bool> boolean(std::string_view key) const noexcept {
    const value* v = find(key);
    if (!v || v->k != kind::boolean) return std::nullopt;
    return v->b;
  }
};

namespace detail {

class parser {
 public:
  explicit parser(std::string_view text, int max_depth) : t_(text), max_depth_(max_depth) {}

  bool parse_document(value& out) {
    ws();
    if (!parse_value(out, 0)) return false;
    ws();
    return p_ == t_.size();
  }

 private:
  std::string_view t_;
  std::size_t p_ = 0;
  int max_depth_;

  void ws() noexcept {
    while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\n' || t_[p_] == '\r')) ++p_;
  }
  bool lit(std::string_view word) noexcept {
    if (t_.substr(p_, word.size()) != word) return false;
    p_ += word.size();
    return true;
  }

  bool parse_value(value& v, int depth) {
    if (depth > max_depth_ || p_ >= t_.size()) return false;
    const char c = t_[p_];
    if (c == '{') return parse_object(v, depth + 1);
    if (c == '[') return parse_array(v, depth + 1);
    if (c == '"') {
      v.k = kind::string;
      return parse_string(v.s);
    }
    if (c == 't' && lit("true")) {
      v.k = kind::boolean;
      v.b = true;
      return true;
    }
    if (c == 'f' && lit("false")) {
      v.k = kind::boolean;
      v.b = false;
      return true;
    }
    if (c == 'n' && lit("null")) {
      v.k = kind::null;
      return true;
    }
    return parse_number(v);
  }

  bool parse_object(value& v, int depth) {
    v.k = kind::object;
    ++p_;
    ws();
    if (p_ < t_.size() && t_[p_] == '}') {
      ++p_;
      return true;
    }
    for (;;) {
      ws();
      if (p_ >= t_.size() || t_[p_] != '"') return false;
      std::string key;
      if (!parse_string(key)) return false;
      for (const auto& existing : v.o) {
        if (existing.first == key) return false;  // duplicate key
      }
      ws();
      if (p_ >= t_.size() || t_[p_] != ':') return false;
      ++p_;
      ws();
      value child;
      if (!parse_value(child, depth)) return false;
      v.o.emplace_back(std::move(key), std::move(child));
      ws();
      if (p_ >= t_.size()) return false;
      if (t_[p_] == ',') {
        ++p_;
        continue;
      }
      if (t_[p_] == '}') {
        ++p_;
        return true;
      }
      return false;
    }
  }

  bool parse_array(value& v, int depth) {
    v.k = kind::array;
    ++p_;
    ws();
    if (p_ < t_.size() && t_[p_] == ']') {
      ++p_;
      return true;
    }
    for (;;) {
      ws();
      value child;
      if (!parse_value(child, depth)) return false;
      v.a.push_back(std::move(child));
      ws();
      if (p_ >= t_.size()) return false;
      if (t_[p_] == ',') {
        ++p_;
        continue;
      }
      if (t_[p_] == ']') {
        ++p_;
        return true;
      }
      return false;
    }
  }

  static void put_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool hex4(std::uint32_t& out) noexcept {
    if (p_ + 4 > t_.size()) return false;
    out = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = t_[p_++];
      out <<= 4;
      if (c >= '0' && c <= '9') out |= static_cast<std::uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') out |= static_cast<std::uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') out |= static_cast<std::uint32_t>(c - 'A' + 10);
      else return false;
    }
    return true;
  }

  bool parse_string(std::string& out) {
    ++p_;  // opening quote
    while (p_ < t_.size()) {
      const auto c = static_cast<unsigned char>(t_[p_]);
      if (c == '"') {
        ++p_;
        return true;
      }
      if (c < 0x20) return false;
      if (c != '\\') {
        out.push_back(static_cast<char>(c));
        ++p_;
        continue;
      }
      if (++p_ >= t_.size()) return false;
      const char e = t_[p_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          std::uint32_t cp = 0;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            std::uint32_t lo = 0;
            if (p_ + 2 > t_.size() || t_[p_] != '\\' || t_[p_ + 1] != 'u') return false;
            p_ += 2;
            if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return false;
          }
          put_utf8(out, cp);
          break;
        }
        default: return false;
      }
    }
    return false;
  }

  bool parse_number(value& v) {
    const std::size_t start = p_;
    if (p_ < t_.size() && t_[p_] == '-') ++p_;
    if (p_ >= t_.size()) return false;
    if (t_[p_] == '0') {
      ++p_;
    } else if (t_[p_] >= '1' && t_[p_] <= '9') {
      while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
    } else {
      return false;
    }
    bool integer = true;
    if (p_ < t_.size() && t_[p_] == '.') {
      integer = false;
      ++p_;
      const std::size_t digits = p_;
      while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
      if (p_ == digits) return false;
    }
    if (p_ < t_.size() && (t_[p_] == 'e' || t_[p_] == 'E')) {
      integer = false;
      ++p_;
      if (p_ < t_.size() && (t_[p_] == '+' || t_[p_] == '-')) ++p_;
      const std::size_t digits = p_;
      while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
      if (p_ == digits) return false;
    }
    const std::string_view text = t_.substr(start, p_ - start);
    v.k = kind::number;
    if (integer) {
      const auto r = std::from_chars(text.data(), text.data() + text.size(), v.i);
      if (r.ec != std::errc{}) return false;  // out of int64 range
      v.is_integer = true;
      v.d = static_cast<double>(v.i);
      return true;
    }
    // Fractional values are only ever informational here; a simple decimal
    // reading is enough and does not depend on the C locale.
    double mant = 0.0;
    double scale = 1.0;
    bool neg = false;
    bool frac = false;
    std::size_t k = 0;
    if (text[k] == '-') {
      neg = true;
      ++k;
    }
    for (; k < text.size() && text[k] != 'e' && text[k] != 'E'; ++k) {
      if (text[k] == '.') {
        frac = true;
        continue;
      }
      mant = mant * 10.0 + (text[k] - '0');
      if (frac) scale *= 10.0;
    }
    int exp = 0;
    if (k < text.size()) {
      ++k;
      bool eneg = false;
      if (text[k] == '+' || text[k] == '-') eneg = text[k++] == '-';
      for (; k < text.size() && exp < 400; ++k) exp = exp * 10 + (text[k] - '0');
      if (eneg) exp = -exp;
    }
    double d = mant / scale;
    for (; exp > 0; --exp) d *= 10.0;
    for (; exp < 0; ++exp) d /= 10.0;
    v.d = neg ? -d : d;
    return true;
  }
};

// Well-formed UTF-8 (RFC 3629): no overlongs, no surrogates, nothing past
// U+10FFFF. A byte another parser would reject must not reach a manifest field.
[[nodiscard]] inline bool valid_utf8(std::string_view text) noexcept {
  std::size_t i = 0;
  while (i < text.size()) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      ++i;
      continue;
    }
    std::size_t n = 0;
    std::uint32_t cp = 0;
    if (c >= 0xC2 && c <= 0xDF) {
      n = 1;
      cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
      n = 2;
      cp = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
      n = 3;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (text.size() - i <= n) return false;  // truncated sequence
    for (std::size_t k = 1; k <= n; ++k) {
      const auto cc = static_cast<unsigned char>(text[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if ((n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)) ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
      return false;
    }
    i += n + 1;
  }
  return true;
}

}  // namespace detail

[[nodiscard]] inline std::optional<value> parse(std::string_view text, int max_depth = 32) {
  if (!detail::valid_utf8(text)) return std::nullopt;
  value v;
  detail::parser p(text, max_depth);
  if (!p.parse_document(v)) return std::nullopt;
  return v;
}

// A streaming writer. The caller keeps the structure balanced; commas are
// inserted automatically.
class writer {
 public:
  writer& begin_object() { return open('{'); }
  writer& end_object() { return close('}'); }
  writer& begin_array() { return open('['); }
  writer& end_array() { return close(']'); }

  writer& key(std::string_view k) {
    comma();
    quote(k);
    out_.push_back(':');
    after_key_ = true;
    return *this;
  }
  writer& string(std::string_view s) {
    comma();
    quote(s);
    return *this;
  }
  writer& integer(std::int64_t n) {
    comma();
    out_ += std::to_string(n);
    return *this;
  }
  writer& number(double d) {
    comma();
    // Fixed three decimals: enough for MB/s and progress, locale-free.
    const bool neg = d < 0;
    if (neg) d = -d;
    const auto whole = static_cast<std::int64_t>(d);
    const auto milli = static_cast<std::int64_t>((d - static_cast<double>(whole)) * 1000.0 + 0.5);
    if (neg) out_.push_back('-');
    out_ += std::to_string(milli >= 1000 ? whole + 1 : whole);
    out_.push_back('.');
    const std::string m = std::to_string(milli >= 1000 ? 0 : milli);
    out_.append(3 - m.size(), '0');
    out_ += m;
    return *this;
  }
  writer& boolean(bool b) {
    comma();
    out_ += b ? "true" : "false";
    return *this;
  }
  // Pre-serialised JSON (another writer's output), inserted as one value.
  writer& raw(std::string_view json_text) {
    comma();
    out_ += json_text.empty() ? std::string_view("null") : json_text;
    return *this;
  }
  writer& null() {
    comma();
    out_ += "null";
    return *this;
  }

  [[nodiscard]] const std::string& str() const noexcept { return out_; }
  [[nodiscard]] std::string take() { return std::move(out_); }

 private:
  std::string out_;
  std::vector<bool> first_;  // per open container: nothing written yet
  bool after_key_ = false;

  void comma() {
    if (after_key_) {
      after_key_ = false;
      return;
    }
    if (!first_.empty()) {
      if (!first_.back()) out_.push_back(',');
      first_.back() = false;
    }
  }
  writer& open(char c) {
    comma();
    out_.push_back(c);
    first_.push_back(true);
    return *this;
  }
  writer& close(char c) {
    out_.push_back(c);
    if (!first_.empty()) first_.pop_back();
    return *this;
  }
  void quote(std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    out_.push_back('"');
    for (const char ch : s) {
      const auto c = static_cast<unsigned char>(ch);
      switch (c) {
        case '"': out_ += "\\\""; break;
        case '\\': out_ += "\\\\"; break;
        case '\n': out_ += "\\n"; break;
        case '\r': out_ += "\\r"; break;
        case '\t': out_ += "\\t"; break;
        default:
          if (c < 0x20) {
            out_ += "\\u00";
            out_.push_back(kHex[c >> 4]);
            out_.push_back(kHex[c & 0xF]);
          } else {
            out_.push_back(static_cast<char>(c));
          }
      }
    }
    out_.push_back('"');
  }
};

}  // namespace mv::json
