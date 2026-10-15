// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #290: the shared halves of the portable readers: the row sink, text cleaning
// and bounded inflate.
#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "meta/parse.h"
#include "meta/parse_util.h"

namespace mv::meta::parse {
namespace {

// Length of the valid UTF-8 sequence at s[i], 0 when it is not one.
std::size_t utf8_len(std::string_view s, std::size_t i) noexcept {
  const auto c = static_cast<unsigned char>(s[i]);
  std::size_t n = 0;
  std::uint32_t cp = 0;
  if (c < 0x80) return 1;
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
    return 0;
  }
  if (s.size() - i <= n) return 0;
  for (std::size_t k = 1; k <= n; ++k) {
    const auto cc = static_cast<unsigned char>(s[i + k]);
    if ((cc & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (cc & 0x3F);
  }
  if ((n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) {
    return 0;
  }
  return n + 1;
}

bool valid_utf8(std::string_view s) noexcept {
  for (std::size_t i = 0; i < s.size();) {
    const std::size_t n = utf8_len(s, i);
    if (n == 0) return false;
    i += n;
  }
  return true;
}

}  // namespace

std::size_t memory_source::read_at(std::uint64_t offset, std::span<std::uint8_t> into) noexcept {
  if (offset >= bytes_.size()) return 0;
  const std::size_t n = std::min<std::size_t>(into.size(), bytes_.size() - static_cast<std::size_t>(offset));
  if (n > 0) std::memcpy(into.data(), bytes_.data() + offset, n);
  return n;
}

std::string clean_text(std::string_view s, std::size_t cap_chars) {
  std::string out;
  out.reserve(std::min(s.size(), cap_chars) + 3);
  // Trailing NULs and whitespace are padding, not content.
  while (!s.empty() && (s.back() == '\0' || s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) {
    s.remove_suffix(1);
  }
  for (std::size_t i = 0; i < s.size();) {
    if (out.size() >= cap_chars) {
      out += "\xE2\x80\xA6";  // …
      return out;
    }
    const std::size_t n = utf8_len(s, i);
    if (n == 0) {
      out += '?';
      ++i;
      continue;
    }
    if (n == 1) {
      const char c = s[i];
      out += (static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\n') || c == 0x7F ? '?' : c;
    } else {
      out.append(s.data() + i, n);
    }
    i += n;
  }
  return out;
}

std::string latin1_or_utf8(std::string_view bytes) {
  if (valid_utf8(bytes)) return std::string(bytes);
  std::string out;
  out.reserve(bytes.size() + bytes.size() / 4);
  for (const char ch : bytes) {
    const auto c = static_cast<unsigned char>(ch);
    if (c < 0x80) {
      out += ch;
    } else {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  return out;
}

bool inflate_bounded(std::span<const std::uint8_t> in, std::size_t max_out, std::string& out) {
  out.clear();
  if (in.empty() || max_out == 0) return false;
  z_stream z{};
  if (inflateInit(&z) != Z_OK) return false;
  bool ok = false;
  try {
    out.resize(std::min<std::size_t>(max_out, std::max<std::size_t>(in.size() * 4, 4096)));
    z.next_in = const_cast<Bytef*>(in.data());
    z.avail_in = static_cast<uInt>(std::min<std::size_t>(in.size(), 0x7FFFFFFF));
    std::size_t produced = 0;
    for (int guard = 0; guard < 4096; ++guard) {
      if (produced == out.size()) {
        if (out.size() >= max_out) break;  // the cap: what fit is kept, the answer is false
        out.resize(std::min(max_out, out.size() * 2));
      }
      z.next_out = reinterpret_cast<Bytef*>(out.data() + produced);
      z.avail_out = static_cast<uInt>(out.size() - produced);
      const int rc = inflate(&z, Z_NO_FLUSH);
      produced = out.size() - z.avail_out;
      if (rc == Z_STREAM_END) {
        ok = true;
        break;
      }
      if (rc != Z_OK && rc != Z_BUF_ERROR) break;
      if (rc == Z_BUF_ERROR && z.avail_in == 0) break;  // truncated input
    }
    out.resize(produced);
  } catch (...) {
    ok = false;
  }
  inflateEnd(&z);
  return ok;
}

bool sink::add(origin space, std::string_view group, std::string_view name, std::string_view label,
               std::string_view value, std::string_view raw_tag, std::size_t cap_chars) {
  if (full()) return false;
  try {
    property p;
    p.space = space;
    p.group = clean_text(group, 128);
    p.name = clean_text(name, 256);
    p.label = clean_text(label.empty() ? name : label, 256);
    p.value = clean_text(value, cap_chars);
    p.raw = p.value;
    p.raw_tag = clean_text(raw_tag, 512);
    rows.push_back(std::move(p));
    return true;
  } catch (...) {
    return false;
  }
}

namespace util {
std::string number_text(double v) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.6g", v);
  return buf;
}
}  // namespace util

}  // namespace mv::meta::parse
