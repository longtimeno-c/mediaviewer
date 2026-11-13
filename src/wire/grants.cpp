// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "wire/grants.h"

#include <sodium.h>

#include <algorithm>
#include <charconv>
#include <span>

#include "wire/known.h"
#include "core/json.h"

namespace mv::driftwire::serve::grants {

namespace {

std::string hex(std::span<const std::uint8_t> b) {
  std::string out(b.size() * 2, '0');
  sodium_bin2hex(out.data(), out.size() + 1, b.data(), b.size());
  return out;
}

template <std::size_t N>
std::optional<std::array<std::uint8_t, N>> unhex(const std::string* s) {
  if (!s || s->size() != N * 2) return std::nullopt;
  std::array<std::uint8_t, N> out{};
  std::size_t len = 0;
  if (sodium_hex2bin(out.data(), out.size(), s->data(), s->size(), nullptr, &len, nullptr) != 0 || len != N) return std::nullopt;
  return out;
}

bool root_id(std::string_view s) {
  return s.size() == 16 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// Sorted and unique, so the same set always reads back the same (the watch
// compares them, and a reordering must not cut a phone).
void write_roots(json::writer& w, const std::optional<std::vector<std::string>>& roots) {
  if (!roots) return;
  std::vector<std::string> ids = *roots;
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  w.key("roots").begin_array();
  for (const auto& r : ids) w.string(r);
  w.end_array();
}

// Absent: nullopt (all). Present and not a list of ids: false.
bool read_roots(const json::value& v, std::optional<std::vector<std::string>>& out) {
  out.reset();
  const json::value* r = v.find("roots");
  if (!r) return true;
  if (r->k != json::kind::array || r->a.size() > kMaxRootIds) return false;
  std::vector<std::string> ids;
  for (const auto& e : r->a) {
    if (e.k != json::kind::string || !root_id(e.s)) return false;
    ids.push_back(e.s);
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  out = std::move(ids);
  return true;
}

std::array<std::uint8_t, 32> code_hash(std::string_view code) {
  static constexpr char kLabel[] = "mv.serve.guest-code.v1";
  std::array<std::uint8_t, 32> h{};
  crypto_generichash(h.data(), h.size(), reinterpret_cast<const std::uint8_t*>(code.data()), code.size(),
                     reinterpret_cast<const std::uint8_t*>(kLabel), sizeof kLabel - 1);
  return h;
}

struct stored_code {
  std::array<std::uint8_t, 32> hash{};
  std::int64_t valid_until = 0;
  limit gives;
};

std::string encode_codes(const std::vector<stored_code>& codes) {
  json::writer w;
  w.begin_object().key("v").integer(1).key("codes").begin_array();
  for (const auto& c : codes) {
    w.begin_object().key("hash").string(hex(c.hash)).key("valid_until").integer(c.valid_until).key("for_s").integer(c.gives.for_s);
    write_roots(w, c.gives.roots);
    w.end_object();
  }
  w.end_array().end_object();
  return w.take();
}

// false: there and unreadable.
bool load_codes(secrets::store& store, std::vector<stored_code>& out) {
  out.clear();
  if (!store.exists(kGuestCodesName)) return true;
  const auto bytes = store.get(kGuestCodesName);
  if (!bytes) return false;
  const auto doc = json::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), 4);
  const json::value* list = doc ? doc->find("codes") : nullptr;
  if (!doc || doc->integer("v") != 1 || !list || list->k != json::kind::array) return false;
  for (const auto& e : list->a) {
    stored_code c;
    const auto h = unhex<32>(e.str("hash"));
    const auto until = e.integer("valid_until");
    const auto for_s = e.integer("for_s");
    if (!h || !until || !for_s || *for_s < 0 || *for_s > kMaxSeconds || !read_roots(e, c.gives.roots)) return false;
    c.hash = *h;
    c.valid_until = *until;
    c.gives.for_s = *for_s;
    out.push_back(std::move(c));
  }
  return true;
}

bool save_codes(secrets::store& store, const std::vector<stored_code>& codes) {
  if (codes.empty()) {
    (void)store.erase(kGuestCodesName);
    return !store.exists(kGuestCodesName);
  }
  const std::string text = encode_codes(codes);
  return store.put(kGuestCodesName, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

}  // namespace

bool grant::allows(std::string_view id, bool catalogue) const {
  if (catalogue || !roots) return true;
  return std::find(roots->begin(), roots->end(), id) != roots->end();
}

std::string encode(const std::vector<grant>& list) {
  json::writer w;
  w.begin_object().key("v").integer(1).key("grants").begin_array();
  for (const auto& g : list) {
    w.begin_object();
    w.key("key").string(hex(g.key)).key("name").string(g.name);
    w.key("expires").integer(g.expires).key("guest").boolean(g.guest).key("created").integer(g.created);
    write_roots(w, g.roots);
    w.end_object();
  }
  w.end_array().end_object();
  return w.take();
}

std::optional<std::vector<grant>> decode(std::string_view text) {
  if (text.size() > 256 * 1024) return std::nullopt;
  const auto doc = json::parse(text, 4);
  const json::value* list = doc ? doc->find("grants") : nullptr;
  if (!doc || doc->integer("v") != 1 || !list || list->k != json::kind::array || list->a.size() > kMaxGrants) {
    return std::nullopt;
  }
  std::vector<grant> out;
  for (const auto& e : list->a) {
    grant g;
    const auto key = unhex<32>(e.str("key"));
    const std::string* name = e.str("name");
    const auto expires = e.integer("expires");
    const auto guest = e.boolean("guest");
    const auto created = e.integer("created");
    if (!key || !name || name->size() > known::kMaxName || !expires || *expires < 0 || !guest || !created ||
        !read_roots(e, g.roots)) {
      return std::nullopt;
    }
    // A grant with no limit at all belongs in the paired list, not here.
    if (*expires == 0 && !g.roots) return std::nullopt;
    g.key = *key;
    g.name = *name;
    g.expires = *expires;
    g.guest = *guest;
    g.created = *created;
    out.push_back(std::move(g));
  }
  return out;
}

bool load(secrets::store& store, std::vector<grant>& out) {
  out.clear();
  if (!store.exists(kGrantsName)) return true;
  const auto bytes = store.get(kGrantsName);
  if (!bytes) return false;
  auto list = decode(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
  if (!list) return false;
  out = std::move(*list);
  return true;
}

bool save(secrets::store& store, const std::vector<grant>& list) {
  if (list.empty()) {
    (void)store.erase(kGrantsName);
    return !store.exists(kGrantsName);
  }
  const std::string text = encode(list);
  return store.put(kGrantsName, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

bool prune(std::vector<grant>& list, std::int64_t now) {
  const auto before = list.size();
  std::erase_if(list, [&](const grant& g) { return g.expires != 0 && now >= g.expires + kExpiredKeptSeconds; });
  return list.size() != before;
}

std::size_t live(const std::vector<grant>& list, std::int64_t now) {
  return static_cast<std::size_t>(std::count_if(list.begin(), list.end(), [&](const grant& g) { return !g.expired(now); }));
}

bool make_room(std::vector<grant>& list, std::size_t unlimited, std::int64_t now) {
  if (unlimited + live(list, now) >= known::kMaxDevices) return false;
  while (list.size() >= kMaxGrants) {
    // The oldest expired one goes first; there is one, since live ones are fewer than kMaxDevices.
    auto oldest = list.end();
    for (auto it = list.begin(); it != list.end(); ++it) {
      if (it->expired(now) && (oldest == list.end() || it->expires < oldest->expires)) oldest = it;
    }
    if (oldest == list.end()) return false;
    list.erase(oldest);
  }
  return true;
}

std::optional<std::int64_t> parse_duration(std::string_view text) {
  if (text.empty() || text.size() > 12) return std::nullopt;
  std::int64_t unit = 1;
  switch (text.back()) {
    case 'm': unit = 60; break;
    case 'h': unit = 3600; break;
    case 'd': unit = 24 * 3600; break;
    case 's': unit = 1; break;
    default: unit = 0; break;
  }
  const std::string_view digits = unit == 0 ? text : text.substr(0, text.size() - 1);
  if (unit == 0) unit = 1;
  std::int64_t n = 0;
  const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
  if (ec != std::errc{} || end != digits.data() + digits.size() || n <= 0 || n > kMaxSeconds / unit) return std::nullopt;
  return n * unit;
}

std::string issue_guest_code(secrets::store& store, std::int64_t valid_s, const limit& gives, std::int64_t now) {
  if (sodium_init() < 0 || valid_s <= 0 || gives.for_s < 0 || gives.for_s > kMaxSeconds || !gives.any()) return {};
  std::vector<stored_code> codes;
  if (!load_codes(store, codes)) return {};
  std::erase_if(codes, [&](const stored_code& c) { return c.valid_until <= now; });
  std::array<std::uint8_t, 16> raw{};
  randombytes_buf(raw.data(), raw.size());
  std::string code = hex(raw);
  sodium_memzero(raw.data(), raw.size());
  stored_code c;
  c.hash = code_hash(code);
  c.valid_until = now + valid_s;
  c.gives = gives;
  codes.push_back(std::move(c));
  if (codes.size() > kMaxGuestCodes) codes.erase(codes.begin(), codes.end() - static_cast<std::ptrdiff_t>(kMaxGuestCodes));
  if (!save_codes(store, codes)) return {};
  return code;
}

std::optional<limit> redeem_guest_code(secrets::store& store, std::string_view code, std::int64_t now) {
  if (code.empty() || code.size() > 128 || sodium_init() < 0) return std::nullopt;
  std::vector<stored_code> codes;
  if (!load_codes(store, codes)) return std::nullopt;
  std::erase_if(codes, [&](const stored_code& c) { return c.valid_until <= now; });
  const auto h = code_hash(code);
  for (auto it = codes.begin(); it != codes.end(); ++it) {
    if (sodium_memcmp(it->hash.data(), h.data(), h.size()) == 0) {
      limit gives = it->gives;
      codes.erase(it);
      if (!save_codes(store, codes)) return std::nullopt;  // a code that could be spent twice is no use
      return gives;
    }
  }
  return std::nullopt;
}

}  // namespace mv::driftwire::serve::grants
