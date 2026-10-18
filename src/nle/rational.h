// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// FCPXML time: a rational number of seconds ("1001/30000s", "5s"), exact.
//
// Timeline arithmetic must not drift: a cut placed 0.4 ms off a frame lands
// on the wrong frame after FCP conforms it. So times stay rational (64-bit
// numerator, positive denominator, reduced) and every operation is exact or
// reports overflow. Header-only and portable (no __int128: MSVC has none).
#pragma once

#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace mv::nle {

struct rational {
  std::int64_t num = 0;
  std::int64_t den = 1;  // > 0
};

namespace detail {

constexpr std::int64_t gcd(std::int64_t a, std::int64_t b) {
  if (a < 0) a = -a;
  if (b < 0) b = -b;
  while (b != 0) {
    const std::int64_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

// a * b, false on overflow.
constexpr bool mul(std::int64_t a, std::int64_t b, std::int64_t& out) {
  if (a == 0 || b == 0) {
    out = 0;
    return true;
  }
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
  if (a == -1) {
    if (b == kMin) return false;
    out = -b;
    return true;
  }
  if (b == -1) {
    if (a == kMin) return false;
    out = -a;
    return true;
  }
  if (a > 0 ? (b > 0 ? a > kMax / b : b < kMin / a) : (b > 0 ? a < kMin / b : a < kMax / b)) return false;
  out = a * b;
  return true;
}

constexpr bool add(std::int64_t a, std::int64_t b, std::int64_t& out) {
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
  if ((b > 0 && a > kMax - b) || (b < 0 && a < kMin - b)) return false;
  out = a + b;
  return true;
}

}  // namespace detail

constexpr rational reduce(rational r) {
  if (r.den < 0) {
    r.num = -r.num;
    r.den = -r.den;
  }
  if (r.num == 0) return {0, 1};
  const std::int64_t g = detail::gcd(r.num, r.den);
  return {r.num / g, r.den / g};
}

// a + b; nullopt on overflow.
constexpr std::optional<rational> add(rational a, rational b) {
  const std::int64_t g = detail::gcd(a.den, b.den);
  std::int64_t l = 0, x = 0, y = 0, n = 0;
  if (!detail::mul(a.den / g, b.den, l)) return std::nullopt;
  if (!detail::mul(a.num, l / a.den, x) || !detail::mul(b.num, l / b.den, y) || !detail::add(x, y, n)) {
    return std::nullopt;
  }
  return reduce({n, l});
}

constexpr std::optional<rational> sub(rational a, rational b) {
  if (b.num == std::numeric_limits<std::int64_t>::min()) return std::nullopt;
  return add(a, {-b.num, b.den});
}

// -1, 0, 1. Exact (cross-multiplication by parts when it would overflow).
constexpr int compare(rational a, rational b) {
  std::int64_t x = 0, y = 0;
  if (detail::mul(a.num, b.den, x) && detail::mul(b.num, a.den, y)) return x < y ? -1 : (x > y ? 1 : 0);
  // Fall back to long double; only reached by absurd times.
  const long double l = static_cast<long double>(a.num) / static_cast<long double>(a.den);
  const long double r = static_cast<long double>(b.num) / static_cast<long double>(b.den);
  return l < r ? -1 : (l > r ? 1 : 0);
}

constexpr bool operator==(rational a, rational b) { return compare(a, b) == 0; }
constexpr bool operator<(rational a, rational b) { return compare(a, b) < 0; }
constexpr bool operator<=(rational a, rational b) { return compare(a, b) <= 0; }
constexpr bool operator>(rational a, rational b) { return compare(a, b) > 0; }
constexpr bool operator>=(rational a, rational b) { return compare(a, b) >= 0; }

constexpr double seconds(rational r) { return static_cast<double>(r.num) / static_cast<double>(r.den); }
constexpr std::int64_t to_ms(rational r) {  // rounded to nearest
  const long double v = static_cast<long double>(r.num) * 1000.0L / static_cast<long double>(r.den);
  return static_cast<std::int64_t>(v < 0 ? v - 0.5L : v + 0.5L);
}
constexpr rational from_ms(std::int64_t ms) { return reduce({ms, 1000}); }

// The nearest multiple of `frame` (a frame duration) to `t`, rounding half up.
constexpr std::optional<rational> snap(rational t, rational frame) {
  if (frame.num <= 0) return t;
  // n = round(t / frame) = round(t.num * frame.den / (t.den * frame.num))
  std::int64_t p = 0, q = 0;
  if (!detail::mul(t.num, frame.den, p) || !detail::mul(t.den, frame.num, q)) return std::nullopt;
  std::int64_t n = p / q;
  const std::int64_t rem = p % q;
  if (rem != 0 && (rem < 0 ? -rem * 2 >= q : rem * 2 >= q)) n += rem < 0 ? -1 : 1;
  std::int64_t num = 0;
  if (!detail::mul(n, frame.num, num)) return std::nullopt;
  return reduce({num, frame.den});
}

// "1001/30000s", "5s", "0s"; also "2.5s" and a bare number (seconds), which
// some third-party writers use. nullopt on anything else.
inline std::optional<rational> parse_time(std::string_view s) {
  if (s.empty()) return std::nullopt;
  if (s.back() == 's') s.remove_suffix(1);
  if (s.empty()) return std::nullopt;
  const std::size_t slash = s.find('/');
  auto integer = [](std::string_view d, std::int64_t& out) {
    if (d.empty()) return false;
    const auto r = std::from_chars(d.data(), d.data() + d.size(), out);
    return r.ec == std::errc() && r.ptr == d.data() + d.size();
  };
  if (slash != std::string_view::npos) {
    std::int64_t n = 0, d = 0;
    if (!integer(s.substr(0, slash), n) || !integer(s.substr(slash + 1), d) || d <= 0) return std::nullopt;
    return reduce({n, d});
  }
  const std::size_t dot = s.find('.');
  if (dot == std::string_view::npos) {
    std::int64_t n = 0;
    if (!integer(s, n)) return std::nullopt;
    return rational{n, 1};
  }
  // A decimal: up to 9 fractional digits, exact.
  const std::string_view whole = s.substr(0, dot);
  const std::string_view frac = s.substr(dot + 1);
  if (frac.empty() || frac.size() > 9) return std::nullopt;
  const bool negative = !whole.empty() && whole[0] == '-';
  std::int64_t w = 0, f = 0;
  if (!(whole.empty() || whole == "-" || integer(whole, w)) || !integer(frac, f) || f < 0) return std::nullopt;
  std::int64_t den = 1;
  for (std::size_t i = 0; i < frac.size(); ++i) den *= 10;
  std::int64_t num = 0;
  if (!detail::mul(w < 0 ? -w : w, den, num) || !detail::add(num, f, num)) return std::nullopt;
  return reduce({negative ? -num : num, den});
}

// FCP's spelling: "0s", "5s", "1001/30000s".
inline std::string format_time(rational r) {
  r = reduce(r);
  if (r.den == 1) return std::to_string(r.num) + "s";
  return std::to_string(r.num) + "/" + std::to_string(r.den) + "s";
}

}  // namespace mv::nle
