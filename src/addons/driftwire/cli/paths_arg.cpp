// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/paths_arg.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <unordered_set>

#include "addons/driftwire/cli/data_dir.h"

namespace mv::driftwire::cli {

namespace fs = std::filesystem;

namespace {

// A glob that matches more than this is cut there (the engine caps a send far lower).
constexpr std::size_t kMaxMatches = 20000;

fs::path to_path(std::string_view utf8) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string to_utf8(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

bool separator(char c, bool windows) { return c == '/' || (windows && c == '\\'); }

// `literal` from `cwd`, absolute and lexically normal, with no trailing separator.
fs::path absolute_of(std::string_view literal, const path_options& o) {
  fs::path p = to_path(literal);
  if (!p.is_absolute()) p = to_path(o.cwd) / p;
  p = p.lexically_normal();
  if (!p.has_filename() && p.has_relative_path()) p = p.parent_path();
  return p;
}

bool path_exists(const fs::path& p) {
  std::error_code ec;
  return fs::exists(p, ec);
}

std::vector<char32_t> code_points(std::string_view s, std::vector<std::size_t>* starts = nullptr) {
  std::vector<char32_t> out;
  for (std::size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (i + n > s.size()) n = 1;
    char32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (std::size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    if (starts) starts->push_back(i);
    out.push_back(cp);
    i += n;
  }
  return out;
}

char32_t fold(char32_t c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }

bool match_impl(std::string_view name, std::string_view pattern, const std::vector<bool>& live, bool fold_case) {
  const std::vector<char32_t> n = code_points(name);
  std::vector<std::size_t> starts;
  const std::vector<char32_t> p = code_points(pattern, &starts);
  const auto is_live = [&](std::size_t k) { return starts[k] < live.size() && live[starts[k]]; };
  const auto same = [&](char32_t a, char32_t b) { return fold_case ? fold(a) == fold(b) : a == b; };
  // One pattern item at `k` against `c`: its length in code points, 0 for no match.
  const auto one = [&](std::size_t k, char32_t c) -> std::size_t {
    if (is_live(k) && p[k] == '?') return 1;
    if (is_live(k) && p[k] == '[') {
      std::size_t j = k + 1;
      const bool negate = j < p.size() && (p[j] == '!' || p[j] == '^');
      if (negate) ++j;
      const std::size_t first = j;
      bool hit = false;
      for (; j < p.size(); ++j) {
        if (p[j] == ']' && j > first) break;
        if (j + 2 < p.size() && p[j + 1] == '-' && p[j + 2] != ']') {
          if (fold_case ? (fold(c) >= fold(p[j]) && fold(c) <= fold(p[j + 2])) : (c >= p[j] && c <= p[j + 2])) {
            hit = true;
          }
          j += 2;
        } else if (same(c, p[j])) {
          hit = true;
        }
      }
      if (j >= p.size()) return same(c, '[') ? 1 : 0;  // no closing ]: a plain [
      return hit != negate ? j - k + 1 : 0;
    }
    return same(c, p[k]) ? 1 : 0;
  };
  std::size_t i = 0, k = 0, star_k = std::string::npos, star_i = 0;
  while (i < n.size()) {
    if (k < p.size() && is_live(k) && p[k] == '*') {
      star_k = ++k;
      star_i = i;
      continue;
    }
    if (k < p.size()) {
      if (const std::size_t len = one(k, n[i])) {
        k += len;
        ++i;
        continue;
      }
    }
    if (star_k != std::string::npos) {
      k = star_k;
      i = ++star_i;
      continue;
    }
    return false;
  }
  while (k < p.size() && is_live(k) && p[k] == '*') ++k;
  return k == p.size();
}

struct component {
  std::string s;
  std::vector<bool> live;
  [[nodiscard]] bool globbed() const { return std::find(live.begin(), live.end(), true) != live.end(); }
};

// The matches of `rest` (components) under `dir`, appended to `out`.
void expand(const fs::path& dir, const std::vector<component>& rest, std::size_t at, bool fold_case,
            std::vector<fs::path>& out) {
  if (out.size() >= kMaxMatches) return;
  if (at == rest.size()) {
    if (path_exists(dir)) out.push_back(dir);
    return;
  }
  const component& c = rest[at];
  const bool last = at + 1 == rest.size();
  if (!c.globbed()) {
    expand(dir / to_path(c.s), rest, at + 1, fold_case, out);
    return;
  }
  // A leading dot is matched only by a dot typed (a shell's rule).
  const bool dot_ok = !c.s.empty() && c.s[0] == '.' && !c.live[0];
  std::vector<fs::path> hits;
  std::error_code ec;
  for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), stop; !ec && it != stop;
       it.increment(ec)) {
    const std::string name = to_utf8(it->path().filename());
    if (name.empty() || (name[0] == '.' && !dot_ok)) continue;
    if (!glob_match(name, c.s, c.live) && !(fold_case && match_impl(name, c.s, c.live, true))) continue;
    std::error_code dec;
    if (!last && !it->is_directory(dec)) continue;
    hits.push_back(it->path());
  }
  std::sort(hits.begin(), hits.end());
  for (const auto& h : hits) {
    if (out.size() >= kMaxMatches) return;
    expand(h, rest, at + 1, fold_case, out);
  }
}

std::vector<fs::path> expand_glob(const std::string& literal, const std::vector<bool>& live, const path_options& o) {
  // The longest part before the first glob character that ends at a
  // separator is the folder to start in; the rest are components.
  std::size_t first_live = 0;
  while (first_live < literal.size() && !live[first_live]) ++first_live;
  std::size_t cut = 0;  // one past the separator
  for (std::size_t i = 0; i < first_live; ++i) {
    if (separator(literal[i], o.windows)) cut = i + 1;
  }
  const fs::path base = absolute_of(cut == 0 ? std::string_view(".") : std::string_view(literal).substr(0, cut), o);
  std::vector<component> rest;
  component cur;
  for (std::size_t i = cut; i < literal.size(); ++i) {
    if (separator(literal[i], o.windows)) {
      if (!cur.s.empty()) rest.push_back(std::move(cur));
      cur = {};
      continue;
    }
    cur.s += literal[i];
    cur.live.push_back(live[i]);
  }
  if (!cur.s.empty()) rest.push_back(std::move(cur));
  std::vector<fs::path> out;
  expand(base, rest, 0, o.windows, out);
  return out;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

path_options default_path_options() {
  path_options o;
#if defined(_WIN32)
  o.windows = true;
#endif
  // UTF-8 on every OS (data_dir.h): on Windows the CRT's getenv("USERPROFILE")
  // is in the ANSI code page, which a user name like "José" is not UTF-8 in.
  o.home = home_dir();
  std::error_code ec;
  const fs::path cwd = fs::current_path(ec);
  if (!ec) o.cwd = to_utf8(cwd);
  return o;
}

std::vector<word> split_words(std::string_view line, bool windows) {
  std::vector<word> out;
  word cur;
  bool in_word = false;
  char quote = 0;
  const auto add = [&](char c, bool live) {
    cur.text += c;
    cur.glob.push_back(live);
  };
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (quote == '\'') {
      if (c == '\'') {
        quote = 0;
      } else {
        add(c, false);
      }
      continue;
    }
    if (quote == '"') {
      if (c == '"') {
        quote = 0;
      } else if (!windows && c == '\\' && i + 1 < line.size() &&
                 std::string_view("\"\\$`").find(line[i + 1]) != std::string_view::npos) {
        add(line[++i], false);
      } else {
        add(c, false);
      }
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (in_word) {
        cur.end = i;
        out.push_back(std::move(cur));
        cur = {};
        in_word = false;
      }
      continue;
    }
    if (!in_word) {
      in_word = true;
      cur.begin = i;
      // ~ alone or before a separator; "~ann" is a name, not ann's home.
      cur.tilde = c == '~' && (i + 1 == line.size() || line[i + 1] == ' ' || line[i + 1] == '\t' ||
                               line[i + 1] == '\n' || line[i + 1] == '\r' || separator(line[i + 1], windows));
    }
    if (c == '\'' && !windows) {  // on Windows an apostrophe is a letter ("Ann's photos")
      quote = '\'';
    } else if (c == '"') {
      quote = '"';
    } else if (c == '\\' && !windows) {
      if (i + 1 < line.size()) add(line[++i], false);
    } else {
      add(c, c == '*' || c == '?' || c == '[');
    }
  }
  if (in_word) {
    cur.end = line.size();
    cur.open_quote = quote;
    out.push_back(std::move(cur));
  }
  return out;
}

std::string file_uri_path(std::string_view s, bool windows) {
  if (s.size() < 7) return {};
  for (std::size_t i = 0; i < 7; ++i) {
    const char want = "file://"[i];
    const char c = s[i] >= 'A' && s[i] <= 'Z' ? static_cast<char>(s[i] - 'A' + 'a') : s[i];
    if (c != want) return {};
  }
  std::string_view rest = s.substr(7);
  const std::size_t slash = rest.find('/');
  const std::string_view host = rest.substr(0, slash);
  if (slash == std::string_view::npos) return {};
  std::string_view encoded = rest.substr(slash);
  std::string out;
  if (!host.empty() && host != "localhost") {
    if (!windows) return {};  // another machine's file
    out = "\\\\" + std::string(host);
  }
  for (std::size_t i = 0; i < encoded.size(); ++i) {
    if (encoded[i] == '%' && i + 2 < encoded.size()) {
      const int hi = hex_value(encoded[i + 1]), lo = hex_value(encoded[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>(hi * 16 + lo);
        i += 2;
        continue;
      }
    }
    out += encoded[i];
  }
  // file:///C:/Users/... is C:/Users/...
  if (windows && host.empty() && out.size() >= 3 && out[0] == '/' && out[2] == ':') out.erase(0, 1);
  return out;
}

bool glob_match(std::string_view name, std::string_view pattern, const std::vector<bool>& live) {
  return match_impl(name, pattern, live, false);
}

namespace {

bool file_scheme(std::string_view s) {
  if (s.size() <= 5) return false;
  for (std::size_t i = 0; i < 5; ++i) {
    const char c = s[i] >= 'A' && s[i] <= 'Z' ? static_cast<char>(s[i] - 'A' + 'a') : s[i];
    if (c != "file:"[i]) return false;
  }
  return true;
}

std::string_view trimmed(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
    s.remove_suffix(1);
  }
  return s;
}

}  // namespace

parsed_paths parse_paths(std::string_view line, const path_options& o) {
  // One path with spaces in it, pasted as it is (a file manager's copy, a
  // terminal that does not quote what is dropped on it): when the words
  // name something missing but the whole line names a file, it is that file.
  if (const std::string_view whole = trimmed(line);
      whole.find_first_of(o.windows ? "\"*?[" : "'\"\\*?[") == std::string_view::npos &&
      whole.find(' ') != std::string_view::npos) {
    std::string literal(whole);
    if (!o.home.empty() && literal.size() > 1 && literal[0] == '~' && separator(literal[1], o.windows)) {
      literal = o.home + literal.substr(1);
    }
    const fs::path p = absolute_of(literal, o);
    if (path_exists(p)) {
      parsed_paths split;
      bool all_there = true;
      for (const word& w : split_words(line, o.windows)) {
        if (!path_exists(absolute_of(w.text, o))) all_there = false;
      }
      if (!all_there) {
        split.paths.push_back(to_utf8(p));
        return split;
      }
    }
  }
  parsed_paths r;
  std::unordered_set<std::string> seen;
  const auto add = [&](const fs::path& p) {
    std::string s = to_utf8(p);
    if (seen.insert(s).second) r.paths.push_back(std::move(s));
  };
  for (const word& w : split_words(line, o.windows)) {
    if (w.open_quote) r.unterminated = true;
    if (w.text.empty()) continue;
    std::string literal = w.text;
    std::vector<bool> live = w.glob;
    if (file_scheme(literal)) {
      literal = file_uri_path(w.text, o.windows);
      if (literal.empty()) {
        r.missing.push_back(w.text);
        continue;
      }
      live.assign(literal.size(), false);
    } else if (w.tilde && !o.home.empty()) {
      literal = o.home + w.text.substr(1);
      live.assign(o.home.size(), false);
      live.insert(live.end(), w.glob.begin() + 1, w.glob.end());
    }
    if (std::find(live.begin(), live.end(), true) != live.end()) {
      const std::vector<fs::path> hits = expand_glob(literal, live, o);
      if (!hits.empty()) {
        for (const auto& h : hits) add(h.lexically_normal());
        continue;
      }
      // No match: perhaps a name with a [ in it, typed as it is.
    }
    const fs::path p = absolute_of(literal, o);
    if (path_exists(p)) {
      add(p);
    } else {
      r.missing.push_back(file_scheme(w.text) ? literal : w.text);  // as typed; a URI decoded
    }
  }
  return r;
}

}  // namespace mv::driftwire::cli
