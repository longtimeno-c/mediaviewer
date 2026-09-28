// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/ai/query.h"

#include <algorithm>
#include <array>
#include <utility>

namespace mv::ai::query {

namespace {

constexpr std::uint32_t kPhotos = 1u;  // MV_AI_KIND_PHOTOS (the ABI header stays out of this TU)
constexpr std::uint32_t kVideos = 2u;  // MV_AI_KIND_VIDEOS

bool space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

// A quote: ASCII ", or the typographic “ ” (E2 80 9C / 9D) a Mac field types
// with smart quotes on. Returns its length in bytes, 0 if none.
std::size_t quote_at(std::string_view s, std::size_t i) {
  if (i >= s.size()) return 0;
  if (s[i] == '"') return 1;
  if (i + 2 < s.size() && static_cast<unsigned char>(s[i]) == 0xE2 && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
      (static_cast<unsigned char>(s[i + 2]) == 0x9C || static_cast<unsigned char>(s[i + 2]) == 0x9D)) {
    return 3;
  }
  return 0;
}

std::string lower_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

bool is_one_of(std::string_view w, std::initializer_list<std::string_view> list) {
  return std::find(list.begin(), list.end(), w) != list.end();
}

// Words that join or frame a description and mean nothing alone.
bool connector(std::string_view w) {
  return is_one_of(w, {"and", "or", "with", "&", "+", ",", "of", "the", "a", "an", "in", "at", "on", "to",
                       "from", "by", "for", "featuring", "includes", "include", "including", "|"});
}

// Words a name suggestion or a lone-word name guess never starts from.
bool stop_word(std::string_view w) {
  return connector(w) || is_one_of(w, {"is", "it", "me", "my", "our", "we", "us", "he", "she", "his", "her", "all",
                                       "some", "this", "that", "who", "what", "where", "when"});
}

std::uint32_t kind_word(std::string_view folded) {
  if (is_one_of(folded, {"video", "videos", "clip", "clips", "movie", "movies", "film", "films"})) return kVideos;
  if (is_one_of(folded, {"photo", "photos", "picture", "pictures", "pic", "pics", "image", "images", "still",
                         "stills"})) {
    return kPhotos;
  }
  return 0;
}

// YYYY, YYYY-MM or YYYY-MM-DD ('/' or '.' also separate): [start, end) in
// days since the epoch. False for anything else (a date still being typed).
bool date_range(std::string_view s, std::int64_t& start_day, std::int64_t& end_day) {
  int parts[3] = {0, 0, 0};
  int digits[3] = {0, 0, 0};
  int n = 0;
  for (char c : s) {
    if (c >= '0' && c <= '9') {
      if (digits[n] >= (n == 0 ? 4 : 2)) return false;
      parts[n] = parts[n] * 10 + (c - '0');
      ++digits[n];
    } else if ((c == '-' || c == '/' || c == '.') && n < 2 && digits[n] > 0) {
      ++n;
    } else {
      return false;
    }
  }
  if (digits[0] != 4 || digits[n] == 0) return false;
  const std::int64_t y = parts[0];
  if (y < 1900 || y > 2200) return false;
  if (n == 0) {
    start_day = days_from_civil(y, 1, 1);
    end_day = days_from_civil(y + 1, 1, 1);
    return true;
  }
  const unsigned m = static_cast<unsigned>(parts[1]);
  if (m < 1 || m > 12) return false;
  if (n == 1) {
    start_day = days_from_civil(y, m, 1);
    end_day = m == 12 ? days_from_civil(y + 1, 1, 1) : days_from_civil(y, m + 1, 1);
    return true;
  }
  const unsigned d = static_cast<unsigned>(parts[2]);
  if (d < 1 || d > 31) return false;
  start_day = days_from_civil(y, m, d);
  end_day = start_day + 1;
  return true;
}

// Lead-ins a spoken or typed request starts with (the Voice add-on sends
// its words here too, plan/19). The kind word inside one is kept, so
// "videos of Anna" still asks for videos.
constexpr std::array<std::string_view, 9> kLeadIns = {
    "pull up all the ", "pull up ", "show me all the ", "show me all ", "show me the ", "show me ",
    "find me ", "find all ", "find "};

struct token {
  std::size_t begin = 0;  // byte range in the query, for suggestions
  std::size_t end = 0;
  std::string text;       // the word (or the quoted text), without - @ person: said:
  std::string op;         // "", "@", or an operator before ':' (lower-case)
  bool negated = false;
  bool quoted = false;
  bool open = false;      // quoted and the closing quote is missing
};

std::vector<token> tokens_of(std::string_view s) {
  std::vector<token> out;
  std::size_t i = 0;
  while (i < s.size()) {
    if (space(static_cast<unsigned char>(s[i]))) {
      ++i;
      continue;
    }
    token t;
    t.begin = i;
    if (s[i] == '-' && i + 1 < s.size() && !space(static_cast<unsigned char>(s[i + 1])) && s[i + 1] != '-') {
      t.negated = true;
      ++i;
    }
    // An operator: letters then ':' (in:2024, person:"Anna S"), or '@'.
    if (i < s.size() && s[i] == '@') {
      t.op = "@";
      ++i;
    } else {
      std::size_t j = i;
      while (j < s.size() && ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z'))) ++j;
      if (j > i && j < s.size() && s[j] == ':') {
        const std::string op = lower_ascii(s.substr(i, j - i));
        if (is_one_of(op, {"in", "before", "after", "since", "until", "is", "type", "kind", "person", "said"})) {
          t.op = op;
          i = j + 1;
        } else {
          // Name:"words" (any other word before a colon): the word, then what
          // follows as its own term, so Tristan:"hello" is Tristan "hello".
          t.text = std::string(s.substr(i, j - i));
          t.end = j + 1;
          i = j + 1;
          out.push_back(std::move(t));
          continue;
        }
      }
    }
    if (const std::size_t q = quote_at(s, i)) {
      t.quoted = true;
      i += q;
      std::size_t close = i;
      std::size_t ql = 0;
      while (close < s.size() && (ql = quote_at(s, close)) == 0) ++close;
      t.text = std::string(s.substr(i, close - i));
      t.open = close >= s.size();
      i = t.open ? s.size() : close + ql;
    } else {
      std::size_t j = i;
      while (j < s.size() && !space(static_cast<unsigned char>(s[j])) && quote_at(s, j) == 0) ++j;
      t.text = std::string(s.substr(i, j - i));
      i = j;
    }
    t.end = i;
    out.push_back(std::move(t));
  }
  return out;
}

std::vector<std::string> split_spaces(std::string_view s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (space(static_cast<unsigned char>(c))) {
      if (!cur.empty()) out.push_back(std::move(cur));
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(std::move(cur));
  return out;
}

struct name_words {
  std::int64_t id = 0;
  std::vector<std::string> words;  // folded
};

std::vector<name_words> fold_names(std::span<const person_name> people) {
  std::vector<name_words> out;
  out.reserve(people.size());
  for (const person_name& p : people) {
    name_words n{p.id, split_spaces(fold(p.name))};
    if (!n.words.empty()) out.push_back(std::move(n));
  }
  return out;
}

// How close `w` (folded) comes to spelling the start of a name word: 0 a
// prefix, 1-2 a near spelling, SIZE_MAX none. `fuzzy_from`: the shortest word
// that may be misspelt (a typo in two letters is a different word).
std::size_t prefix_distance(std::string_view w, std::string_view name_word, std::size_t fuzzy_from) {
  if (w.size() <= name_word.size() && name_word.compare(0, w.size(), w) == 0) return 0;
  if (w.size() < fuzzy_from) return SIZE_MAX;
  const std::size_t limit = w.size() >= 8 ? 2 : 1;
  std::size_t best = SIZE_MAX;
  // Against the name word's start of about the same length (a typo can
  // drop or add a letter), and the whole word.
  for (std::size_t len = w.size() > 0 ? w.size() - 1 : 0; len <= w.size() + 1; ++len) {
    if (len == 0 || len > name_word.size()) continue;
    best = std::min(best, edit_distance(w, name_word.substr(0, len), limit));
  }
  return best <= limit ? best : SIZE_MAX;
}

// Named people whose name a word starts or nearly spells, best first.
std::vector<std::pair<std::size_t, std::size_t>> near_names(std::string_view w, const std::vector<name_words>& names,
                                                            std::size_t fuzzy_from) {
  std::vector<std::pair<std::size_t, std::size_t>> out;  // (distance, index)
  for (std::size_t i = 0; i < names.size(); ++i) {
    std::size_t best = SIZE_MAX;
    for (const std::string& nw : names[i].words) best = std::min(best, prefix_distance(w, nw, fuzzy_from));
    if (best != SIZE_MAX) out.push_back({best, i});
  }
  std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  return out;
}

// The folded words `a[i..]` spell name `n` whole (every word), or by its
// first word alone when it has more than one. Returns the words used.
std::size_t spells(const std::vector<std::string>& a, std::size_t i, const name_words& n) {
  if (i + n.words.size() <= a.size() &&
      std::equal(n.words.begin(), n.words.end(), a.begin() + static_cast<std::ptrdiff_t>(i))) {
    return n.words.size();
  }
  if (n.words.size() > 1 && a[i] == n.words.front()) return 1;
  return 0;
}

std::string strip_lead_in(std::string_view q) {
  std::size_t b = 0;
  while (b < q.size() && space(static_cast<unsigned char>(q[b]))) ++b;
  const std::string head = lower_ascii(q.substr(b, 40));
  for (std::string_view lead : kLeadIns) {
    if (head.compare(0, lead.size(), lead) == 0) return std::string(q.substr(b + lead.size()));
  }
  return std::string(q.substr(b));
}

}  // namespace

// ---- helpers --------------------------------------------------------------------------

std::string fold(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 'A' && c <= 'Z') {
      out.push_back(static_cast<char>(c - 'A' + 'a'));
    } else if (c == 0xC3 && i + 1 < s.size()) {
      // Latin-1 letters (U+00C0-U+00FF): their base letter.
      static constexpr char kBase[] = "aaaaaaaceeeeiiiidnooooo/ouuuuyts" "aaaaaaaceeeeiiiidnooooo/ouuuuyty";
      const unsigned char n = static_cast<unsigned char>(s[i + 1]);
      if (n >= 0x80 && n <= 0xBF) {
        const char base = kBase[n - 0x80];
        if (base == '/') {
          out.append(s.substr(i, 2));  // x and ÷ are not letters
        } else if (n == 0x9F) {
          out.append("ss");
        } else if (n == 0x86 || n == 0xA6) {
          out.append("ae");
        } else {
          out.push_back(base);
        }
        ++i;
        continue;
      }
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

std::vector<std::string> words_of(std::string_view utf8) {
  std::vector<std::string> out;
  std::string cur;
  for (unsigned char c : utf8) {
    const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80 ||
                      c == '\'';
    if (word) {
      if (c == '\'') continue;
      cur.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c));
    } else if (!cur.empty()) {
      out.push_back(std::move(cur));
      cur.clear();
    }
  }
  if (!cur.empty()) out.push_back(std::move(cur));
  return out;
}

bool contains_phrase(std::span<const std::string> words, std::span<const std::string> phrase, bool last_prefix) {
  if (phrase.empty()) return false;
  if (phrase.size() > words.size()) return false;
  for (std::size_t i = 0; i + phrase.size() <= words.size(); ++i) {
    bool ok = true;
    for (std::size_t k = 0; k < phrase.size() && ok; ++k) {
      const std::string& w = words[i + k];
      const std::string& p = phrase[k];
      if (w == p) continue;
      ok = last_prefix && k + 1 == phrase.size() && w.size() > p.size() && w.compare(0, p.size(), p) == 0;
    }
    if (ok) return true;
  }
  return false;
}

std::size_t edit_distance(std::string_view a, std::string_view b, std::size_t limit) {
  const std::size_t n = a.size(), m = b.size();
  if ((n > m ? n - m : m - n) > limit) return limit + 1;
  // Three rows of the optimal-string-alignment table.
  std::vector<std::size_t> prev2(m + 1), prev(m + 1), cur(m + 1);
  for (std::size_t j = 0; j <= m; ++j) prev[j] = j;
  for (std::size_t i = 1; i <= n; ++i) {
    cur[0] = i;
    std::size_t row_min = cur[0];
    for (std::size_t j = 1; j <= m; ++j) {
      const std::size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
      if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) cur[j] = std::min(cur[j], prev2[j - 2] + 1);
      row_min = std::min(row_min, cur[j]);
    }
    if (row_min > limit) return limit + 1;
    std::swap(prev2, prev);
    std::swap(prev, cur);
  }
  return std::min(prev[m], limit + 1);
}

std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
  // Howard Hinnant's algorithm.
  y -= m <= 2 ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// ---- number ---------------------------------------------------------------------------

namespace {

constexpr std::array<std::pair<std::string_view, std::string_view>, 18> kIrregular = {{
    {"child", "children"}, {"person", "people"}, {"man", "men"},       {"woman", "women"},
    {"foot", "feet"},      {"tooth", "teeth"},   {"mouse", "mice"},    {"goose", "geese"},
    {"leaf", "leaves"},    {"knife", "knives"},  {"wolf", "wolves"},   {"loaf", "loaves"},
    {"life", "lives"},     {"wife", "wives"},    {"shelf", "shelves"}, {"calf", "calves"},
    {"ox", "oxen"},        {"cactus", "cacti"},
}};

// Nouns that read the same either way, or have no useful other form.
bool invariant(std::string_view w) {
  return is_one_of(w, {"sheep", "fish", "deer", "moose", "series", "species", "news", "glasses", "scissors",
                       "pants", "jeans", "shorts", "trousers", "aircraft", "salmon", "trout", "bison", "snow",
                       "rain", "sunrise", "sunset", "water", "grass", "sand", "fog", "mist", "hair", "rice",
                       "bread", "music", "art", "food", "furniture", "equipment", "luggage", "traffic"});
}

bool vowel(char c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u'; }

bool ends_with(std::string_view w, std::string_view tail) {
  return w.size() >= tail.size() && w.compare(w.size() - tail.size(), tail.size(), tail) == 0;
}

// The singular of a plural-looking word; empty when it does not look plural.
std::string singular_of(std::string_view w) {
  for (const auto& [one, many] : kIrregular) {
    if (w == many) return std::string(one);
  }
  if (w.size() > 4 && ends_with(w, "ies")) return std::string(w.substr(0, w.size() - 3)) + "y";
  for (std::string_view tail : {"sses", "ches", "shes", "xes", "zes"}) {
    if (w.size() > tail.size() + 1 && ends_with(w, tail)) return std::string(w.substr(0, w.size() - 2));
  }
  if (ends_with(w, "ss") || ends_with(w, "us") || ends_with(w, "is") || ends_with(w, "'s")) return {};
  if (w.size() > 3 && ends_with(w, "s")) return std::string(w.substr(0, w.size() - 1));
  return {};
}

std::string plural_of(std::string_view w) {
  for (const auto& [one, many] : kIrregular) {
    if (w == one) return std::string(many);
  }
  if (w.size() > 2 && w.back() == 'y' && !vowel(w[w.size() - 2])) return std::string(w.substr(0, w.size() - 1)) + "ies";
  for (std::string_view tail : {"s", "x", "z", "ch", "sh"}) {
    if (ends_with(w, tail)) return std::string(w) + "es";
  }
  return std::string(w) + "s";
}

bool numberable(std::string_view w) {
  if (w.size() < 3 || stop_word(w) || invariant(w) || kind_word(w) != 0) return false;
  for (char c : w) {
    if (c < 'a' || c > 'z') return false;  // a number, a name with an accent, punctuation
  }
  // Verbs and adverbs have no plural worth embedding.
  if ((w.size() > 5 && ends_with(w, "ing")) || (w.size() > 4 && ends_with(w, "ly"))) return false;
  return true;
}

}  // namespace

std::string other_number(std::string_view word) {
  const std::string w = lower_ascii(word);
  if (!numberable(w)) return {};
  if (std::string s = singular_of(w); !s.empty()) return s;
  for (const auto& [one, many] : kIrregular) {
    if (w == many) return std::string(one);
  }
  return plural_of(w);
}

std::vector<std::string> number_forms(std::string_view text) {
  std::vector<std::string> out;
  // The last word: letters only, after trimming.
  std::size_t s0 = 0, e = text.size();
  while (s0 < e && space(static_cast<unsigned char>(text[s0]))) ++s0;
  while (e > s0 && space(static_cast<unsigned char>(text[e - 1]))) --e;
  std::size_t b = e;
  while (b > s0 && !space(static_cast<unsigned char>(text[b - 1]))) --b;
  const std::string head(text.substr(s0, b - s0));
  const std::string last = lower_ascii(text.substr(b, e - b));
  const std::string trimmed(text.substr(s0, e - s0));
  if (!numberable(last)) {
    out.push_back(trimmed);
    return out;
  }
  // One canonical pair whichever number was typed: the singular, then its
  // plural ("bus" / "buses" and "buse" / "buses" differ: English is not
  // regular enough to promise more).
  std::string one = singular_of(last);
  if (one.empty()) one = last;
  const std::string many = plural_of(one);
  if (one == many) {
    out.push_back(trimmed);
    return out;
  }
  out.push_back(head + one);
  out.push_back(head + many);
  return out;
}

// ---- parse ----------------------------------------------------------------------------

parsed parse(std::string_view utf8) {
  parsed p;
  const std::string q = strip_lead_in(utf8);
  const std::vector<token> toks = tokens_of(q);
  bool or_pending = false;
  std::vector<std::size_t> bare;  // indexes into p.words of plain words (kind-word candidates)
  for (const token& t : toks) {
    if (t.op.empty() && !t.quoted && !t.negated && (t.text == "|" || lower_ascii(t.text) == "or")) {
      or_pending = !p.words.empty();
      continue;
    }
    if (t.quoted && (t.op.empty() || t.op == "said")) {
      if (!words_of(t.text).empty()) p.phrases.push_back({t.text, t.negated, t.open});
      or_pending = false;
      continue;
    }
    if (t.op == "said") {
      if (!words_of(t.text).empty()) p.phrases.push_back({t.text, t.negated, false});
      or_pending = false;
      continue;
    }
    if (t.op == "@" || t.op == "person") {
      if (!fold(t.text).empty()) p.words.push_back({t.text, t.negated, true, or_pending});
      or_pending = false;
      continue;
    }
    if (t.op == "is" || t.op == "type" || t.op == "kind") {
      const std::uint32_t k = kind_word(fold(t.text));
      (t.negated ? p.not_kinds : p.kinds) |= k;
      continue;
    }
    if (!t.op.empty()) {
      // A date. One still being typed ("in:20") is dropped, not searched.
      std::int64_t start = 0, end = 0;
      if (!date_range(t.text, start, end)) continue;
      std::int64_t from = std::numeric_limits<std::int64_t>::min(), to = std::numeric_limits<std::int64_t>::max();
      if (t.op == "in") {
        from = start;
        to = end;
      } else if (t.op == "before") {
        to = start;
      } else if (t.op == "after") {
        from = end;
      } else if (t.op == "since") {
        from = start;
      } else if (t.op == "until") {
        to = end;
      }
      if (t.negated && t.op == "in") {
        // -in:2024 is not a range; leave it out rather than guess.
        continue;
      }
      if (from != std::numeric_limits<std::int64_t>::min()) p.from_unix = std::max(p.from_unix, from * 86400);
      if (to != std::numeric_limits<std::int64_t>::max()) p.to_unix = std::min(p.to_unix, to * 86400);
      continue;
    }
    if (t.text.empty()) continue;
    // A plain word, or "-word".
    if (t.negated) {
      if (const std::uint32_t k = kind_word(fold(t.text))) {
        p.not_kinds |= k;
        continue;
      }
    }
    if (!t.negated) bare.push_back(p.words.size());
    p.words.push_back({t.text, t.negated, false, or_pending});
    or_pending = false;
  }
  // A kind word last among the plain words is a filter ("Anna beach photos",
  // "Tristan video"); in the middle it is part of a description ("a video
  // camera"). First, "videos of" / "clips of" still asks for videos, but
  // "photos of" / "pictures of" is how people ask for anything (a spoken
  // request, plan/19): it is dropped, not a photos-only filter, as before
  // 2026-09-28.
  const auto take_kind = [&](std::size_t at, bool first) {
    const std::uint32_t k = kind_word(fold(p.words[at].text));
    if (k == 0) return;
    if (!first || k == kVideos) p.kinds |= k;
    p.words.erase(p.words.begin() + static_cast<std::ptrdiff_t>(at));
  };
  if (!bare.empty()) {
    // The last first: erasing it leaves the first's index as it was.
    take_kind(bare.back(), false);
    if (bare.front() != bare.back()) take_kind(bare.front(), true);
  }
  return p;
}

// ---- resolve --------------------------------------------------------------------------

plan resolve(const parsed& p, std::span<const person_name> people) {
  plan out;
  out.kinds = p.kinds;
  if (p.not_kinds != 0) out.kinds = (out.kinds == 0 ? (kPhotos | kVideos) : out.kinds) & ~p.not_kinds;
  if ((p.kinds != 0 || p.not_kinds != 0) && out.kinds == 0) out.impossible = true;
  out.from_unix = p.from_unix;
  out.to_unix = p.to_unix;
  if (out.from_unix >= out.to_unix) out.impossible = true;
  for (const parsed::phrase& ph : p.phrases) {
    if (ph.negated) {
      out.not_phrases.push_back(ph.text);
    } else {
      out.phrases.push_back(ph.text);
      out.last_phrase_open = ph.open;
    }
  }
  const std::vector<name_words> names = fold_names(people);
  std::vector<std::string> folded;
  folded.reserve(p.words.size());
  for (const parsed::word& w : p.words) folded.push_back(fold(w.text));

  std::vector<std::string> text;  // the words left for the towers, as typed
  bool last_was_person = false;
  const auto add_group = [&](std::vector<std::int64_t> ids, bool or_before) {
    if (or_before && last_was_person && !out.people.empty()) {
      for (std::int64_t id : ids) {
        if (std::find(out.people.back().begin(), out.people.back().end(), id) == out.people.back().end()) {
          out.people.back().push_back(id);
        }
      }
    } else {
      out.people.push_back(std::move(ids));
    }
    last_was_person = true;
  };
  for (std::size_t i = 0; i < p.words.size(); ++i) {
    const parsed::word& w = p.words[i];
    if (w.person) {
      // @name / person:name: whole names, then name starts, then near spellings.
      std::vector<std::int64_t> ids;
      const std::vector<std::string> want = split_spaces(folded[i]);
      for (const name_words& n : names) {
        if (!want.empty() && spells(want, 0, n) == want.size()) ids.push_back(n.id);
      }
      if (ids.empty() && want.size() == 1) {
        const auto near = near_names(want.front(), names, 4);
        for (const auto& [d, at] : near) {
          if (d == near.front().first) ids.push_back(names[at].id);
        }
      }
      if (ids.empty()) {
        if (!w.negated) out.impossible = true;
        continue;
      }
      if (w.negated) {
        out.not_people.insert(out.not_people.end(), ids.begin(), ids.end());
      } else {
        add_group(std::move(ids), w.or_before);
      }
      continue;
    }
    // A plain word: a whole name (the longest that fits, over several words),
    // or a first name.
    std::vector<std::int64_t> ids;
    std::size_t used = 0;
    // Only plain words of the same sign make up one name.
    std::size_t run_end = i;
    while (run_end < p.words.size() && !p.words[run_end].person && p.words[run_end].negated == w.negated) ++run_end;
    const std::vector<std::string> run(folded.begin() + static_cast<std::ptrdiff_t>(i),
                                       folded.begin() + static_cast<std::ptrdiff_t>(run_end));
    for (const name_words& n : names) {
      const std::size_t k = spells(run, 0, n);
      if (k == 0) continue;
      if (k > used) {
        used = k;
        ids.clear();
      }
      if (k == used) ids.push_back(n.id);
    }
    if (used > 0) {
      if (w.negated) {
        out.not_people.insert(out.not_people.end(), ids.begin(), ids.end());
      } else {
        add_group(std::move(ids), w.or_before);
      }
      i += used - 1;
      continue;
    }
    if (w.negated) {
      out.not_text.push_back(w.text);
      continue;
    }
    // "or" between two people groups them; before a description it stays a word.
    if (w.or_before && !text.empty()) text.push_back("or");
    text.push_back(w.text);
    if (!connector(folded[i])) last_was_person = false;
  }
  // Joining words at either end ("and beach", "Anna with") mean nothing, and
  // nor do connectors alone ("Tristan at the"). A preposition that leads a
  // description stays: "on a skateboard" embeds better than "skateboard".
  const auto joins = [](const std::string& s) {
    return is_one_of(fold(s), {"and", "or", "&", "+", ",", "|", "of", "with", "featuring", "includes", "include",
                               "including", "that", "which", "where"});
  };
  while (!text.empty() && joins(text.front())) text.erase(text.begin());
  while (!text.empty() && joins(text.back())) text.pop_back();
  if (std::all_of(text.begin(), text.end(), [](const std::string& s) { return connector(fold(s)); })) text.clear();
  for (const std::string& s : text) out.text += (out.text.empty() ? "" : " ") + s;

  // A lone word that starts a name, or nearly spells one: those people lead
  // and the word still describes (a union). Not for a stop word, and not
  // when a person is already named.
  if (out.people.empty() && text.size() == 1 && !names.empty()) {
    const std::string w = fold(text.front());
    if (w.size() >= 2 && !stop_word(w)) {
      const auto near = near_names(w, names, 4);
      for (const auto& [d, at] : near) {
        if (d == near.front().first) out.people_first.push_back(names[at].id);
      }
    }
  }
  return out;
}

// ---- suggest --------------------------------------------------------------------------

std::vector<suggestion> suggest(std::string_view utf8, std::span<const person_name> people, std::size_t max) {
  std::vector<suggestion> out;
  if (utf8.empty() || space(static_cast<unsigned char>(utf8.back())) || people.empty()) return out;
  const std::vector<token> toks = tokens_of(utf8);
  if (toks.empty()) return out;
  const token& t = toks.back();
  const bool explicit_person = t.op == "@" || t.op == "person";
  if (t.quoted || (!t.op.empty() && !explicit_person)) return out;
  const std::string w = fold(t.text);
  if (w.empty() && !explicit_person) return out;
  if (!explicit_person && (w.size() < 2 || stop_word(w))) return out;
  const std::vector<name_words> names = fold_names(people);
  // Already a whole name: nothing to complete (another name that starts with
  // it is still offered).
  std::vector<std::pair<std::size_t, std::size_t>> near;
  if (w.empty()) {
    for (std::size_t i = 0; i < names.size(); ++i) near.push_back({0, i});
  } else {
    near = near_names(w, names, 3);
  }
  const std::string before(utf8.substr(0, t.begin));
  for (const auto& [d, at] : near) {
    if (out.size() >= max) break;
    const person_name* pn = nullptr;
    for (const person_name& p : people) {
      if (p.id == names[at].id) pn = &p;
    }
    if (!pn) continue;
    if (fold(pn->name) == w) continue;
    std::string word = pn->name;
    const bool spaced = word.find(' ') != std::string::npos;
    std::string lead = t.negated ? "-" : "";
    if (t.op == "@") lead += "@";
    if (t.op == "person") lead += "person:";
    if (explicit_person && spaced) word = "\"" + word + "\"";
    out.push_back({pn->id, pn->name, before + lead + word + " "});
  }
  return out;
}

}  // namespace mv::ai::query
